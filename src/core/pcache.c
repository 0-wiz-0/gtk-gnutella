/*
 * $Id$
 *
 * Copyright (c) 2001-2003, Raphael Manfredi
 *
 *----------------------------------------------------------------------
 * This file is part of gtk-gnutella.
 *
 *  gtk-gnutella is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  gtk-gnutella is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with gtk-gnutella; if not, write to the Free Software
 *  Foundation, Inc.:
 *      59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *----------------------------------------------------------------------
 */

/**
 * @file
 *
 * Pong caching (LimeWire's ping/pong reducing scheme).
 */

#include "common.h"

RCSID("$Id$");

#include "sockets.h"
#include "hosts.h"
#include "hcache.h"
#include "pcache.h"
#include "nodes.h"
#include "share.h" /* For shared_files_scanned() and shared_kbytes_scanned(). */
#include "routing.h"
#include "gmsg.h"
#include "alive.h"
#include "inet.h"
#include "gnet_stats.h"
#include "hostiles.h"
#include "settings.h"
#include "udp.h"

#include "if/gnet_property_priv.h"

#include "lib/atoms.h"
#include "lib/endian.h"
#include "lib/walloc.h"
#include "lib/override.h"	/* Must be the last header included */

#define PCACHE_MAX_FILES	10000000	/* Arbitrarily large file count */

/***
 *** Messages
 ***/

/**
 * Sends a ping to given node, or broadcast to everyone if `n' is NULL.
 */
static void
send_ping(struct gnutella_node *n, guint8 ttl)
{
	struct gnutella_msg_init *m;

	m = build_ping_msg(NULL, ttl);

	if (n) {
		if (NODE_IS_WRITABLE(n)) {
			n->n_ping_sent++;
			gmsg_sendto_one(n, (gchar *) m, sizeof(*m));
		}
	} else {
		const GSList *sl_nodes = node_all_nodes();
		const GSList *sl;

		/*
		 * XXX Have to loop to count pings sent.
		 * XXX Need to do that more generically, to factorize code.
		 */

		for (sl = sl_nodes; sl; sl = g_slist_next(sl)) {
			n = (struct gnutella_node *) sl->data;
			if (!NODE_IS_WRITABLE(n))
				continue;
			n->n_ping_sent++;
		}

		gmsg_sendto_all(sl_nodes, (gchar *) m, sizeof(*m));
	}
}

/**
 * Build ping message, bearing given TTL and MUID.
 * By construction, hops=0 for all pings.
 * If the MUID is NULL, a random one is assigned.
 *
 * @return pointer to static data
 */
struct gnutella_msg_init *
build_ping_msg(const gchar *muid, guint8 ttl)
{
	static struct gnutella_msg_init m;

	g_assert(ttl);

	if (muid)
		memcpy(&m.header, muid, 16);
	else
		message_set_muid(&m.header, GTA_MSG_INIT);

	m.header.function = GTA_MSG_INIT;
	m.header.ttl = ttl;
	m.header.hops = 0;

	WRITE_GUINT32_LE(0, m.header.size);

	return &m;
}

/**
 * Build pong message, returns pointer to static data.
 */
struct gnutella_msg_init_response *
build_pong_msg(
	guint8 hops, guint8 ttl, const gchar *muid,
	guint32 ip, guint16 port, guint32 files, guint32 kbytes)
{
	static struct gnutella_msg_init_response pong;

	pong.header.function = GTA_MSG_INIT_RESPONSE;
	pong.header.hops = hops;
	pong.header.ttl = ttl;
	memcpy(&pong.header.muid, muid, 16);

	WRITE_GUINT16_LE(port, pong.response.host_port);
	WRITE_GUINT32_BE(ip, pong.response.host_ip);
	WRITE_GUINT32_LE(files, pong.response.files_count);
	WRITE_GUINT32_LE(kbytes, pong.response.kbytes_count);
	WRITE_GUINT32_LE(sizeof(struct gnutella_init_response), pong.header.size);

	return &pong;
}

/**
 * Send pong message back to node.
 * If `control' is true, send it as a higher priority message.
 */
static void
send_pong(
	struct gnutella_node *n, gboolean control,
	guint8 hops, guint8 ttl, gchar *muid,
	guint32 ip, guint16 port, guint32 files, guint32 kbytes)
{
	struct gnutella_msg_init_response *r;

	g_assert(ttl >= 1);

	if (!NODE_IS_WRITABLE(n))
		return;

	r = build_pong_msg(hops, ttl, muid, ip, port, files, kbytes);
	n->n_pong_sent++;

	if (NODE_IS_UDP(n))
		udp_send_reply(n, r, sizeof(*r));
	else if (control)
		gmsg_ctrl_sendto_one(n, (gchar *) r, sizeof(*r));
	else
		gmsg_sendto_one(n, (gchar *) r, sizeof(*r));
}

/**
 * Send info about us back to node, using the hopcount information present in
 * the header of the node structure to construct the TTL of the pong we
 * send.
 *
 * If `control' is true, send it as a higher priority message.
 */
static void
send_personal_info(struct gnutella_node *n, gboolean control)
{
	guint32 kbytes;
	guint32 files;

	g_assert(n->header.function == GTA_MSG_INIT);	/* Replying to a ping */

	files = MIN(shared_files_scanned(), ~((guint32) 0U));

	/*
	 * Mark pong if we are an ultra node: the amount of kbytes scanned must
	 * be an exact power of two, and at minimum 8.
	 */

	kbytes = MIN(shared_kbytes_scanned(), ~((guint32) 0U));

	if (current_peermode == NODE_P_ULTRA) {
		if (kbytes <= 8)
			kbytes = 8;
		else
			kbytes = next_pow2(kbytes);
	} else if (kbytes)
		kbytes |= 1;		/* Ensure not a power of two */

	/*
	 * Pongs are sent with a TTL just large enough to reach the pinging host,
	 * up to a maximum of max_ttl.	Note that we rely on the hop count being
	 * accurate.
	 *				--RAM, 15/09/2001
	 */

	send_pong(n, control, 0, MIN((guint) n->header.hops + 1, max_ttl),
		n->header.muid, listen_ip(), listen_port, files, kbytes);
}

