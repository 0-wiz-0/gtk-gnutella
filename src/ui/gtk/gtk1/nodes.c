/*
 * $Id$
 *
 * Copyright (c) 2001-2003, Raphael Manfredi, Richard Eckart
 *
 * GUI filtering functions.
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

#include "gtk/gui.h"

RCSID("$Id$");

#include "interface-glade.h"
#include "gtk/nodes_common.h"
#include "gtk/nodes.h"
#include "gtk/notebooks.h"
#include "gtk/columns.h"

#include "if/gui_property_priv.h"
#include "if/bridge/ui2c.h"

#include "lib/glib-missing.h"
#include "lib/iso3166.h"
#include "lib/override.h"		/* Must be the last header included */

#define UPDATE_MIN	300		/* Update screen every 5 minutes at least */

static gchar gui_tmp[4096];

/* 
 * These hash tables record which information about which nodes has
 * changed. By using this the number of updates to the gui can be
 * significantly reduced.
 */
static GHashTable *ht_node_info_changed = NULL;
static GHashTable *ht_node_flags_changed = NULL;

static void nodes_gui_update_node_info(gnet_node_info_t *n, gint row);
static void nodes_gui_update_node_flags(
	gnet_node_t n, gnet_node_flags_t *flags, gint row);


static gboolean nodes_gui_is_visible(void)
{
	static GtkNotebook *notebook = NULL;
	gint current_page;

	if (notebook == NULL)
		notebook = GTK_NOTEBOOK(lookup_widget(main_window, "notebook_main"));

	current_page = gtk_notebook_get_current_page(notebook);

	return current_page == nb_main_page_gnet;
}

/***
 *** Callbacks
 ***/

/*
 * nodes_gui_node_removed:
 *
 * Callback: called when a node is removed from the backend.
 *
 * Removes all references to the node from the frontend.
 */
static void nodes_gui_node_removed(gnet_node_t n)
{
    if (gui_debug >= 5)
        printf("nodes_gui_node_removed(%u)\n", n);

    nodes_gui_remove_node(n);
}

/*
 * nodes_gui_node_added:
 *
 * Callback: called when a node is added from the backend.
 *
 * Adds the node to the gui.
 */
static void nodes_gui_node_added(gnet_node_t n)
{
    gnet_node_info_t info;

    if (gui_debug >= 5)
        printf("nodes_gui_node_added(%u)\n", n);

    guc_node_fill_info(n, &info);
    nodes_gui_add_node(&info);
    guc_node_clear_info(&info);
}

/*
 * nodes_gui_node_info_changed:
 *
 * Callback: called when node information was changed by the backend.
 *
 * This schedules an update of the node information in the gui at the
 * next tick. 
 */
static void nodes_gui_node_info_changed(gnet_node_t n)
{
    g_hash_table_insert(ht_node_info_changed, 
        GUINT_TO_POINTER(n), GUINT_TO_POINTER(1));
}

/*
 * nodes_gui_node_flags_changed
 *
 * Callback invoked when the node's user-visible flags are changed.
 *
 * This schedules an update of the node information in the gui at the
 * next tick. 
 */
static void nodes_gui_node_flags_changed(gnet_node_t n)
{
    g_hash_table_insert(ht_node_flags_changed, 
        GUINT_TO_POINTER(n), GUINT_TO_POINTER(1));
}


/***
 *** Private functions
 ***/

/*
 * nodes_gui_update_node_info:
 *
 * Update the row with the given nodeinfo. If row is -1 the row number
 * is determined by the node_handle contained in the gnet_node_info_t.
 */
