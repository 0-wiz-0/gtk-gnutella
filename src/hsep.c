/*
 * Copyright (c) 2004, Thomas Schuerger & Jeroen Asselman
 *
 * Horizon Size Estimation Protocol 0.2
 *
 * Protocol is defined here: http://www.menden.org/gnutella/hsep.html
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

/*
 * General API information:
 *
 * - hsep_init() should be called once on startup of GtkG
 * - hsep_connection_init(node) should be called once for each
 *   newly established HSEP-capable connection
 * - hsep_connection_close(node) should be called when a HSEP-capable
 *   connection is closed
 * - hsep_timer() should be called frequently to send out
 *   HSEP messages to HSEP-capable nodes as required
 * - hsep_notify_shared(files, kibibytes) should be called whenever the
 *   number of shared files and/or kibibytes has changed
 * - hsep_process_msg(node) should be called whenever a HSEP message
 *   is received from a HSEP-capable node
 * - hsep_reset() can be used to reset all HSEP data (not for normal use)
 * - hsep_get_global_table(dest, triples) can be used to get the global
 *   HSEP table
 * - hsep_get_connection_table(conn, dest, triples) can be used to get a
 *   per-connection HSEP table
 * - hsep_add_global_table_listener(cb, freqtype, interval) can be used to
 *   add a listener that is informed whenever the global HSEP table changes.
 * - hsep_remove_global_table_listener(cb) can be used to remove an added
 *   listener for global HSEP table changes.
 * - hsep_has_global_table_changed(since) can be used to check if the
 *   global HSEP table has changed since the specified point in time.
 * - hsep_get_non_hsep_triple(tripledest) can be used to determine the
 *   reachable resources contributed by non-HSEP nodes (this is what direct
 *   neighbors that don't support HSEP tell us they're sharing).
 *
 * Obtaining horizon size information on demand:
 *
 * To obtain horizon size information, use the global HSEP table or the
 * per-connection HSEP table, obtained using hsep_get_global_table(...) or
 * hsep_get_connection_table (...), respectively (never access the internal
 * arrays directly). To check if the global table has changed, use
 * hsep_has_global_table_changed(...). The usable array indexes are between 1
 * (for 1 hop) and HSEP_N_MAX (for n_max hops). Note that the arrays only
 * consider other nodes (i.e. exclude what we share ourselves), so the array
 * index 0 always contains zeros. Note also that each triple represents the
 * reachable resources *within* the number of hops, not at *exactly* the number
 * of hops. To get the values for exactly the number of hops, simply subtract
 * the preceeding triple from the desired triple.
 *
 * Obtaining horizon size information using event-driven callbacks (only
 * for the global HSEP table):
 *
 * You can register a callback function for being informed whenever the
 * global HSEP table changes by calling hsep_add_global_table_listener(...).
 * On change of the global HSEP table the callback will be called with a pointer
 * to a copy of the HSEP table and the number of provided triples. You must
 * remove the listener later using hsep_remove_global_table_listener(...).
 *
 * Note: To support exchanging information about clients that don't support
 * HSEP, these clients' library sizes (from PONG messages) are taken into
 * account when HSEP messages are sent (that info is added to what we see
 * in a distance of >= 1 hop).
 */

#include "common.h"

#include "gmsg.h"
#include "routing.h"
#include "nodes.h"
#include "hsep.h"
#include "header.h"
#include "uploads.h"
#include "share.h"

RCSID("$Id$");

#if G_BYTE_ORDER == G_BIG_ENDIAN
#define guint64_to_LE(x)	GUINT64_SWAP_LE_BE(x)
#elif G_BYTE_ORDER == G_LITTLE_ENDIAN
#define guint64_to_LE(x)	x
#else
#error "Byte order not supported"
#endif

/* global HSEP table */
static hsep_triple hsep_global_table[HSEP_N_MAX+1];

/*
 * My own HSEP triple (first value must not be changed, the other must be
 * be updated whenever the number of our shared files/kibibytes change
 * by calling hsep_notify_shared()).
 */

static hsep_triple hsep_own = {1, 0, 0};

static event_t *hsep_global_table_changed_event;
static time_t hsep_last_global_table_change = 0;

/*
 * hsep_init
 *
 * Initializes HSEP.
 */