/**
 * Send a pong for each of our connected neighbours to specified node.
 */
static void
send_neighbouring_info(struct gnutella_node *n)
{
	const GSList *sl;

	g_assert(n->header.function == GTA_MSG_INIT);	/* Replying to a ping */
	g_assert(n->header.hops == 0);					/* Originates from node */
	g_assert(n->header.ttl == 2);					/* "Crawler" ping */

	for (sl = node_all_nodes(); sl; sl = g_slist_next(sl)) {
		struct gnutella_node *cn = (struct gnutella_node *) sl->data;

		if (!NODE_IS_WRITABLE(cn))
			continue;

		/*
		 * If we have valid Gnet information for the node, build the pong
		 * as if it came from the neighbour, only we don't send the ping,
		 * and don't have to read back the pong and resent it.
		 *
		 * Otherwise, don't send anything back: we no longer keep routing
		 * information for pings.
		 */

		if (cn->gnet_ip == 0)
			continue;				/* No information yet */

		send_pong(n, FALSE,
			1, 1, n->header.muid,			/* hops = 1, TTL = 1 */
			cn->gnet_ip, cn->gnet_port,
			cn->gnet_files_count, cn->gnet_kbytes_count);

		/*
		 * Since we won't see the neighbour pong, we won't be able to store
		 * it in our reserve, so do it from here.
		 */

		if (!NODE_IS_LEAF(n))
			host_add(cn->gnet_ip, cn->gnet_port, FALSE);

		/*
		 * Node can be removed should its send queue saturate.
		 */

		if (!NODE_IS_CONNECTED(n))
			return;
	}
}

/***
 *** Ping/pong reducing scheme.
 ***/

/*
 * Data structures used:
 *
 * `pong_cache' is an array of MAX_CACHE_HOPS+1 entries.
 * Each entry is a structure holding a one-way list and a traversal pointer
 * so we may iterate over the list of cached pongs at that hop level.
 *
 * `cache_expire_time' is the time after which we will expire the whole cache
 * and ping all our connections.
 */

static time_t pcache_expire_time = 0;

struct cached_pong {		/* A cached pong */
	gint refcount;			/* How many lists reference us? */
	guint32 node_id;		/* The node ID from which we got that pong */
	guint32 last_sent_id;	/* Node ID to which we last sent this pong */
	guint32 ip;				/* Values from the pong message */
	guint32 port;
	guint32 files_count;
	guint32 kbytes_count;
};

struct cache_line {			/* A cache line for a given hop value */
	gint hops;				/* Hop count of this cache line */
	GSList *pongs;			/* List of cached_pong */
	GSList *cursor;			/* Cursor within list: last item traversed */
};

struct recent {
	GHashTable *ht_recent_pongs;	/* Recent pongs we know about */
	GList *recent_pongs;			/* Recent pongs we got */
	GList *last_returned_pong;		/* Last returned from list */
	gint recent_pong_count;			/* # of pongs in recent list */
};

#define PONG_CACHE_SIZE		(MAX_CACHE_HOPS+1)

static struct cache_line pong_cache[PONG_CACHE_SIZE];
static struct recent recent_pongs[HOST_MAX];

#define CACHE_UP_LIFESPAN	5		/* seconds -- ultra/normal mode */
#define CACHE_LEAF_LIFESPAN	120		/* seconds -- leaf mode */
#define MAX_PONGS			10		/* Max pongs returned per ping */
#define OLD_PING_PERIOD		45		/* Pinging period for "old" clients */
#define OLD_CACHE_RATIO		20		/* % of pongs from "old" clients we cache */
#define RECENT_PING_SIZE	50		/* remember last 50 pongs we saw */
#define MIN_UP_PING			3		/* ping at least 3 neighbours */
#define UP_PING_RATIO		20		/* ping 20% of UP, at random */

#define cache_lifespan(m)	\
	((m) == NODE_P_LEAF ? CACHE_LEAF_LIFESPAN : CACHE_UP_LIFESPAN)

/*
 * cached_pong_hash
 * cached_pong_eq
 *
 * Callbacks for the `ht_recent_pongs' hash table.
 */

static guint
cached_pong_hash(gconstpointer key)
{
	const struct cached_pong *cp = (const struct cached_pong *) key;

	return (guint) (cp->ip ^ ((cp->port << 16) | cp->port));
}
static gint
cached_pong_eq(gconstpointer v1, gconstpointer v2)
{
	const struct cached_pong *h1 = (const struct cached_pong *) v1;
	const struct cached_pong *h2 = (const struct cached_pong *) v2;

	return h1->ip == h2->ip && h1->port == h2->port;
}

/**
 * Initialization.
 */
void
pcache_init(void)
{
	gint h;

	memset(pong_cache, 0, sizeof(pong_cache));
	memset(recent_pongs, 0, sizeof(recent_pongs));

	for (h = 0; h < PONG_CACHE_SIZE; h++)
		pong_cache[h].hops = h;

	recent_pongs[HOST_ANY].ht_recent_pongs =
		g_hash_table_new(cached_pong_hash, cached_pong_eq);

	recent_pongs[HOST_ULTRA].ht_recent_pongs =
		g_hash_table_new(cached_pong_hash, cached_pong_eq);
}

/**
 * Free cached pong when noone references it any more.
 */
