/*
 * $Id$
 *
 * Copyright (c) 2003, Raphael Manfredi
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
 * Vendor-specific messages.
 */

#include "common.h"			/* For -DUSE_DMALLOC */

#include "vmsg.h"
#include "vendors.h"
#include "nodes.h"
#include "search.h"
#include "gmsg.h"
#include "routing.h"		/* For message_set_muid() */
#include "gnet_stats.h"
#include "gnet_net_stats.h"
#include "dq.h"
#include "settings.h"		/* For listen_ip() */
#include "override.h"		/* Must be the last header included */

RCSID("$Id$");

static gchar v_tmp[256];	/* Large enough for a payload of 225 bytes */

/*
 * Vendor message handler.
 */

struct vmsg;

typedef void (*vmsg_handler_t)(struct gnutella_node *n,
	struct vmsg *vmsg, gchar *payload, gint size);

/*
 * Definition of vendor messages
 */
struct vmsg {
	guint32 vendor;
	guint16 id;
	guint16 version;
	vmsg_handler_t handler;
	gchar *name;
};

static void handle_messages_supported(struct gnutella_node *n,
	struct vmsg *vmsg, gchar *payload, gint size);
static void handle_hops_flow(struct gnutella_node *n,
	struct vmsg *vmsg, gchar *payload, gint size);
static void handle_connect_back(struct gnutella_node *n,
	struct vmsg *vmsg, gchar *payload, gint size);
static void handle_proxy_req(struct gnutella_node *n,
	struct vmsg *vmsg, gchar *payload, gint size);
static void handle_proxy_ack(struct gnutella_node *n,
	struct vmsg *vmsg, gchar *payload, gint size);
static void handle_qstat_req(struct gnutella_node *n,
	struct vmsg *vmsg, gchar *payload, gint size);
static void handle_qstat_answer(struct gnutella_node *n,
	struct vmsg *vmsg, gchar *payload, gint size);

/*
 * Known vendor-specific messages.
 */
static struct vmsg vmsg_map[] = {
	/* This list MUST be sorted by vendor, id, version */

	{ T_0000, 0x0000, 0x0000, handle_messages_supported, "Messages Supported" },
	{ T_BEAR, 0x0004, 0x0001, handle_hops_flow, "Hops Flow" },
	{ T_BEAR, 0x0007, 0x0001, handle_connect_back, "Connect Back" },
	{ T_BEAR, 0x000b, 0x0001, handle_qstat_req, "Query Status Request" },
	{ T_BEAR, 0x000c, 0x0001, handle_qstat_answer, "Query Status Response" },
	{ T_LIME, 0x0015, 0x0002, handle_proxy_req, "Push Proxy Request" },
	{ T_LIME, 0x0016, 0x0002, handle_proxy_ack, "Push Proxy Acknowledgment" },

	/* Above line intentionally left blank (for "!}sort" on vi) */
};

#define END(v)		(v - 1 + sizeof(v) / sizeof(v[0]))

/*
 * Items in the "Message Supported" vector.
 */
struct vms_item {
	guint32 vendor;
	guint16 selector_id;
	guint16 version;
};

#define VMS_ITEM_SIZE	8		/* Each entry is 8 bytes (4+2+2) */

/**
 * Find message, given vendor code, and id, version.
 *
 * We don't necessarily match the version exactly: we only guarantee to
 * return a handler whose version number is greater or equal than the message
 * received.
 *
 * Returns handler callback if found, NULL otherwise.
 */
static struct vmsg *
find_message(guint32 vendor, guint16 id, guint16 version)
{
	struct vmsg *low = vmsg_map;
	struct vmsg *high = END(vmsg_map);

	while (low <= high) {
		struct vmsg *mid = low + (high - low) / 2;
		gint c;

		c = vendor_code_cmp(mid->vendor, vendor);

		if (c == 0) {
			if (mid->id != id)
				c = mid->id < id ? -1 : +1;
		}

		if (c == 0) {
			if (mid->version < version)		/* Return match if >= */
				c = -1;
		}

		if (c == 0)
			return mid;
		else if (c < 0)
			low = mid + 1;
		else
			high = mid - 1;
	}

	return NULL;		/* Not found */
}