void hsep_init(void)
{
	header_features_add(&xfeatures.connections,
		"HSEP", HSEP_VERSION_MAJOR, HSEP_VERSION_MINOR);

	memset(hsep_global_table, 0, sizeof(hsep_global_table));

	hsep_global_table_changed_event =
	    event_new("hsep_global_table_changed");

 	hsep_fire_global_table_changed(time(NULL));
}

/*
 * hsep_add_global_table_listener
 *
 * Adds the specified listener to the list of subscribers for
 * global HSEP table change events. The specified callback is
 * called once immediately, independent of the given frequency type
 * and time interval. This function must be called after hsep_init()
 * has been called.
 */

void hsep_add_global_table_listener(GCallback cb, frequency_t t,
                                    guint32 interval)
{
	hsep_triple table[HSEP_N_MAX + 1];

	/* add callback to the event subscriber list */
	event_add_subscriber(hsep_global_table_changed_event, cb, t, interval);

	/*
	 * Fire up the first event to the specified callback. We do it
	 * manually, because we don't want to fire all listeners, but
	 * just the newly added one, and we want it independent of the
	 * given callback call constraints.
	 */

	hsep_get_global_table(table, G_N_ELEMENTS(table));

	(*((hsep_global_listener_t)cb))(table, G_N_ELEMENTS(table));
}

void hsep_remove_global_table_listener(GCallback cb)
{
	event_remove_subscriber(hsep_global_table_changed_event, cb);
}

/*
 * hsep_reset
 *
 * Resets all HSEP data. The global HSEP table and all connections'
 * HSEP tables are reset to zero. The number of own shared files and
 * kibibytes is untouched. This can be used to watch how quickly
 * the HSEP data converges back to the correct "static" state. As soon
 * as we have received a HSEP message from each of our peers, this state
 * should be reached. Use with care, because this reset will temporarily
 * affect all HSEP-capable nodes in the radius of N_MAX hops!
 */

void hsep_reset(void)
{
	int i;
	GSList *sl;

	memset(hsep_global_table, 0, sizeof(hsep_global_table));

	for (sl = (GSList *) node_all_nodes(); sl; sl = g_slist_next(sl)) {
		struct gnutella_node *n = (struct gnutella_node *) sl->data;

		/* also consider unestablished connections here */

		if (!(n->attrs & NODE_A_CAN_HSEP))
			continue;

		memset(n->hsep_table, 0, sizeof(n->hsep_table));
		memset(n->hsep_sent_table, 0, sizeof(n->hsep_sent_table));

		/* this is what we know before receiving the first message */
		
		for (i = 1; i <= HSEP_N_MAX; i++) {
			n->hsep_table[i][HSEP_IDX_NODES] = 1;
			hsep_global_table[i][HSEP_IDX_NODES]++;
		}

		/*
		 * There's no need to reset the last_sent timestamp.
		 * If we'd do this, hsep_timer() would send a message
		 * to all HSEP connections the next time it is called.
		 */
	}
	hsep_fire_global_table_changed(time(NULL));
}

/*
 * hsep_connection_init
 *
 * Initializes the connection's HSEP data.
 */

void hsep_connection_init(struct gnutella_node *n)
{
	int i;

	g_assert(n);

	if (dbg > 1)
		printf("HSEP: Initializing node %p\n", n);
	
	memset(n->hsep_table, 0, sizeof(n->hsep_table));
	memset(n->hsep_sent_table, 0, sizeof(n->hsep_sent_table));

	/* this is what we know before receiving the first message */
		
	for (i = 1; i <= HSEP_N_MAX; i++) {
		n->hsep_table[i][HSEP_IDX_NODES] = 1;
		hsep_global_table[i][HSEP_IDX_NODES]++;
	}

	/*
	 * Initialize counters and timestamps.
	 */

	n->hsep_msgs_received = 0;
	n->hsep_triples_received = 0;
	n->hsep_last_received = 0;
	n->hsep_msgs_sent = 0;
	n->hsep_triples_sent = 0;
	n->hsep_last_sent = 0;

	hsep_sanity_check();

	hsep_fire_global_table_changed(time(NULL));
}