static void
free_cached_pong(struct cached_pong *cp)
{
	g_assert(cp->refcount > 0);		/* Someone was referencing it */

	if (--(cp->refcount) != 0)
		return;

	wfree(cp, sizeof(*cp));
}


/**
 * Get a recent pong from the list, updating `last_returned_pong' as we
 * go along, so that we never return twice the same pong instance.
 *
 * Fills `ip' and `port' with the pong value and return TRUE if we
 * got a pong.  Otherwise return FALSE.
 */
gboolean
pcache_get_recent(host_type_t type, guint32 *ip, guint16 *port)
{
	static guint32 last_ip = 0;
	static guint16 last_port = 0;
	GList *l;
	struct cached_pong *cp;
	struct recent *rec;

	g_assert((guint) type < HOST_MAX);

	rec = &recent_pongs[type];

	if (!rec->recent_pongs)		/* List empty */
		return FALSE;

	/*
	 * If `last_returned_pong' is NULL, it means we reached the head
	 * of the list, so we traverse faster than we get pongs.
	 *
	 * Try with the head of the list, because maybe we have a recent pong
	 * there, but if it is the same as the last ip/port we returned, then
	 * go back to the tail of the list.
	 */

	if (rec->last_returned_pong == NULL) {
		l = g_list_first(rec->recent_pongs);
		cp = (struct cached_pong *) l->data;

		if (cp->ip != last_ip || cp->port != last_port)
			goto found;

		if (g_list_next(l) == NULL)		/* Head is the only item in list */
			return FALSE;
	} else {
		/* Regular case */
		l = g_list_previous(rec->last_returned_pong);
		for (/* empty */ ; l; l = g_list_previous(l)) {
			cp = (struct cached_pong *) l->data;
			if (cp->ip != last_ip || cp->port != last_port)
				goto found;
		}
	}

	/*
	 * Still none found, go back to the end of the list.
	 */

	for (l = g_list_last(rec->recent_pongs); l; l = g_list_previous(l)) {
		cp = (struct cached_pong *) l->data;
		if (cp->ip != last_ip || cp->port != last_port)
			goto found;
	}

	return FALSE;

found:
	rec->last_returned_pong = l;
	*ip = last_ip = cp->ip;
	*port = last_port = cp->port;

	if (dbg > 8)
		printf("returning recent %s PONG %s\n",
			host_type_to_gchar(type), ip_port_to_gchar(cp->ip, cp->port));

	return TRUE;
}

/**
 * Add recent pong to the list, handled as a FIFO cache, if not already
 * present.
 */
static void
add_recent_pong(host_type_t type, struct cached_pong *cp)
{
	struct recent *rec;

	g_assert((gint) type >= 0 && type < HOST_MAX);

	rec = &recent_pongs[type];

    if (
        !host_is_valid(cp->ip, cp->port) || 
        (NULL != g_hash_table_lookup(
            rec->ht_recent_pongs, (gconstpointer) cp)) ||
        hcache_node_is_bad(cp->ip)
    ) {
        return;
    }

	if (rec->recent_pong_count == RECENT_PING_SIZE) {		/* Full */
		GList *lnk = g_list_last(rec->recent_pongs);
		struct cached_pong *cp = (struct cached_pong *) lnk->data;

		rec->recent_pongs = g_list_remove_link(rec->recent_pongs, lnk);
		g_hash_table_remove(rec->ht_recent_pongs, cp);

		if (lnk == rec->last_returned_pong)
			rec->last_returned_pong = g_list_previous(rec->last_returned_pong);

		free_cached_pong(cp);
		g_list_free_1(lnk);
	} else {
		rec->recent_pong_count++;
    }
	
	rec->recent_pongs = g_list_prepend(rec->recent_pongs, cp);
	g_hash_table_insert(rec->ht_recent_pongs, cp, (gpointer) 1);
	cp->refcount++;		/* We don't refcount insertion in the hash table */
}

/**
 * Determine the pong type (any, or of the ultra kind).
 */
static host_type_t
pong_type(struct gnutella_init_response *pong)
{
	guint32 kbytes;

	READ_GUINT32_LE(pong->kbytes_count, kbytes);

	/*
	 * Ultra pongs are marked by having their kbytes count be an
	 * exact power of two, and greater than 8.
	 */

	return (kbytes >= 8 && is_pow2(kbytes)) ? HOST_ULTRA : HOST_ANY;
}

/**
 * Clear the whole recent pong list.
 */
void
pcache_clear_recent(host_type_t type)
{
	GList *l;
	struct recent *rec;

	g_assert((gint) type >= 0 && type < HOST_MAX);

	rec = &recent_pongs[type];

	for (l = rec->recent_pongs; l; l = g_list_next(l)) {
		struct cached_pong *cp = (struct cached_pong *) l->data;

		g_hash_table_remove(rec->ht_recent_pongs, cp);
		free_cached_pong(cp);
	}

	g_list_free(rec->recent_pongs);
	rec->recent_pongs = NULL;
	rec->last_returned_pong = NULL;
	rec->recent_pong_count = 0;
}

/**
 * Called when a new outgoing connection has been made.
 *
 * + If we need a connection, or have less than MAX_PONGS entries in our caught
 *   list, send a ping at normal TTL value.
 * + Otherwise, send a handshaking ping with TTL=1
 */
void
pcache_outgoing_connection(struct gnutella_node *n)
{
	g_assert(NODE_IS_CONNECTED(n));

	if (connected_nodes() < up_connections || hcache_is_low(HOST_ANY))
		send_ping(n, my_ttl);		/* Regular ping, get fresh pongs */
	else
		send_ping(n, 1);			/* Handshaking ping */
}

/**
 * Expire the whole cache.
 */