static void nodes_gui_update_node_info(gnet_node_info_t *n, gint row)
{
    GtkCList *clist = GTK_CLIST
        (lookup_widget(main_window, "clist_nodes"));

    g_assert(n != NULL);

    if (row == -1) {
        row = gtk_clist_find_row_from_data(
            clist, GUINT_TO_POINTER(n->node_handle));
    }

    if (row != -1) {
        gnet_node_status_t status;
        time_t now = time((time_t *) NULL);

        guc_node_get_status(n->node_handle, &status);

        gtk_clist_set_text(clist, row, c_gnet_user_agent,
			n->vendor ? n->vendor : "...");

        gtk_clist_set_text(clist, row, c_gnet_loc,
			(char *) iso3166_country_cc(n->country)); /* override const */

        gm_snprintf(gui_tmp, sizeof(gui_tmp), "%d.%d",
            n->proto_major, n->proto_minor);
        gtk_clist_set_text(clist, row, c_gnet_version, gui_tmp);

		if (status.status == GTA_NODE_CONNECTED)
	        gtk_clist_set_text(clist, row, c_gnet_connected, 
       			short_uptime(now - status.connect_date));

		if (status.up_date)
    	    gtk_clist_set_text(clist, row, c_gnet_uptime, 
	        	status.up_date ?  short_uptime(now - status.up_date) : "...");

        gtk_clist_set_text(clist, row, c_gnet_info,
			nodes_gui_common_status_str(&status, now));
    } else {
        g_warning("%s: no matching row found", G_GNUC_PRETTY_FUNCTION);
    }
}

/*
 * nodes_gui_update_node_flags
 *
 */
static void nodes_gui_update_node_flags(
    gnet_node_t n, gnet_node_flags_t *flags, gint row)
{
    GtkCList *clist = GTK_CLIST
        (lookup_widget(main_window, "clist_nodes"));

    if (row == -1)
        row = gtk_clist_find_row_from_data(clist, GUINT_TO_POINTER(n));

    if (row != -1) {
        gtk_clist_set_text(clist, row, c_gnet_flags,
			nodes_gui_common_flags_str(flags));
	if (NODE_P_LEAF == flags->peermode || NODE_P_NORMAL == flags->peermode) {
		GdkColor *color = &(gtk_widget_get_style(GTK_WIDGET(clist))
			->fg[GTK_STATE_INSENSITIVE]);
		gtk_clist_set_foreground(clist, row, color);
	}

    } else {
        g_warning("%s: no matching row found", G_GNUC_PRETTY_FUNCTION);
    }
}

/***
 *** Public functions
 ***/

/*
 * nodes_gui_early_init:
 *
 * Initialized the widgets.
 */
void nodes_gui_early_init(void)
{
	popup_nodes = create_popup_nodes();
}

/*
 * nodes_gui_init:
 *
 * Initialize the nodes controller. Register callbacks in the backend.
 */
void nodes_gui_init(void) 
{
    gtk_clist_column_titles_passive(
        GTK_CLIST(lookup_widget(main_window, "clist_nodes")));

    gtk_widget_set_sensitive
        (lookup_widget(popup_nodes, "popup_nodes_remove"), FALSE);

    ht_node_info_changed = g_hash_table_new(g_direct_hash, g_direct_equal);
    ht_node_flags_changed = g_hash_table_new(g_direct_hash, g_direct_equal);

    guc_node_add_node_added_listener(nodes_gui_node_added);
    guc_node_add_node_removed_listener(nodes_gui_node_removed);
    guc_node_add_node_info_changed_listener(nodes_gui_node_info_changed);
    guc_node_add_node_flags_changed_listener(nodes_gui_node_flags_changed);
}

/*
 * nodes_gui_shutdown:
 *
 * Unregister callbacks in the backend and clean up.
 */
void nodes_gui_shutdown() 
{
    guc_node_remove_node_added_listener(nodes_gui_node_added);
    guc_node_remove_node_removed_listener(nodes_gui_node_removed);
    guc_node_remove_node_info_changed_listener(nodes_gui_node_info_changed);
    guc_node_remove_node_flags_changed_listener(nodes_gui_node_flags_changed);

    g_hash_table_destroy(ht_node_info_changed);
    g_hash_table_destroy(ht_node_flags_changed);

    ht_node_info_changed = NULL;
    ht_node_flags_changed = NULL;
}

/*
 * nodes_gui_remove_node:
 *
 * Removes all references to the given node handle in the gui.
 */
void nodes_gui_remove_node(gnet_node_t n)
{
    GtkWidget *clist_nodes;
    gint row;

    clist_nodes = lookup_widget(main_window, "clist_nodes");

    /* 
     * Make sure node is remove from the "changed" hash table so
     * we don't try an update. 
     */
    g_assert(NULL != ht_node_info_changed);
    g_assert(NULL != ht_node_flags_changed);

    g_hash_table_remove(ht_node_info_changed, GUINT_TO_POINTER(n));
    g_hash_table_remove(ht_node_flags_changed, GUINT_TO_POINTER(n));

	row = gtk_clist_find_row_from_data(GTK_CLIST(clist_nodes),
		GUINT_TO_POINTER(n));
    if (row != -1)
        gtk_clist_remove(GTK_CLIST(clist_nodes), row);
    else 
        g_warning("nodes_gui_remove_node: no matching row found");
}