/*
 * hsep_timer
 *
 * Sends a HSEP message to all nodes where the last message
 * has been sent some time ago. This should be called frequently
 * (e.g. every second or every few seconds).
 */

void hsep_timer(time_t now)
{
	GSList *sl;
	gboolean scanning_shared;
	static time_t last_sent = 0;

	/* update number of shared files and KiB */

	gnet_prop_get_boolean_val(PROP_LIBRARY_REBUILDING, &scanning_shared);

	if (!scanning_shared) {
		if (upload_is_enabled())
			hsep_notify_shared(shared_files_scanned(), shared_kbytes_scanned());
		else
			hsep_notify_shared(0, 0);
	}

	for (sl = (GSList *) node_all_nodes(); sl; sl = g_slist_next(sl)) {
		struct gnutella_node *n = (struct gnutella_node *) sl->data;
		int diff;

		/* only consider established connections here */
		if (!NODE_IS_ESTABLISHED(n))
			continue;

		if (!(n->attrs & NODE_A_CAN_HSEP))
			continue;

		/* check how many seconds ago the last message was sent */
		diff = delta_time(now, n->hsep_last_sent);

		/* the -900 is used to react to changes in system time */
		if (diff >= HSEP_MSG_INTERVAL || diff < -900)
			hsep_send_msg(n, now);
	}

	/*
	 * Quick'n dirty hack to update the horizon stats in the
	 * statusbar at least once every 3 seconds.
	 *
	 * TODO: remove this and implement it properly in the
	 * statusbar code.
	 */

	if (delta_time(now, last_sent) >= 3) {
		hsep_fire_global_table_changed(now);
		last_sent = now;
	}
}

/*
 * hsep_connection_close
 *
 * Updates the global HSEP table when a connection is about
 * to be closed. The connection's HSEP data is restored to
 * zero and the CAN_HSEP attribute is cleared.
 */

void hsep_connection_close(struct gnutella_node *n)
{
	unsigned int i;
	guint64 *globalt = (guint64 *) &hsep_global_table[1];
	guint64 *connectiont;

	g_assert(n);

	connectiont = (guint64 *) &n->hsep_table[1];

	if (dbg > 1)
		printf("HSEP: Deinitializing node %p\n", n);

	for (i = 0; i < HSEP_N_MAX; i++) {
		*globalt++ -= *connectiont;
		*connectiont++ = 0;
		*globalt++ -= *connectiont;
		*connectiont++ = 0;
		*globalt++ -= *connectiont;
		*connectiont++ = 0;
	}

	/*
	 * Clear CAN_HSEP attribute so that the HSEP code
	 * will not use the node any longer.
	 */

	n->attrs &= ~NODE_A_CAN_HSEP;

	if (dbg > 1)
		hsep_dump_table();

	hsep_fire_global_table_changed(time(NULL));
}

/*
 * hsep_process_msg
 *
 * Processes a received HSEP message by updating the
 * connection's and the global HSEP table.
 */