static void
pcache_expire(void)
{
	gint i;
	gint entries = 0;

	for (i = 0; i < PONG_CACHE_SIZE; i++) {
		struct cache_line *cl = &pong_cache[i];
		GSList *sl;

		for (sl = cl->pongs; sl; sl = g_slist_next(sl)) {
			entries++;
			free_cached_pong((struct cached_pong *) sl->data);
		}
		g_slist_free(cl->pongs);

		cl->pongs = NULL;
		cl->cursor = NULL;
	}

	if (dbg > 4)
		printf("Pong CACHE expired (%d entr%s, %d in reserve)\n",
			entries, entries == 1 ? "y" : "ies", hcache_size(HOST_ANY));
}

/**
 * Final shutdown.
 */
void
pcache_close(void)
{
	static host_type_t types[] = { HOST_ANY, HOST_ULTRA };
	guint i;

	pcache_expire();

	for (i = 0; i < G_N_ELEMENTS(types); i++) {
		host_type_t type = types[i];

		pcache_clear_recent(type);
		g_hash_table_destroy(recent_pongs[type].ht_recent_pongs);
	}
}

/**
 * Send a ping to all "new" clients to which we are connected, and one to
 * older client if and only if at least OLD_PING_PERIOD seconds have
 * elapsed since our last ping, as determined by `next_ping'.
 */
static void
ping_all_neighbours(time_t now)
{
	const GSList *sl;
	GSList *may_ping = NULL;
	GSList *to_ping = NULL;
	gint ping_cnt = 0;
	gint selected = 0;
	gint left;

	/*
	 * Because nowadays the network has a higher outdegree for ultrapeers,
	 * and because of the widespread use of X-Try-Ultrapeers headers, it is
	 * less critical to use pings as a way to collect hosts.
	 *
	 * Therefore, don't ping all neighbours but only UP_PING_RATIO percent
	 * of them, chosen at random, with at least MIN_UP_PING hosts chosen.
	 *
	 *		--RAM, 12/01/2004
	 */

	for (sl = node_all_nodes(); sl; sl = g_slist_next(sl)) {
		struct gnutella_node *n = (struct gnutella_node *) sl->data;

		if (!NODE_IS_WRITABLE(n) || NODE_IS_LEAF(n))
			continue;

		/*
		 * If node is in TX flow control, we already have problems,
		 * so don't increase them by sending more pings.
		 *		--RAM, 19/06/2003
		 */

		if (NODE_IN_TX_FLOW_CONTROL(n))
			continue;

		if ((n->attrs & NODE_A_PONG_CACHING) || now > n->next_ping) {
			may_ping = g_slist_prepend(may_ping, n);
			ping_cnt++;
		}
	}

	for (sl = may_ping, left = ping_cnt; sl; sl = g_slist_next(sl), left--) {
		struct gnutella_node *n = (struct gnutella_node *) sl->data;

		if (
			ping_cnt <= MIN_UP_PING ||
			(selected < MIN_UP_PING && left <= (MIN_UP_PING - selected)) ||
			random_value(99) < UP_PING_RATIO 
		) {
			to_ping = g_slist_prepend(to_ping, n);
			selected++;
		}
	}

	for (sl = to_ping; sl; sl = g_slist_next(sl)) {
		struct gnutella_node *n = (struct gnutella_node *) sl->data;

		if (!(n->attrs & NODE_A_PONG_CACHING))
			n->next_ping = now + OLD_PING_PERIOD;

		send_ping(n, my_ttl);
	}

	g_slist_free(may_ping);
	g_slist_free(to_ping);
}

/**
 * Check pong cache for expiration.
 * If expiration time is reached, flush it and ping all our neighbours.
 */
void
pcache_possibly_expired(time_t now)
{
	if (delta_time(now, pcache_expire_time) >= 0) {
		pcache_expire();
		pcache_expire_time = now + cache_lifespan(current_peermode);
		ping_all_neighbours(now);
	}
}

/**
 * Called when peer mode is changed to recompute the pong cache lifetime.
 */
void
pcache_set_peermode(node_peer_t mode)
{
	pcache_expire_time = time(NULL) + cache_lifespan(mode);
}

/**
 * Fill ping_guid[] and pong_needed[] arrays in the node from which we just
 * accepted a ping.
 *
 * When we accept a ping from a connection, we don't really relay the ping.
 * Our cache is filled by the pongs we receive back from our periodic
 * pinging of the neighbours.
 *
 * However, when we get some pongs back, we forward them back to the nodes
 * for which we have accepted a ping and which still need results, as
 * determined by pong_needed[] (index by pong hop count).  The saved GUID
 * of the ping allows us to fake the pong reply, so the sending node recognizes
 * those as being "his" pongs.
 */
static void
setup_pong_demultiplexing(struct gnutella_node *n, guint8 ttl)
{
	gint remains;
	gint h;

	g_assert(n->header.function == GTA_MSG_INIT);

	memcpy(n->ping_guid, n->header.muid, 16);
	memset(n->pong_needed, 0, sizeof(n->pong_needed));
	n->pong_missing = 0;

	/*
	 * `ttl' is currently the amount of hops the ping could travel.
	 * If it's 1, it means it would have travelled on host still, and we
	 * would have got a pong back with an hop count of 0.
	 *
	 * Since our pong_needed[] array is indexed by the hop count of pongs,
	 * we need to substract one from the ttl parameter.
	 */

	if (ttl-- == 0)
		return;

	ttl = MIN(ttl, MAX_CACHE_HOPS);		/* We limit the maximum hop count */

	/*
	 * Now we're going to distribute "evenly" the MAX_PONGS we can return
	 * to this ping accross the (0..ttl) range.  We start by the beginning
	 * of the array to give more weight to high-hops pongs.
	 */

	n->pong_missing = remains = MAX_PONGS;

	for (h = 0; h <= MAX_CACHE_HOPS; h++) {
		guchar amount = (guchar) (remains / (MAX_CACHE_HOPS + 1 - h));
		n->pong_needed[h] = amount;
		remains -= amount;
		if (dbg > 7)
			printf("pong_needed[%d] = %d, remains = %d\n", h, amount, remains);
	}

	g_assert(remains == 0);
}