/**
 * Main entry point to handle reception of vendor-specific message.
 */
void
vmsg_handle(struct gnutella_node *n)
{
	struct gnutella_vendor *v = (struct gnutella_vendor *) n->data;
	guint32 vendor;
	guint16 id;
	guint16 version;
	struct vmsg *vm;

	READ_GUINT32_BE(v->vendor, vendor);
	READ_GUINT16_LE(v->selector_id, id);
	READ_GUINT16_LE(v->version, version);

	vm = find_message(vendor, id, version);

	if (dbg > 4)
		printf("VMSG %s \"%s\": vendor=%s, id=%u, version=%u\n",
			gmsg_infostr(&n->header), vm == NULL ? "UNKNOWN" : vm->name,
			vendor_code_str(vendor), id, version);

	/*
	 * If we can't handle the message, we count it as "unknown type", which
	 * is not completely exact because the type (vendor-specific) is known,
	 * it was only the subtype of that message which was unknown.  Still, I
	 * don't think it is ambiguous enough to warrant another drop type.
	 *		--RAM, 04/01/2003.
	 */

	if (vm == NULL) {
		gnet_stats_count_dropped(n, MSG_DROP_UNKNOWN_TYPE);
		if (dbg)
			g_warning("unknown vendor message: %s vendor=%s id=%u version=%u",
				gmsg_infostr(&n->header), vendor_code_str(vendor), id, version);
		return;
	}

	(*vm->handler)(n, vm, n->data + sizeof(*v), n->size - sizeof(*v));
}

/**
 * Fill common message header part for all vendor-specific messages.
 * The GUID is blanked (all zero bytes), TTL is set to 1 and hops to 0.
 * Those common values can be superseded by the caller if needed.
 *
 * `size' is only the size of the payload we filled so far.
 * `maxsize' is the size of the already allocated vendor messsage.
 *
 * Returns the total size of the whole Gnutella message.
 */
static guint32
vmsg_fill_header(struct gnutella_header *header, guint32 size, guint32 maxsize)
{
	guint32 msize;

	memset(header->muid, 0, 16);				/* Default GUID: all blank */
	header->function = GTA_MSG_VENDOR;
	header->ttl = 1;
	header->hops = 0;

	msize = size + sizeof(struct gnutella_vendor);

	WRITE_GUINT32_LE(msize, header->size);

	msize += sizeof(struct gnutella_header);

	if (msize > maxsize)
		g_error("allocated vendor message is only %u bytes, would need %u",
			maxsize, msize);

	return msize;
}

/**
 * Fill leading part of the payload data, containing the common part for
 * all vendor-specific messages.
 *
 * Returns start of payload after that common part.
 */
static guchar *
vmsg_fill_type(
	struct gnutella_vendor *base, guint32 vendor, guint16 id, guint16 version)
{
	WRITE_GUINT32_BE(vendor, base->vendor);
	WRITE_GUINT16_LE(id, base->selector_id);
	WRITE_GUINT16_LE(version, base->version);

	return (guchar *) (base + 1);
}

/**
 * Report a vendor-message with bad payload to the stats.
 */
static void
vmsg_bad_payload(
	struct gnutella_node *n, struct vmsg *vmsg, gint size, gint expected)
{
	n->rx_dropped++;
	n->n_bad++;
	gnet_stats_count_dropped(n, MSG_DROP_BAD_SIZE);

	if (dbg)
		gmsg_log_bad(n, "Bad payload size %d for %s/%dv%d (%s), expected %d",
			size, vendor_code_str(vmsg->vendor), vmsg->id, vmsg->version,
			vmsg->name, expected);
}

/**
 * Handle the "Messages Supported" message.
 */
static void
handle_messages_supported(struct gnutella_node *n,
	struct vmsg *vmsg, gchar *payload, gint size)
{
	guint16 count;
	gint i;
	gchar *description;
	gint expected;

	READ_GUINT16_LE(payload, count);