void hsep_process_msg(struct gnutella_node *n,time_t now)
{
	unsigned int length;
	unsigned int i, max, msgmax;
	guint64 *messaget;
	guint64 *connectiont;
	guint64 *globalt = (guint64 *) &hsep_global_table[1];

	g_assert(n);

	length = n->size;

	/* note the offset between message and local data by 1 triple */

	messaget = (guint64 *) n->data;
	connectiont = (guint64 *) &n->hsep_table[1];
	
	if (length == 0) {   /* error, at least 1 triple must be present */
		if (dbg > 1)
			printf("HSEP: Node %p sent empty message\n", n);

		return;
	}

	if (length % 24) {   /* error, # of triples not an integer */
		if (dbg > 1)
			printf("HSEP: Node %p sent broken message\n", n);

		return;
	}

	/* get N_MAX of peer servent (other_n_max) */
	msgmax = length / 24;

	if (NODE_IS_LEAF(n) && msgmax > 1) {
		if (dbg > 1) {
			printf("HSEP: Node %p is a leaf, but sent %u triples "
			    "instead of 1\n", n, msgmax);
		}
		return;
	}

	/* truncate if peer servent sent more triples than we need */
	if (msgmax > HSEP_N_MAX)
		max = HSEP_N_MAX;
	else
		max = msgmax;
	
	/*
	 * Convert message from little endian to native byte order.
	 * Only the part of the message we are using is converted.
	 * If native byte order is little endian, do nothing.
	 */

	for (i = max; i > 0; i--) {
		*messaget = guint64_to_LE(*messaget);
		messaget++;
		*messaget = guint64_to_LE(*messaget);
		messaget++;
		*messaget = guint64_to_LE(*messaget);
		messaget++;
	}

	messaget = (guint64 *) n->data;		/* back to front */

	/*
	 * Perform sanity check on received message.
	 */

	if (*messaget != 1) {   /* number of nodes for 1 hop must be 1 */
		if (dbg > 1)
			printf("HSEP: Node %p's message's #nodes for 1 hop is not 1\n", n);

		return;
	}

	if (!hsep_check_monotony((hsep_triple *) messaget, max)) {
		if (dbg > 1)
			printf("HSEP: Node %p's message's monotony check failed\n", n);

		return;
	}

	if (dbg > 1) {
		printf("HSEP: Received %d %s from node %p (msg #%u): ", max,
		    max == 1 ? "triple" : "triples", n, n->hsep_msgs_received + 1);
	}

	/*
	 * Update global and per-connection tables.
	 */

	for (i = 0; i < max; i++) {
		if (dbg > 1)
			printf("(%" PRIu64 ",%" PRIu64 ",%" PRIu64 ") ",
				messaget[0], messaget[1], messaget[2]);
		*globalt++ += *messaget - *connectiont;
		*connectiont++ = *messaget++;
		*globalt++ += *messaget - *connectiont;
		*connectiont++ = *messaget++;
		*globalt++ += *messaget - *connectiont;
		*connectiont++ = *messaget++;
	}

	if (dbg > 1)
		printf("\n");

	/*
	 * If the peer servent sent less triples than we need,
	 * repeat the last triple until we have enough triples
	 */

	for (; i < HSEP_N_MAX; i++) {
		/* go back to previous triple */
		messaget -= 3;

		*globalt++ += *messaget - *connectiont;
		*connectiont++ = *messaget++;
		*globalt++ += *messaget - *connectiont;
		*connectiont++ = *messaget++;
		*globalt++ += *messaget - *connectiont;
		*connectiont++ = *messaget++;
	}

	/*
	 * Update counters and timestamps.
	 */

	n->hsep_msgs_received++;
	n->hsep_triples_received += msgmax;

	n->hsep_last_received = now;

	if (dbg > 1)
		hsep_dump_table();

	hsep_fire_global_table_changed(now);
}

/*
 * hsep_send_msg
 *
 * Sends a HSEP message to the given node, but only if data to send
 * has changed. Should be called about every 30-60 seconds per node.
 * Will automatically be called by hsep_timer() and
 * hsep_connection_init(). Node must be HSEP-capable.
 */