/**
 * Internal routine for send_cached_pongs.
 *
 * Iterates on a list of cached pongs and send back any pong to node `n'
 * that did not originate from it.  Update `cursor' in the cached line
 * to be the address of the last traversed item.
 *
 * Return FALSE if we're definitely done, TRUE if we can still iterate.
 */
static gboolean
iterate_on_cached_line(
	struct gnutella_node *n, struct cache_line *cl, guint8 ttl,
	GSList *start, GSList *end, gboolean strict)
{
	gint hops = cl->hops;
	GSList *sl;

	sl = start;
	for (; sl && sl != end && n->pong_missing; sl = g_slist_next(sl)) {
		struct cached_pong *cp = (struct cached_pong *) sl->data;

		cl->cursor = sl;

		/*
		 * We never send a cached pong to the node from which it came along.
		 *
		 * The `last_sent_id' trick is used because we're going to iterate
		 * twice on the cache list: once to send pongs that strictly match
		 * the hop counts needed, and another time to send pongs as needed,
		 * more loosely.  The two runs are consecutive, so we're saving in
		 * each cached entry the node to which we sent it last, so we don't
		 * resend the same pong twice.
		 *
		 * We're only iterating upon reception of the intial ping from the
		 * node.  After that, we'll send pongs as we receive them, and
		 * only if they strictly match the needed TTL.
		 */

		if (n->id == cp->node_id)
			continue;
		if (n->id == cp->last_sent_id)
			continue;
		cp->last_sent_id = n->id;

		/*
		 * When sending a cached pong, don't forget that its cached hop count
		 * is the one we got when we received it, i.e. hops=0 means a pong
		 * from one of our immediate neighbours.  However, we're now "routing"
		 * it, so we must increase the hop count.
		 */

		g_assert(hops < 255);		/* Because of MAX_CACHE_HOPS */

		send_pong(n, FALSE, hops + 1, ttl, n->ping_guid,
			cp->ip, cp->port, cp->files_count, cp->kbytes_count);

		n->pong_missing--;

		if (dbg > 7)
			printf("iterate: sent cached pong %s (hops=%d, TTL=%d) to %s, "
				"missing=%d %s\n", ip_port_to_gchar(cp->ip, cp->port),
				hops, ttl, node_ip(n), n->pong_missing,
				strict ? "STRICT" : "loose");

		if (strict && --(n->pong_needed[hops]) == 0)
			return FALSE;

		/*
		 * Node can be removed should its send queue saturate.
		 */

		if (!NODE_IS_CONNECTED(n))
			return FALSE;
	}

	return n->pong_missing != 0;
}

/**
 * Send pongs from cache line back to node `n' if more are needed for this
 * hop count and they are not originating from the node.  When `strict'
 * is false, we send even if no pong at that hop level is needed.
 */
static void
send_cached_pongs(
	struct gnutella_node *n,
	struct cache_line *cl, guint8 ttl, gboolean strict)
{
	gint hops = cl->hops;
	GSList *old = cl->cursor;

	if (strict && !n->pong_needed[hops])
		return;

	/*
	 * We start iterating after `cursor', until the end of the list, at which
	 * time we restart from the beginning until we reach `cursor', included.
	 * When we leave, `cursor' will point to the last traversed item.
	 */

	if (old) {
		if (!iterate_on_cached_line(n, cl, ttl, old->next, NULL, strict))
			return;
		(void) iterate_on_cached_line(n, cl, ttl, cl->pongs, old->next, strict);
	} else
		(void) iterate_on_cached_line(n, cl, ttl, cl->pongs, NULL, strict);
}

/**
 * We received a pong we cached from `n'.  Send it to all other nodes if
 * they need one at this hop count.
 */
static void
pong_all_neighbours_but_one(
	struct gnutella_node *n, struct cached_pong *cp, host_type_t ptype,
	guint8 hops, guint8 ttl)
{
	const GSList *sl;

	for (sl = node_all_nodes(); sl; sl = g_slist_next(sl)) {
		struct gnutella_node *cn = (struct gnutella_node *) sl->data;

		if (cn == n)
			continue;

		if (!NODE_IS_WRITABLE(cn))
			continue;

		/*
		 * Since we iterate twice initially at ping reception, once strictly
		 * and the other time loosly, `pong_missing' is always accurate but
		 * can be different from the sum of `pong_needed[i]', for all `i'.
		 */

		if (!cn->pong_missing)
			continue;

		if (!cn->pong_needed[hops])
			continue;

		/*
		 * If node is a leaf node, we can only send it Ultra pongs.
		 */

		if (NODE_IS_LEAF(cn) && ptype != HOST_ULTRA)
			continue;

		cn->pong_missing--;
		cn->pong_needed[hops]--;

		/*
		 * When sending a cached pong, don't forget that its cached hop count
		 * is the one we got when we received it, i.e. hops=0 means a pong
		 * from one of our immediate neighbours.  However, we're now "routing"
		 * it, so we must increase the hop count.
		 */

		g_assert(hops < 255);

		send_pong(cn, FALSE, hops + 1, ttl, cn->ping_guid,
			cp->ip, cp->port, cp->files_count, cp->kbytes_count);

		if (dbg > 7)
			printf("pong_all: sent cached pong %s (hops=%d, TTL=%d) to %s "
				"missing=%d\n", ip_port_to_gchar(cp->ip, cp->port),
				hops, ttl, node_ip(cn), cn->pong_missing);
	}
}

/**
 * We received an ultra pong.
 * Send it to one randomly selected leaf, which is not already missing pongs.
 */
