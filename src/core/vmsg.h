/*
 * $Id$
 *
 * Copyright (c) 2003, Raphael Manfredi
 *
 * Vendor-specific messages.
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

#ifndef _core_vmsg_h_
#define _core_vmsg_h_

#include <glib.h>

struct gnutella_node;
struct pmsg;

/*
 * Public interface
 */

void vmsg_handle(struct gnutella_node *n);
const gchar *vmsg_infostr(gpointer data, gint size);

void vmsg_send_messages_supported(struct gnutella_node *n);
void vmsg_send_hops_flow(struct gnutella_node *n, guint8 hops);
void vmsg_send_tcp_connect_back(struct gnutella_node *n, guint16 port);
void vmsg_send_udp_connect_back(struct gnutella_node *n, guint16 port);
void vmsg_send_proxy_req(struct gnutella_node *n, const gchar *muid);
void vmsg_send_proxy_ack(struct gnutella_node *n, gchar *muid);
void vmsg_send_qstat_req(struct gnutella_node *n, gchar *muid);
void vmsg_send_qstat_answer(struct gnutella_node *n, gchar *muid, guint16 hits);
void vmsg_send_proxy_cancel(struct gnutella_node *n);
void vmsg_send_oob_reply_ack(struct gnutella_node *n, gchar *muid, guint8 want);

struct pmsg *vmsg_build_oob_reply_ind(gchar *muid, guint8 hits);

#endif	/* _core_vmsg_h_ */

/* vi: set ts=4: */