void hsep_send_msg(struct gnutella_node *n,time_t now)
{
	unsigned int i;
	unsigned int msglen;
	unsigned int triples;
	unsigned int opttriples;
	guint64 *globalt = (guint64 *) hsep_global_table;
	guint64 *connectiont;
	guint64 *ownt = (guint64 *) hsep_own;
	guint64 *messaget;
	struct gnutella_msg_hsep_data *m;
	hsep_triple tmp[HSEP_N_MAX];
	hsep_triple other;

	g_assert(n);

	connectiont = (guint64 *) n->hsep_table;
	
	/*
	 * If we are a leaf, we just need to send one triple,
	 * which contains our own data (this triple is expanded
	 * to the needed number of triples on the peer's side).
	 * As the 0'th global and 0'th connection triple are zero,
	 * it contains only our own triple, which is correct.
	 */

	if (current_peermode == NODE_P_LEAF)
		triples = 1;
	else
		triples = G_N_ELEMENTS(tmp);

	/*
	 * Allocate and initialize message to send.
	 */

	msglen = sizeof(struct gnutella_header) + triples * 24;
	m = (struct gnutella_msg_hsep_data *) g_malloc(msglen);

	message_set_muid(&m->header, GTA_MSG_HSEP_DATA);

	m->header.function = GTA_MSG_HSEP_DATA;
	m->header.ttl = 1;
	m->header.hops = 0;

	messaget = (guint64 *) &tmp;

	/*
	 * Collect HSEP data to send and convert the data to
	 * little endian byte order.
	 */

	if (triples > 1) {
		/* determine what we know about non-HSEP nodes in 1 hop distance */
		hsep_get_non_hsep_triple(&other);
	}

	for (i = 0; i < triples; i++) {
		guint64 val;
		val = *ownt++ + *globalt++ - *connectiont++ +
		    (i > 0 ? other[HSEP_IDX_NODES] : 0);
		*messaget++ = guint64_to_LE(val);
		val = *ownt++ + *globalt++ - *connectiont++ +
		    (i > 0 ? other[HSEP_IDX_FILES] : 0);
		*messaget++ = guint64_to_LE(val);
		val = *ownt++ + *globalt++ - *connectiont++ +
		    (i > 0 ? other[HSEP_IDX_KIB] : 0);
		*messaget++ = guint64_to_LE(val);
		ownt -= 3;  /* back to start of own triple */
	}

	memcpy(m->triple, tmp, triples * sizeof tmp[0]);

	/* check if the table differs from the previously sent table */
	if (0 == memcmp(m->triple, n->hsep_sent_table, triples * 24)) {
		G_FREE_NULL(m);
		goto charge_timer;
	}
	
	/*
	 * Note that on big endian architectures the message data is now in
	 * the wrong byte order. Nevertheless, we can use hsep_triples_to_send()
	 * with that data.
	 */
	
	/* optimize number of triples to send */
	opttriples = hsep_triples_to_send((const hsep_triple *) tmp, triples);

	globalt = (guint64 *) hsep_global_table;
	connectiont = (guint64 *) n->hsep_table;

	if (dbg > 1) {
		printf("HSEP: Sending %d %s to node %p (msg #%u): ", opttriples,
		    opttriples == 1 ? "triple" : "triples", n, n->hsep_msgs_sent + 1);
	}

	for (i = 0; i < opttriples; i++) {
		if (dbg > 1) {
			printf("(%" PRIu64 ",%" PRIu64 ",%" PRIu64 ") ",
				ownt[0] + globalt[0] - connectiont[0],
			    ownt[1] + globalt[1] - connectiont[1],
			    ownt[2] + globalt[2] - connectiont[2]);
		}

		globalt += 3;
		connectiont += 3;
	}

	if (dbg > 1)
		printf("\n");

	/* write message size */
	WRITE_GUINT32_LE(opttriples * 24, m->header.size);
	
	/* correct message length */
	msglen = sizeof(struct gnutella_header) + opttriples * 24;

	/* send message to peer node */
	gmsg_sendto_one(n, (gchar *) m, msglen);

	/* store the table for later comparison */
	memcpy((char *) n->hsep_sent_table, tmp, triples * 24);

	G_FREE_NULL(m);

	/*
	 * Update counters.
	 */

	n->hsep_msgs_sent++;
	n->hsep_triples_sent += opttriples;

charge_timer:

	/*
	 * Set the last_sent timestamp to the current time +/- some
	 * random skew.
	 */

	n->hsep_last_sent = now +
		(time_t) random_value(2 * HSEP_MSG_SKEW) - (time_t) HSEP_MSG_SKEW;
}

/*
 * hsep_notify_shared
 *
 * This should be called whenever the number of shared files or kibibytes
 * change. The values are checked for changes, nothing is done if nothing
 * has changed. Note that kibibytes are determined by shifting the number
 * of bytes right by 10 bits, not by dividing by 1000.
 */

void hsep_notify_shared(guint64 ownfiles, guint64 ownkibibytes)
{
	/* check for change */
	if (ownfiles != hsep_own[HSEP_IDX_FILES] ||
		ownkibibytes != hsep_own[HSEP_IDX_KIB]) {
		if (dbg) {
			printf("HSEP: Shared files changed to %" PRIu64 " (%" PRIu64" KiB)\n",
			    ownfiles, ownkibibytes);
		}
		
		hsep_own[HSEP_IDX_FILES] = ownfiles;
		hsep_own[HSEP_IDX_KIB] = ownkibibytes;

		/*
		 * We could send a HSEP message to all nodes now, but these changes
		 * will propagate within at most HSEP_MSG_INTERVAL + HSEP_MSG_SKEW
		 * seconds anyway.
		 */
	}
}