	if (dbg)
		printf("VMSG node %s <%s> supports %u vendor message%s\n",
			node_ip(n), node_vendor(n), count,
			count == 1 ? "" : "s");

	expected = (gint) sizeof(count) + count * VMS_ITEM_SIZE;

	if (size != expected) {
		vmsg_bad_payload(n, vmsg, size, expected);
		return;
	}

	description = payload + 2;		/* Skip count */

	/*
	 * Analyze the supported messages.
	 */

	for (i = 0; i < count; i++) {
		guint32 vendor;
		gint id;
		gint version;
		struct vmsg *vm;

		READ_GUINT32_BE(description, vendor);
		description += 4;
		READ_GUINT16_LE(description, id);
		description += 2;
		READ_GUINT16_LE(description, version);
		description += 2;

		vm = find_message(vendor, id, version);

		if (vm == NULL) {
			if (dbg > 1)
				printf("VMSG node %s <%s> supports unknown %s/%dv%d\n",
					node_ip(n), node_vendor(n),
					vendor_code_str(vendor), id, version);
			continue;
		}

		/*
		 * Look for leaf-guided dynamic query support.
		 *
		 * Remote can advertise only one of the two messages needed, we
		 * can infer support for the other!.
		 */

		if (
			vm->handler == handle_qstat_req ||
			vm->handler == handle_qstat_answer
		)
			n->attrs |= NODE_A_LEAF_GUIDE;
	}
}

/**
 * Send a "Messages Supported" message to specified node, telling it which
 * subset of the vendor messages we can understand.  We don't send information
 * about the "Messages Supported" message itself, since this one is guarateeed
 * to be always understood
 */
void
vmsg_send_messages_supported(struct gnutella_node *n)
{
	struct gnutella_msg_vendor *m = (struct gnutella_msg_vendor *) v_tmp;
	guint16 count = G_N_ELEMENTS(vmsg_map) - 1;
	guint32 paysize = sizeof(count) + count * VMS_ITEM_SIZE;
	guint32 msgsize;
	guchar *payload;
	guint i;

	msgsize = vmsg_fill_header(&m->header, paysize, sizeof(v_tmp));
	payload = vmsg_fill_type(&m->data, T_0000, 0, 0);

	/*
	 * First 2 bytes is the number of entries in the vector.
	 */

	WRITE_GUINT16_LE(count, payload);
	payload += 2;

	/*
	 * Fill one entry per message type supported, excepted ourselves.
	 */

	for (i = 0; i < G_N_ELEMENTS(vmsg_map); i++) {
		struct vmsg *msg = &vmsg_map[i];

		if (msg->vendor == T_0000)		/* Don't send info about ourselves */
			continue;

		WRITE_GUINT32_BE(msg->vendor, payload);
		payload += 4;
		WRITE_GUINT16_LE(msg->id, payload);
		payload += 2;
		WRITE_GUINT16_LE(msg->version, payload);
		payload += 2;
	}

	gmsg_sendto_one(n, (gchar *) m, msgsize);
}

/**
 * Handle the "Hops Flow" message.
 */
static void
handle_hops_flow(struct gnutella_node *n,
	struct vmsg *vmsg, gchar *payload, gint size)
{
	guint8 hops;

	g_assert(vmsg->version <= 1);

	if (size != 1) {
		vmsg_bad_payload(n, vmsg, size, 1);
		return;
	}

	hops = *payload;
	node_set_hops_flow(n, hops);
}

/**
 * Send an "Hops Flow" message to specified node.
 */
void
vmsg_send_hops_flow(struct gnutella_node *n, guint8 hops)
{
	struct gnutella_msg_vendor *m = (struct gnutella_msg_vendor *) v_tmp;
	guint32 paysize = sizeof(hops);
	guint32 msgsize;
	guchar *payload;

	msgsize = vmsg_fill_header(&m->header, paysize, sizeof(v_tmp));
	payload = vmsg_fill_type(&m->data, T_BEAR, 4, 1);

	*payload = hops;

	/*
	 * Send the message as a control message, so that it gets sent ASAP.
	 */

	gmsg_ctrl_sendto_one(n, (gchar *) m, msgsize);
}

