/*
 * $Id$
 *
 * Copyright (c) 2002-2003, Raphael Manfredi
 *
 * Network driver -- link level.
 *
 * This driver reads data from the network and builds messages that are given
 * to the upper layer on the "interrupt stack".
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

#include "gnutella.h"
#include <errno.h>

#include "nodes.h"
#include "sockets.h"
#include "pmsg.h"
#include "rx.h"
#include "rx_link.h"
#include "rxbuf.h"
#include "bsched.h"
#include "override.h"		/* Must be the last header included */

RCSID("$Id$");

/*
 * Private attributes for the link.
 */
struct attr {
	wrap_io_t 	 *wio;	/* Cached wrapped IO object */
	bio_source_t *bio;	/* Bandwidth-limited I/O source */
	bsched_t *bs;		/* Scheduler to attach I/O source to */
};

/*
 * is_readable
 *
 * Invoked when the input file descriptor has more data available.
 */
static void is_readable(gpointer data, gint source, inputevt_cond_t cond)
{
	rxdrv_t *rx = (rxdrv_t *) data;
	struct attr *attr = (struct attr *) rx->opaque;
	struct gnutella_node *n = rx->node;
	pdata_t *db;
	pmsg_t *mb;
	gint r;

	g_assert(attr->bio);			/* Input enabled */

	if (cond & INPUT_EVENT_EXCEPTION) {
		node_eof(n, "Read failed (Input Exception)");
		return;
	}

	/*
	 * Grab an RX buffer, and try to fill as much as we can.
	 */

	db = rxbuf_new();

	r = bio_read(attr->bio, pdata_start(db), pdata_len(db));

	if (r == 0) {
		if (n->n_ping_sent <= 2 && n->n_pong_received)
			node_eof(n, "Got %d connection pong%s",
				n->n_pong_received, n->n_pong_received == 1 ? "" : "s");
		else
			node_eof(n, "Failed (EOF)");
		goto error;
	} else if (r < 0 && errno == EAGAIN)
		goto error;
	else if (r < 0) {
		node_eof(n, "Read error: %s", g_strerror(errno));
		goto error;
	}

	/*
	 * Got something, build a message and send it to the upper layer.
	 * NB: `mb' is expected to be freed by the last layer using it.
	 */

	node_add_rx_given(rx->node, r);

	mb = pmsg_alloc(PMSG_P_DATA, db, 0, r);

	(*rx->data_ind)(rx, mb);
	return;

error:
	rxbuf_free(db, NULL);
}

/***
 *** Polymorphic routines.
 ***/

/*
 * rx_link_init
 *
 * Initialize the driver.
 * Always succeeds, so never returns NULL.
 */
static gpointer rx_link_init(rxdrv_t *rx, gpointer args)
{
	struct attr *attr;

	g_assert(rx);

	attr = walloc(sizeof(*attr));

	attr->wio = &rx->node->socket->wio;
	attr->bio = NULL;
	attr->bs = rx->node->peermode == NODE_P_LEAF ? bws.glin : bws.gin;

	rx->opaque = attr;
	
	return rx;		/* OK */
}

/*
 * rx_link_destroy
 *
 * Get rid of the driver's private data.
 */
static void rx_link_destroy(rxdrv_t *rx)
{
	struct attr *attr = (struct attr *) rx->opaque;

	if (attr->bio) {
		bsched_source_remove(attr->bio);
		attr->bio = NULL;					/* Paranoid */
	}

	wfree(attr, sizeof(*attr));
}

/*
 * rx_link_recv
 *
 * Inject data into driver.
 *
 * Since we normally read from the network, we don't have to process those
 * data and can forward them directly to the upper layer.
 */
static void rx_link_recv(rxdrv_t *rx, pmsg_t *mb)
{
	g_assert(rx);
	g_assert(mb);

	node_add_rx_given(rx->node, pmsg_size(mb));

	/*
	 * Call the registered data_ind callback to feed the upper layer.
	 * NB: `mb' is expected to be freed by the last layer using it.
	 */

	(*rx->data_ind)(rx, mb);
}

/*
 * rx_link_enable
 *
 * Enable reception of data.
 */
static void rx_link_enable(rxdrv_t *rx)
{
	struct attr *attr = (struct attr *) rx->opaque;

	g_assert(attr->bio == NULL);

	/*
	 * Install reading callback.
	 */

	attr->bio = bsched_source_add(attr->bs, attr->wio, BIO_F_READ,
		is_readable, (gpointer) rx);

	g_assert(attr->bio);
}

/*
 * rx_link_disable
 *
 * Disable reception of data.
 */
static void rx_link_disable(rxdrv_t *rx)
{
	struct attr *attr = (struct attr *) rx->opaque;
	
	g_assert(attr->bio);

	bsched_source_remove(attr->bio);
	attr->bio = NULL;
}

/*
 * rx_link_bio_source
 *
 * Return I/O source of the lower level.
 */
static struct bio_source *rx_link_bio_source(rxdrv_t *rx)
{
	struct attr *attr = (struct attr *) rx->opaque;

	return attr->bio;
}

struct rxdrv_ops rx_link_ops = {
	rx_link_init,		/* init */
	rx_link_destroy,	/* destroy */
	rx_link_recv,		/* recv */
	rx_link_enable,		/* enable */
	rx_link_disable,	/* disable */
	rx_link_bio_source,	/* bio_source */
};

/* vi: set ts=4 sw=4 cindent: */