static void
pong_random_leaf(struct cached_pong *cp, guint8 hops, guint8 ttl)
{
	const GSList *sl;
	gint leaves;
	struct gnutella_node *leaf = NULL;

	g_assert(current_peermode == NODE_P_ULTRA);

	for (sl = node_all_nodes(), leaves = 0; sl; sl = g_slist_next(sl)) {
		struct gnutella_node *cn = (struct gnutella_node *) sl->data;
		gint threshold;

		if (cn->pong_missing)	/* A job for pong_all_neighbours_but_one() */
			continue;

		if (!NODE_IS_LEAF(cn))
			continue;

		if (NODE_IN_TX_FLOW_CONTROL(cn))	/* Already overwhelmed */
			continue;

		/*
		 * Randomly select one leaf.
		 *
		 * As we go along, the probability that we retain the current leaf
		 * decreases.  It is 1 for the first leaf, 1/2 for the second leaf,
		 * 1/3 for the third leaf, etc...
		 */

		leaves++;
		threshold = (gint) (1000.0 / leaves);

		if ((gint) random_value(999) < threshold)
			leaf = cn;
	}

	/*
	 * Send the pong to the selected leaf, if any.
	 *
	 * NB: If the leaf never sent a ping before, leaf->ping_guid will
	 * be a zero GUID.  That's OK.
	 */

	if (leaf != NULL) {
		send_pong(leaf, FALSE, hops + 1, ttl, leaf->ping_guid,
			cp->ip, cp->port, cp->files_count, cp->kbytes_count);

		if (dbg > 7)
			printf("pong_random_leaf: sent pong %s (hops=%d, TTL=%d) to %s\n",
				ip_port_to_gchar(cp->ip, cp->port), hops, ttl, node_ip(leaf));
	}
}

/**
 * Add pong from node `n' to our cache of recent pongs.
 * Returns the cached pong object.
 */
static struct cached_pong *
record_fresh_pong(
	host_type_t type,
	struct gnutella_node *n,
	guint8 hops, guint32 ip, guint16 port,
	guint32 files_count, guint32 kbytes_count)
{
	struct cache_line *cl;
	struct cached_pong *cp;
	guint8 hop;


	g_assert((gint) type >= 0 && type < HOST_MAX);

	cp = (struct cached_pong *) walloc(sizeof(struct cached_pong));

	cp->refcount = 1;
	cp->node_id = n->id;
	cp->last_sent_id = n->id;
	cp->ip = ip;
	cp->port = port;
	cp->files_count = files_count;
	cp->kbytes_count = kbytes_count;

	hop = CACHE_HOP_IDX(hops);		/* Trim high values to MAX_CACHE_HOPS */
	cl = &pong_cache[hop];
	cl->pongs = g_slist_append(cl->pongs, cp);
	add_recent_pong(type, cp);

	return cp;
}

/**
 * Called when an UDP ping is received.
 */
static void
pcache_udp_ping_received(struct gnutella_node *n)
{
	g_assert(NODE_IS_UDP(n));

	/*
	 * If we got a PING whose MUID is our node's GUID, then it's a reply
	 * to our "UDP Connect Back" message.  Ignore it, we've already
	 * noticed that we got an unsolicited UDP message.
	 */

	if (guid_eq(guid, n->header.muid)) {
		if (udp_debug > 19)
			printf("UDP got unsolicited PING matching our GUID!\n");
		return;
	}

	send_personal_info(n, FALSE);
}

/**
 * Called when a ping is received from a node.
 *
 * + If current time is less than what `ping_accept' says, drop the ping.
 *   Otherwise, accept the ping and increment `ping_accept' by n->ping_throttle.
 * + If cache expired, call pcache_expire() and broadcast a new ping to all
 *   the "new" clients (i.e. those flagged NODE_A_PONG_CACHING).  For "old"
 *   clients, do so only if "next_ping" time was reached.
 * + Handle "alive" pings (TTL=1) and "crawler" pings (TTL=2) immediately,
 *   then return.
 * + Setup pong demultiplexing tables, recording the fact that  the node needs
 *   to be sent pongs as we receive them.
 * + Return a pong for us if we accept incoming connections right now.
 * + Return cached pongs, avoiding to resend a pong coming from that node ID.
 */