/*
 * hsep_sanity_check
 *
 * Sanity check for the global and per-connection HSEP tables.
 * Assertions are made for all these checks. If HSEP is implemented
 * and used correctly, the sanity check will succed.
 *
 * Performed checks (* stands for an arbitrary value):
 *
 * - own triple must be (1, *, *)
 * - global triple for 0 hops must be (0, 0, 0)
 * - per-connection triple for 0 hops must be (0, 0, 0)
 * - per-connection triple for 1 hops must be (1, *, *)
 * - per-connection triples must be monotonically increasing
 * - the sum of the n'th triple of each connection must match the
 *   n'th global table triple for all n
 */

void hsep_sanity_check(void)
{
	hsep_triple sum[HSEP_N_MAX+1];
	GSList *sl;
	guint64 *globalt;
	guint64 *sumt;
	unsigned int i;

	memset(sum, 0, sizeof(sum));

	g_assert(hsep_own[HSEP_IDX_NODES] == 1);

	/*
	 * Iterate over all HSEP-capable nodes, and for each triple index
	 * sum up all the connections' triple values.
	 */

	for (sl = (GSList *) node_all_nodes() ; sl; sl = g_slist_next(sl)) {
		struct gnutella_node *n = (struct gnutella_node *) sl->data;
		guint64 *connectiont;

		/* also consider unestablished connections here */

		if (!(n->attrs & NODE_A_CAN_HSEP))
			continue;

		sumt = (guint64 *) sum;
		connectiont = (guint64 *) n->hsep_table;

		g_assert(connectiont[HSEP_IDX_NODES] == 0);      /* check nodes */
		g_assert(connectiont[HSEP_IDX_FILES] == 0);      /* check files */
		g_assert(connectiont[HSEP_IDX_KIB] == 0);        /* check KiB */
		g_assert(connectiont[HSEP_IDX_NODES + 3] == 1);  /* check nodes */

		/* check if values are monotonously increasing (skip first) */
		g_assert(
			hsep_check_monotony((hsep_triple *) (connectiont + 3), HSEP_N_MAX)
			);

		/* sum up the values */

		for (i = 0; i <= HSEP_N_MAX; i++) {
			*sumt++ += *connectiont++;
			*sumt++ += *connectiont++;
			*sumt++ += *connectiont++;
		}
	}

	globalt = (guint64 *) hsep_global_table;
	sumt = (guint64 *) sum;

	/* check sums */

	for (i = 0; i <= HSEP_N_MAX; i++) {
		g_assert(*globalt == *sumt);
		globalt++;
		sumt++;
		g_assert(*globalt == *sumt);
		globalt++;
		sumt++;
		g_assert(*globalt == *sumt);
		globalt++;
		sumt++;
	}
}

/*
 * hsep_dump_table
 *
 * Outputs the global HSEP table to the console.
 */

void hsep_dump_table(void)
{
	unsigned int i;

	printf("HSEP: Reachable nodes (1-%d hops): ", HSEP_N_MAX);

	for (i = 1; i <= HSEP_N_MAX; i++)
		printf("%" PRIu64 " ", hsep_global_table[i][HSEP_IDX_NODES]);

	printf("\nHSEP: Reachable files (1-%d hops): ", HSEP_N_MAX);

	for (i = 1; i <= HSEP_N_MAX; i++)
		printf("%" PRIu64 " ", hsep_global_table[i][HSEP_IDX_FILES]);

	printf("\nHSEP:   Reachable KiB (1-%d hops): ", HSEP_N_MAX);

	for (i = 1; i <= HSEP_N_MAX; i++)
		printf("%" PRIu64 " ", hsep_global_table[i][HSEP_IDX_KIB]);

	printf("\n");

	hsep_sanity_check();
}

/*
 * hsep_check_monotony
 *
 * Checks the monotony of the given triples. TRUE is returned if 0 or 1
 * triple is given. Returns TRUE if monotony is ok, FALSE otherwise.
 */