/*
 * nodes_gui_add_node:
 *
 * Adds the given node to the gui.
 */
void nodes_gui_add_node(gnet_node_info_t *n)
{
    GtkCList *clist_nodes;
    gint row;
	gchar *titles[c_gnet_num];
	gchar proto_tmp[32];

    g_assert(n != NULL);

   	gm_snprintf(proto_tmp, sizeof(proto_tmp), "%d.%d",
		n->proto_major, n->proto_minor);

    titles[c_gnet_host]       = ip_port_to_gchar(n->ip, n->port);
    titles[c_gnet_flags]      = "...";
    titles[c_gnet_user_agent] = n->vendor ? n->vendor : "...";
    titles[c_gnet_loc]        = iso3166_country_cc(n->country);
    titles[c_gnet_version]    = proto_tmp;
    titles[c_gnet_connected]  = "...";
    titles[c_gnet_uptime]     = "...";
    titles[c_gnet_info]       = "...";

    clist_nodes = GTK_CLIST(lookup_widget(main_window, "clist_nodes"));

    row = gtk_clist_append(clist_nodes, titles);
    gtk_clist_set_row_data(clist_nodes, row, GUINT_TO_POINTER(n->node_handle));
}

/*
 * gui_update_nodes_display
 *
 * Update all the nodes at the same time.
 */

/* FIXME: we should remember for every node when it was last
 *        updated and only refresh every node at most once every
 *        second. This information should be kept in a struct pointed
 *        to by the row user_data and should be automatically freed
 *        when removing the row (see upload stats code).
 */
void nodes_gui_update_nodes_display(time_t now)
{
	GtkCList *clist;
	GList *l;
	gint row = 0;
    gnet_node_status_t status;
    static time_t last_update = 0;

    if (last_update == now)
        return;

	/*
	 * Usually don't perform updates if nobody is watching.  However,
	 * we do need to perform periodic cleanup of dead entries or the
	 * memory usage will grow.  Perform an update every UPDATE_MIN minutes
	 * at least.
	 *		--RAM, 28/12/2003
	 */
	if (!nodes_gui_is_visible() && delta_time(now, last_update) < UPDATE_MIN)
		return;

    last_update = now;

    clist = GTK_CLIST(lookup_widget(main_window, "clist_nodes"));
    gtk_clist_freeze(clist);

	for (l = clist->row_list, row = 0; l; l = l->next, row++) {
		gnet_node_t n =
			(gnet_node_t) GPOINTER_TO_UINT(((GtkCListRow *) l->data)->data);

        guc_node_get_status(n, &status);

        /* 
         * Update additional info too if it has recorded changes.
         */
        if (g_hash_table_lookup(ht_node_info_changed, GUINT_TO_POINTER(n))) {
            gnet_node_info_t info;

            g_hash_table_remove(ht_node_info_changed, GUINT_TO_POINTER(n));
            guc_node_fill_info(n, &info);
            nodes_gui_update_node_info(&info, row);
            guc_node_clear_info(&info);
        }

        if (g_hash_table_lookup(ht_node_flags_changed, GUINT_TO_POINTER(n))) {
            gnet_node_flags_t flags;

            g_hash_table_remove(ht_node_flags_changed, GUINT_TO_POINTER(n));
            guc_node_fill_flags(n, &flags);
            nodes_gui_update_node_flags(n, &flags, -1);
        }
    
		/*
		 * Don't update times if we've already disconnected.
		 */
		if (status.status == GTA_NODE_CONNECTED) {
	        gtk_clist_set_text(clist, row, c_gnet_connected, 
        			short_uptime(now - status.connect_date));
		
			if (status.up_date)
				gtk_clist_set_text(clist, row, c_gnet_uptime, 
					status.up_date ?
						short_uptime(now - status.up_date) : "...");
		}
        gtk_clist_set_text(clist, row, c_gnet_info,
			nodes_gui_common_status_str(&status, now));
    }
    gtk_clist_thaw(clist);
}

/* vi: set ts=4: */