void
pcache_ping_received(struct gnutella_node *n)
{
	time_t now = time((time_t *) 0);
	gint h;
	guint8 ttl;

	g_assert(NODE_IS_CONNECTED(n));

	if (NODE_IS_UDP(n)) {
		pcache_udp_ping_received(n);
		return;
	}

	/*
	 * Handle "alive" pings and "crawler" pings specially.
	 * Besides, we always accept them.
	 *
	 * If we get a TTL=0 ping, assume it's used to ack an "alive ping" we
	 * sent earlier.  Don't event log we got a message with TTL=0, we're
	 * getting way too many of them and nobody on the GDF seems to care.
	 * BearShare is known to do this, and they admitted it publicly like
	 * it was a good idea!
	 *
	 *		--RAM, 2004-08-09
	 */

	if (n->header.hops == 0 && n->header.ttl <= 2) {
		n->n_ping_special++;
		n->n_ping_accepted++;

		if (n->header.ttl == 1)
			send_personal_info(n, TRUE);	/* Control message, prioritary */
		else if (n->header.ttl == 2) {
			if (current_peermode != NODE_P_LEAF)
				send_neighbouring_info(n);
		} else
			alive_ack_first(n->alive_pings, n->header.muid);
		return;
	}

	/*
	 * If we get a ping with hops != 0 from a host that claims to
	 * implement ping/pong reduction, then they are not playing
	 * by the same rules as we are.  Emit a warning.
	 *		--RAM, 03/03/2001
	 */

	if (
		n->header.hops &&
		(n->attrs & (NODE_A_PONG_CACHING|NODE_A_PONG_ALIEN)) ==
			NODE_A_PONG_CACHING
	) {
		if (dbg)
			g_warning("node %s (%s) [%d.%d] claimed ping reduction, "
				"got ping with hops=%d", node_ip(n),
				node_vendor(n),
				n->proto_major, n->proto_minor, n->header.hops);
		n->attrs |= NODE_A_PONG_ALIEN;		/* Warn only once */
	}

	/*
	 * Accept the ping?.
	 */

	if (now < n->ping_accept) {
		n->n_ping_throttle++;		/* Drop the ping */
        gnet_stats_count_dropped(n, MSG_DROP_THROTTLE);
		return;
	} else {
		n->n_ping_accepted++;
		n->ping_accept = now + n->ping_throttle;	/* Drop all until then */
	}

	/*
	 * Purge cache if needed.
	 */

	pcache_possibly_expired(now);

	if (!NODE_IS_CONNECTED(n))		/* Can be removed if send queue is full */
		return;

	/*
	 * If TTL = 0, only us can reply, and we'll do that below in any case..
	 * We call setup_pong_demultiplexing() anyway to reset the pong_needed[]
	 * array.
	 *
	 * A leaf node will not demultiplex pongs, so don't bother.
	 */

	if (current_peermode != NODE_P_LEAF)
		setup_pong_demultiplexing(n, n->header.ttl);

	/*
	 * If we can accept an incoming connection, send a reply.
	 *
	 * If we are firewalled, we nonetheless send a ping
	 * when inet_can_answer_ping() tells us we can, irrespective
	 * of whether we can accept a new node connection: the aim is
	 * to trigger an incoming connection that will prove us we're
	 * not firewalled.
	 *
	 * Finally, we always reply to the first ping we get with our
	 * personal information (reply to initial ping sent after handshake).
	 */

	if (
		n->n_ping_accepted == 1 ||
		((is_firewalled || node_missing() > 0) && inet_can_answer_ping())
	) {
		send_personal_info(n, FALSE);
		if (!NODE_IS_CONNECTED(n))	/* Can be removed if send queue is full */
			return;
	}

	if (current_peermode == NODE_P_LEAF)
		return;

	/*
	 * We continue here only for non-leaf nodes.
	 */

	/*
	 * Return cached pongs if we have some and they are needed.
	 * We first try to send pongs on a per-hop basis, based on pong_needed[].
	 */

	ttl = MIN((guint) n->header.hops + 1, max_ttl);

	for (h = 0; n->pong_missing && h < n->header.ttl; h++) {
		struct cache_line *cl = &pong_cache[CACHE_HOP_IDX(h)];

		if (cl->pongs) {
			send_cached_pongs(n, cl, ttl, TRUE);
			if (!NODE_IS_CONNECTED(n))
				return;
		}
	}

	/*
	 * We then re-iterate if some pongs are still needed, sending any we
	 * did not already send.
	 */

	for (h = 0; n->pong_missing && h < n->header.ttl; h++) {
		struct cache_line *cl = &pong_cache[CACHE_HOP_IDX(h)];

		if (cl->pongs) {
			send_cached_pongs(n, cl, ttl, FALSE);
			if (!NODE_IS_CONNECTED(n))
				return;
		}
	}
}

/**
 * Called when a pong is received from a node.
 *
 * + Record node in the main host catching list.
 * + If node is not a "new" client (i.e. flagged as NODE_A_PONG_CACHING),
 *   cache randomly OLD_CACHE_RATIO percent of those (older clients need
 *   to be able to get incoming connections as well).
 * + Cache pong in the pong.hops cache line, associated with the node ID (so we
 *   never send back this entry to the node).
 * + For all nodes but `n', propagate pong if neeed, with demultiplexing.
 */