gboolean hsep_check_monotony(hsep_triple *table, unsigned int triples)
{
	guint64 *prev;
	guint64 *curr;
	gboolean error = FALSE;

	g_assert(table);

	if (triples < 2)  /* handle special case */
		return TRUE;
	
	prev = (guint64 *) table;
	curr = (guint64 *) &table[1];
	
	/* if any triple is not >= the previous one, error will be TRUE */

	while (!error && --triples)
		error |= (*curr++ < *prev++) ||
				  (*curr++ < *prev++) ||
				  (*curr++ < *prev++);

	return FALSE == error;
}

/*
 * hsep_triples_to_send
 *
 * Takes a list of triples and returns the optimal number of triples
 * to send in a HSEP message. The number of triples to send
 * is n_opt, defined as (triple indices counted from 0):
 *
 * n_opt := 1 + min {n | triple[n] = triple[k] for all k in [n+1,triples-1]}
 *
 * If there is no such n_opt, n_opt := triples.
 * If all triples are equal, 1 is returned, which is correct.
 *
 * NOTE: this algorithm works regardless of the byte order of the triple data,
 * because only equality tests are used.
 */

unsigned int hsep_triples_to_send(const hsep_triple *table,
	unsigned int triples)
{
	guint64 a, b, c;
	guint64 *ptr = (guint64 *) &table[triples];

	g_assert(table);

	if (triples < 2)  /* handle special case */
		return triples;

	c = *--ptr;  /* get KiB of last triple   */
	b = *--ptr;  /* get files of last triple */
	a = *--ptr;  /* get nodes of last triple */

	/*
	 * ptr now points to the start of the last triple. We go backwards until
	 * we find a triple where at least one of its components is different
	 * from the previously checked triple.
	 */

	while (triples > 0 && *--ptr == c && *--ptr == b && *--ptr == a)
		triples--;

	return triples;
}

/**
 * hsep_get_global_table
 *
 * Copies the first maxtriples triples from the global HSEP table into
 * the specified buffer. If maxtriples is larger than the number of
 * triples in the table, it is truncated appropriately. Note that also
 * the 0'th triple is copied, which is always zero.
 *
 * The number of copied triples is returned.
 */

unsigned int hsep_get_global_table(hsep_triple *buffer, unsigned int maxtriples)
{
	unsigned int i;
	guint64 *src = (guint64 *) hsep_global_table;
	guint64 *dest = (guint64 *) buffer;

	g_assert(buffer);

	if (maxtriples > HSEP_N_MAX + 1)
		maxtriples = HSEP_N_MAX + 1;

	for (i = 0; i < maxtriples; i++) {
		*dest++ = *src++;
		*dest++ = *src++;
		*dest++ = *src++;
	}

	return maxtriples;
}

/**
 * hsep_get_connection_table
 *
 * Copies the first maxtriples triples from the connection's HSEP table into
 * the specified buffer. If maxtriples is larger than the number of
 * triples in the table, it is truncated appropriately. Note that also
 * the 0'th triple is copied, which is always zero.
 *
 * The number of copied triples is returned.
 */

unsigned int hsep_get_connection_table(struct gnutella_node *n,
    hsep_triple *buffer, unsigned int maxtriples)
{
	unsigned int i;
	guint64 *src;
	guint64 *dest = (guint64 *) buffer;
	
	g_assert(n);
	g_assert(buffer);

	src = (guint64 *) n->hsep_table;

	if (maxtriples > HSEP_N_MAX + 1)
		maxtriples = HSEP_N_MAX + 1;

	for (i = 0; i < maxtriples; i++) {
		*dest++ = *src++;
		*dest++ = *src++;
		*dest++ = *src++;
	}

	return maxtriples;
}

/*
 * hsep_close
 *
 * Used to shutdown HSEP.
 */

void hsep_close(void)
{
	event_destroy(hsep_global_table_changed_event);
}

/*
 * hsep_fire_global_table_changed
 *
 * Fires a change event for the global HSEP table.
 */

void hsep_fire_global_table_changed(time_t now)
{
	/* store global table change time */
	hsep_last_global_table_change = now;

	/* do nothing if we don't have any listeners */

	if (event_subscriber_active(hsep_global_table_changed_event)) {
		hsep_triple table[HSEP_N_MAX + 1];

		/*
		 * Make a copy of the global HSEP table and give that
		 * copy and the number of included triples to the
		 * listeners.
		 */

		hsep_get_global_table(table, G_N_ELEMENTS(table));

		event_trigger(hsep_global_table_changed_event,
		    T_NORMAL(hsep_global_listener_t, table, G_N_ELEMENTS(table)));
	}
}