/**
 * Handle the "Connect Back" message.
 */
static void
handle_connect_back(struct gnutella_node *n,
	struct vmsg *vmsg, gchar *payload, gint size)
{
	guint16 port;

	g_assert(vmsg->version <= 1);

	if (size != 2) {
		vmsg_bad_payload(n, vmsg, size, 2);
		return;
	}

	READ_GUINT16_LE(payload, port);

	if (port == 0) {
		g_warning("got improper port #%d in %s from %s <%s>",
			port, vmsg->name, node_ip(n), node_vendor(n));
		return;
	}

	/* XXX forward to neighbours supporting the remote connect back message? */

	node_connect_back(n, port);
}

/**
 * Send an "Connect Back" message to specified node, telling it to connect
 * back to us on the specified port.
 */
void
vmsg_send_connect_back(struct gnutella_node *n, guint16 port)
{
	struct gnutella_msg_vendor *m = (struct gnutella_msg_vendor *) v_tmp;
	guint32 paysize = sizeof(port);
	guint32 msgsize;
	guchar *payload;

	msgsize = vmsg_fill_header(&m->header, paysize, sizeof(v_tmp));
	payload = vmsg_fill_type(&m->data, T_BEAR, 7, 1);

	WRITE_GUINT16_LE(port, payload);

	gmsg_sendto_one(n, (gchar *) m, msgsize);
}

/**
 * Handle reception of the "Push Proxy Request" message.
 */
static void
handle_proxy_req(struct gnutella_node *n,
	struct vmsg *vmsg, gchar *payload, gint size)
{
	if (size != 0) {
		vmsg_bad_payload(n, vmsg, size, 0);
		return;
	}

	/*
	 * Normally, a firewalled host should be a leaf node, not an UP.
	 * Warn if node is not a leaf, but accept to be the push proxy
	 * nonetheless.
	 */

	if (!NODE_IS_LEAF(n))
		g_warning("got %s from non-leaf node %s <%s>",
			vmsg->name, node_ip(n), node_vendor(n));

	/*
	 * Add proxying info for this node.  On successful completion,
	 * we'll send an acknowledgement.
	 */

	if (node_proxying_add(n, n->header.muid))	/* MUID is the node's GUID */
		vmsg_send_proxy_ack(n, n->header.muid);
}

/**
 * Send a "Push Proxy Request" message to specified node, using supplied
 * `muid' as the message ID (which is our GUID).
 */
void
vmsg_send_proxy_req(struct gnutella_node *n, gchar *muid)
{
	struct gnutella_msg_vendor *m = (struct gnutella_msg_vendor *) v_tmp;
	guint32 msgsize;

	g_assert(!NODE_IS_LEAF(n));

	msgsize = vmsg_fill_header(&m->header, 0, sizeof(v_tmp));
	memcpy(m->header.muid, muid, 16);
	(void) vmsg_fill_type(&m->data, T_LIME, 21, 2);

	gmsg_sendto_one(n, (gchar *) m, msgsize);

	if (dbg > 2)
		g_warning("sent proxy REQ to %s <%s>", node_ip(n), node_vendor(n));
}

/**
 * Handle reception of the "Push Proxy Acknowledgment" message.
 */
static void
handle_proxy_ack(struct gnutella_node *n,
	struct vmsg *vmsg, gchar *payload, gint size)
{
	guint32 ip;
	guint16 port;

	g_assert(vmsg->version >= 2);

	if (size != 6) {
		vmsg_bad_payload(n, vmsg, size, 6);
		return;
	}

	READ_GUINT32_BE(payload, ip);
	payload += 4;
	READ_GUINT16_LE(payload, port);

	if (dbg > 2)
		g_warning("got proxy ACK from %s <%s>: proxy at %s",
			node_ip(n), node_vendor(n), ip_port_to_gchar(ip, port));


	if (!host_is_valid(ip, port)) {
		g_warning("got improper address %s in %s from %s <%s>",
			ip_port_to_gchar(ip, port), vmsg->name,
			node_ip(n), node_vendor(n));
		return;
	}