void
pcache_pong_received(struct gnutella_node *n)
{
	guint32 ip;
	guint16 port;
	guint32 files_count;
	guint32 kbytes_count;
	guint32 swapped_count;
	struct cached_pong *cp;
	host_type_t ptype;

	n->n_pong_received++;

	if (NODE_IS_UDP(n))
		return;							/* XXX UDP PONG ignored for now */

	/*
	 * Decompile the pong information.
	 */

	READ_GUINT16_LE(n->data, port);
	READ_GUINT32_BE(n->data + 2, ip);
	READ_GUINT32_LE(n->data + 6, files_count);
	READ_GUINT32_LE(n->data + 10, kbytes_count);

	/*
	 * Sanity checks: make sure the files_count is reasonable, or try
	 * to swap it otherwise.  Then try to adjust the kbytes_count if we
	 * fixed the files_count.
	 *		--RAM, 13/07/2004
	 */

	if (files_count > PCACHE_MAX_FILES) {	/* Arbitrarily large constant */
		gboolean fixed = FALSE;

		swapped_count = swap_guint32(files_count);

		if (swapped_count > PCACHE_MAX_FILES) {
			if (dbg && ip == n->ip)
				g_warning("node %s (%s) sent us a pong with "
					"large file count %u (0x%x), dropped",
					node_ip(n), node_vendor(n), files_count, files_count);
			n->rx_dropped++;
			return;
		} else {
			if (dbg && ip == n->ip) g_warning(
				"node %s (%s) sent us a pong with suspect file count %u "
				"(fixed to %u)",
				node_ip(n), node_vendor(n), files_count, swapped_count);
			files_count = swapped_count;
			fixed = TRUE;
		}
		/*
		 * Maybe the kbytes_count is correct if the files_count was?
		 */

		swapped_count = swap_guint32(kbytes_count);

		if (fixed && swapped_count < kbytes_count)
			kbytes_count = swapped_count;		/* Probably wrong as well */
	}

	/*
	 * Handle replies from our neighbours specially
	 */

	if (n->header.hops == 0) {
		/*
		 * For an incoming connection, we might not know the GNet IP address
		 * of the remote node yet (we know the remote endpoint, but it could
		 * be a proxy for a firewalled node).  The information from the pong
		 * may help us fill this gap.
		 */

		if (n->gnet_ip == 0 && (n->flags & NODE_F_INCOMING)) {
			if (ip == n->ip) {
				n->gnet_ip = ip;		/* Signals: we have figured it out */
				n->gnet_port = port;
			} else if (!(n->flags & NODE_F_ALIEN_IP)) {
				if (dbg) g_warning(
					"node %s (%s) sent us a pong for itself with alien IP %s",
					node_ip(n), node_vendor(n), ip_to_gchar(ip));
				n->flags |= NODE_F_ALIEN_IP;	/* Probably firewalled */
			}
		}

		/*
		 * Only record library stats for the node if it is the first pong
		 * we receive from it (likely to be a reply to our handshaking ping)
		 * or if it comes from the node's IP.
		 * Indeed, LimeWire suffers from a bug where it will forward foreign
		 * pongs with hops=0 even though they are not coming from the node.
		 *		--RAM, 11/01/2004.
		 */

		if (n->n_pong_received == 1 || ip == n->gnet_ip) {
			n->gnet_files_count = files_count;
			n->gnet_kbytes_count = kbytes_count;
		}

		/*
		 * Spot any change in the pong's IP address.  We try to avoid messages
		 * about "connection pongs" by checking whether we have sent at least
		 * 2 pings (one handshaking ping plus one another).
		 */

		if (n->gnet_pong_ip && ip != n->gnet_pong_ip) {
			if (dbg && n->n_ping_sent > 2) g_warning(
				"node %s (%s) sent us a pong for new IP %s (used %s before)",
				node_ip(n), node_vendor(n),
				ip_port_to_gchar(ip, port), ip_to_gchar(n->gnet_pong_ip));
		}

		n->gnet_pong_ip = ip;

		/*
		 * If it was an acknowledge for one of our alive pings, don't cache.
		 */

		if (alive_ack_ping(n->alive_pings, n->header.muid))
			return;
	}

	/*
	 * If it's not a connectible pong, discard it.
	 */

	if (!host_is_valid(ip, port)) {
		gnet_stats_count_dropped(n, MSG_DROP_PONG_UNUSABLE);
		return;
	}

	/*
	 * If pong points to an hostile IP address, discard it.
	 */

	if (hostiles_check(ip)) {
		gnet_stats_count_dropped(n, MSG_DROP_HOSTILE_IP);
		return;
	}

	/*
	 * If pong points to us, maybe we explicitly connected to ourselves
	 * (tests) or someone is trying to fool us.
	 */

	if (ip == listen_ip() && port == listen_port)
		return;

	/*
	 * Add pong to our reserve, and possibly try to connect.
	 */

	host_add(ip, port, TRUE);

	/*
	 * If we got a pong from an "old" client, cache OLD_CACHE_RATIO of
	 * its pongs, randomly.  Returning from this routine means we won't
	 * cache it.
	 */

	if (!(n->attrs & NODE_A_PONG_CACHING)) {
		gint ratio = (gint) random_value(100);
		if (ratio >= OLD_CACHE_RATIO) {
			if (dbg > 7)
				printf("NOT CACHED pong %s (hops=%d, TTL=%d) from OLD %s\n",
					ip_port_to_gchar(ip, port), n->header.hops, n->header.ttl,
					node_ip(n));
			return;
		}
	}

	/*
	 * Insert pong within our cache.
	 */

	ptype = pong_type((struct gnutella_init_response *) n->data);

	cp = record_fresh_pong(HOST_ANY, n, n->header.hops, ip, port,
		files_count, kbytes_count);

	if (ptype == HOST_ULTRA)
		(void) record_fresh_pong(HOST_ULTRA, n, n->header.hops, ip, port,
			files_count, kbytes_count);

	if (dbg > 6)
		printf("CACHED %s pong %s (hops=%d, TTL=%d) from %s %s\n",
			ptype == HOST_ULTRA ? "ultra" : "normal",
			ip_port_to_gchar(ip, port), n->header.hops, n->header.ttl,
			(n->attrs & NODE_A_PONG_CACHING) ? "NEW" : "OLD", node_ip(n));

	/*
	 * Demultiplex pong: send it to all the connections but the one we
	 * received it from, provided they need more pongs of this hop count.
	 */

	if (current_peermode != NODE_P_LEAF)
		pong_all_neighbours_but_one(n,
			cp, ptype, CACHE_HOP_IDX(n->header.hops), MAX(1, n->header.ttl));

	/*
	 * If we're in ultra mode, send 33% of all the ultra pongs we get
	 * to one random leaf.
	 */

	if (
		current_peermode == NODE_P_ULTRA &&
		ptype == HOST_ULTRA && random_value(99) < 33
	)
		pong_random_leaf(
			cp, CACHE_HOP_IDX(n->header.hops), MAX(1, n->header.ttl));
}

/**
 * Fake a pong for a node from which we received an incoming connection,
 * using the supplied IP/port.
 *
 * This pong is not multiplexed to neighbours, but is used to populate our
 * cache, so we can return its address to others, assuming that if it is
 * making an incoming connection to us, it is really in need for other
 * connections as well.
 */
void
pcache_pong_fake(struct gnutella_node *n, guint32 ip, guint16 port)
{
	g_assert(n->attrs & NODE_A_ULTRA);

	if (!host_is_valid(ip, port))
		return;

	host_add(ip, port, FALSE);
	(void) record_fresh_pong(HOST_ULTRA, n, 1, ip, port, 0, 0);
}

/* vi: set ts=4: */