/*
 * hsep_has_global_table_changed
 *
 * Checks whether the global HSEP table has changed since the
 * specified point in time. Returns TRUE if this is the case,
 * FALSE otherwise.
 */

gboolean hsep_has_global_table_changed(time_t since)
{
	return hsep_last_global_table_change > since;
}

/*
 * hsep_get_non_hsep_triple
 *
 * Gets a HSEP-compatible triple for all non-HSEP nodes.
 * The number of nodes is just the number of established non-HSEP
 * connections, the number of shared files and KiB is the
 * sum of the known PONG-based library sizes of those connections.
 * Note that this takes only direct neighbor connections into
 * account. Also note that the shared library size in KiB is
 * not accurate due to Gnutella protocol limitations.
 *
 * The determined values are stored in the provided triple address.
 */

void hsep_get_non_hsep_triple(hsep_triple *tripledest)
{
	GSList *sl;
	guint64 other_nodes = 0;      /* # of non-HSEP nodes */
	guint64 other_files = 0;      /* what non-HSEP nodes share (files) */
	guint64 other_kib = 0;        /* what non-HSEP nodes share (KiB) */

	g_assert(tripledest);

	/*
	 * Iterate over all established non-HSEP nodes and count these nodes and
	 * sum up what they share (PONG-based library size).
	 */

	for (sl = (GSList *) node_all_nodes() ; sl; sl = g_slist_next(sl)) {
		struct gnutella_node *n = (struct gnutella_node *) sl->data;
		gnet_node_status_t status;

		if ((!NODE_IS_ESTABLISHED(n)) || n->attrs & NODE_A_CAN_HSEP)
			continue;

		other_nodes++;

		node_get_status(n->node_handle, &status);

		if (status.gnet_info_known) {
			other_files += status.gnet_files_count;
			other_kib += status.gnet_kbytes_count;
		}
	}

	tripledest[0][HSEP_IDX_NODES] = other_nodes;
	tripledest[0][HSEP_IDX_FILES] = other_files;
	tripledest[0][HSEP_IDX_KIB] = other_kib;
}


/*
 * hsep_get_static_str
 *
 * Returns a static string of the cell contents of the given row and column.
 * NB: The static buffers for each column are disjunct.
 */
const gchar *hsep_get_static_str(gint row, gint column)
{
	static gchar buf[21];
	hsep_triple hsep_table[HSEP_N_MAX + 1];
	hsep_triple *other = walloc(sizeof(hsep_triple));

	hsep_get_global_table(hsep_table, G_N_ELEMENTS(hsep_table));
	hsep_get_non_hsep_triple(other);

    switch (column) {
    case HSEP_IDX_NODES:
		gm_snprintf(buf, sizeof(buf), "%" PRIu64,
		    hsep_table[row][HSEP_IDX_NODES] + other[0][HSEP_IDX_NODES]);
		break;
	
    case HSEP_IDX_FILES:

		gm_snprintf(buf, sizeof(buf), "%" PRIu64,
		    hsep_table[row][HSEP_IDX_FILES] + other[0][HSEP_IDX_FILES]);
		break;
	
	case HSEP_IDX_KIB:
		/* Make a copy because concurrent usage of short_kb_size64()
	 	 * could be hard to discover. */
		g_strlcpy(buf, short_kb_size64(hsep_table[row][HSEP_IDX_KIB] +
		    other[0][HSEP_IDX_KIB]), sizeof buf);	
		break;

	default:
		g_assert_not_reached();
	    return NULL;
    }
	
	wfree(other, sizeof(hsep_triple));
  	return buf;
}

/*
 * hsep_get_table_size
 *
 * Returns the size of the global hsep table
 *
 */
gint hsep_get_table_size(void)
{	
	hsep_triple hsep_table[HSEP_N_MAX + 1];
	hsep_get_global_table(hsep_table, G_N_ELEMENTS(hsep_table));
	return G_N_ELEMENTS(hsep_table);
}


/* vi: set ts=4: */