	node_proxy_add(n, ip, port);
}

/**
 * Send a "Push Proxy Acknowledgment" message to specified node, using
 * supplied `muid' as the message ID (which is the target node's GUID).
 */
void
vmsg_send_proxy_ack(struct gnutella_node *n, gchar *muid)
{
	struct gnutella_msg_vendor *m = (struct gnutella_msg_vendor *) v_tmp;
	guint32 paysize = sizeof(guint32) + sizeof(guint16);
	guint32 msgsize;
	guchar *payload;

	msgsize = vmsg_fill_header(&m->header, paysize, sizeof(v_tmp));
	memcpy(m->header.muid, muid, 16);
	payload = vmsg_fill_type(&m->data, T_LIME, 22, 2);

	WRITE_GUINT32_BE(listen_ip(), payload);
	payload += 4;
	WRITE_GUINT16_LE(listen_port, payload);

	/*
	 * Reply with a control message, so that the issuer knows that we can
	 * proxyfy pushes to it ASAP.
	 */

	gmsg_ctrl_sendto_one(n, (gchar *) m, msgsize);
}

/**
 * Handle reception of "Query Status Request", where the UP requests how
 * many results the search filters of the leave (ourselves) let pass through.
 */
static void
handle_qstat_req(struct gnutella_node *n,
	struct vmsg *vmsg, gchar *payload, gint size)
{
	guint32 kept;

	if (size != 0) {
		vmsg_bad_payload(n, vmsg, size, 0);
		return;
	}

	if (!search_get_kept_results(n->header.muid, &kept)) {
		/*
		 * We did not find any search for this MUID.  Either the remote
		 * side goofed, or they closed the search.
		 */

		kept = 0xffff;		/* Magic value telling them to stop the search */
	}

	vmsg_send_qstat_answer(n, n->header.muid, (guint16) kept);
}

/**
 * Send a "Query Status Request" message to specified node, using supplied
 * `muid' as the message ID (which is the query ID).
 */
void
vmsg_send_qstat_req(struct gnutella_node *n, gchar *muid)
{
	struct gnutella_msg_vendor *m = (struct gnutella_msg_vendor *) v_tmp;
	guint32 msgsize;

	msgsize = vmsg_fill_header(&m->header, 0, sizeof(v_tmp));
	memcpy(m->header.muid, muid, 16);
	(void) vmsg_fill_type(&m->data, T_BEAR, 11, 1);

	gmsg_sendto_one(n, (gchar *) m, msgsize);
}

/**
 * Handle "Query Status Response" where the leave notifies us about the
 * amount of results its search filters let pass through for the specified
 * query.
 */
static void
handle_qstat_answer(struct gnutella_node *n,
	struct vmsg *vmsg, gchar *payload, gint size)
{
	guint16 kept;

	if (size != 2) {
		vmsg_bad_payload(n, vmsg, size, 2);
		return;
	}

	/*
	 * Let the dynamic querying side about the reply.
	 */

	READ_GUINT16_LE(payload, kept);
	
	if (kept)
		dq_got_query_status(n->header.muid, NODE_ID(n), kept);
}

/**
 * Send a "Query Status Response" message to specified node.
 *
 * @param muid is the query ID
 * @param hits is the number of hits our filters did not drop.
 */
void
vmsg_send_qstat_answer(struct gnutella_node *n, gchar *muid, guint16 hits)
{
	struct gnutella_msg_vendor *m = (struct gnutella_msg_vendor *) v_tmp;
	guint32 msgsize;
	guint32 paysize = sizeof(guint16);
	gchar *payload;

	msgsize = vmsg_fill_header(&m->header, paysize, sizeof(v_tmp));
	memcpy(m->header.muid, muid, 16);
	payload = vmsg_fill_type(&m->data, T_BEAR, 12, 1);

	WRITE_GUINT16_LE(hits, payload);

	gmsg_sendto_one(n, (gchar *) m, msgsize);
}

/* vi: set ts=4: */
