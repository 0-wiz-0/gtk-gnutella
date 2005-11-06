/*
 * $Id$
 *
 * Copyright (c) 2001-2003, Raphael Manfredi, Richard Eckart
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
 * @ingroup gtk
 * @file
 *
 * GUI filtering functions.
 *
 * @author Raphael Manfredi
 * @author Richard Eckart
 * @date 2001-2003
 */

#include "gtk/gui.h"

RCSID("$Id$");

#include "interface-glade.h"

#include "gtk/gtk-missing.h"
#include "gtk/nodes_common.h"
#include "gtk/nodes.h"
#include "gtk/columns.h"
#include "gtk/notebooks.h"
#include "gtk/settings.h"
#include "gtk/statusbar.h"

#include "if/gui_property.h"
#include "if/gui_property_priv.h"
#include "if/bridge/ui2c.h"

#include "lib/atoms.h"
#include "lib/glib-missing.h"
#include "lib/iso3166.h"
#include "lib/utf8.h"
#include "lib/walloc.h"
#include "lib/override.h"	/* Must be the last header included */

#define UPDATE_MIN	300		/**< Update screen every 5 minutes at least */

/*
 * These hash tables record which information about which nodes has
 * changed. By using this the number of updates to the gui can be
 * significantly reduced.
 */
static GHashTable *ht_node_info_changed = NULL;
static GHashTable *ht_node_flags_changed = NULL;

static GtkTreeView *treeview_nodes = NULL;
static GtkTreeStore *nodes_model = NULL;

/* hash table for fast handle -> GtkTreeIter mapping */
static GHashTable *nodes_handles = NULL;
/* list of all node handles */

static GHashTable *ht_pending_lookups = NULL;

static tree_view_motion_t *tvm_nodes;

/***
 *** Private functions
 ***/

static void nodes_gui_node_removed(gnet_node_t);
static void nodes_gui_node_added(gnet_node_t);
static void nodes_gui_node_info_changed(gnet_node_t);
static void nodes_gui_node_flags_changed(gnet_node_t);

/**
 * Create a column, associating the attribute ``attr'' (usually "text") of the
 * cell_renderer to the first column of the model. Also associate the
 * foreground color with the c_gnet_fg column, so that we can set
 * the foreground color for the whole row.
 */
static void
add_column(GtkTreeView *tree, const gchar *title, 
	GtkTreeCellDataFunc cell_data_func, gpointer udata)
{
	GtkCellRenderer *renderer;
    GtkTreeViewColumn *column;

	renderer = gtk_cell_renderer_text_new();
	g_object_set(G_OBJECT(renderer),
	     "xpad", GUI_CELL_RENDERER_XPAD,
	     "ypad", GUI_CELL_RENDERER_YPAD,
	     (void *) 0);

   	column = gtk_tree_view_column_new_with_attributes(title, renderer,
				(void *) 0);
	g_object_set(G_OBJECT(column),
		"title", title,
		"fixed-width", 1,
		"min-width", 1,
		"reorderable", TRUE,
		"resizable", TRUE,
		"sizing", GTK_TREE_VIEW_COLUMN_FIXED,
		(void *) 0);
	
	if (cell_data_func != NULL)
		gtk_tree_view_column_set_cell_data_func(column, renderer,
			cell_data_func, udata, NULL);

    gtk_tree_view_append_column(GTK_TREE_VIEW(tree), column);
}

struct node_data {
	gchar *user_agent;	/* Atom */
	gchar *info;		/* walloc()ed */
	gchar *host;		/* walloc()ed */
	size_t host_size;
	size_t info_size;
	guint uptime;
	guint connected;
	gint country;
	GtkTreeIter iter;
	gchar version[24];
	gchar flags[16];
	gnet_node_t handle;
};

static void
node_data_free(gpointer value)
{
	struct node_data *data = value;

	if (data->user_agent)
		atom_str_free(data->user_agent);
	WFREE_NULL(data, sizeof *data);
}

static GtkTreeStore *
create_nodes_model(void)
{
	static GType columns[1];
	GtkTreeStore *store;

	columns[0] = G_TYPE_POINTER;
	store = gtk_tree_store_newv(G_N_ELEMENTS(columns), columns);
	return GTK_TREE_STORE(store);
}

static void
parent_cell_renderer(GtkCellRenderer *cell, 
	GtkTreeModel *model, GtkTreeIter *iter, guint id)
{
	static const GValue zero_value;
	GValue value = zero_value;
	const struct node_data *data;

	gtk_tree_model_get_value(model, iter, 0, &value);
	data = g_value_peek_pointer(&value);
	if (id == c_gnet_host)
		g_object_set(cell,
			"text", data->host, "xalign", (gfloat) 0.0, (void *) 0);
	else
		g_object_set(cell, "text", data->user_agent, (void *) 0);
}

static void
child_cell_renderer(GtkTreeViewColumn *column, GtkCellRenderer *cell, 
	GtkTreeModel *model, GtkTreeIter *iter)
{
	static const GValue zero_value;
	GtkTreeIter parent;
	GtkTreeView *tv;
	GtkTreePath *path;
	GValue value;
	const gchar *s;
	gboolean expanded;
	guint u;

	gtk_tree_model_iter_parent(model, &parent, iter);
	path = gtk_tree_model_get_path(model, &parent);
	tv = GTK_TREE_VIEW(column->tree_view);
	expanded = gtk_tree_view_row_expanded(tv, path);
	gtk_tree_path_free(path);

	if (!expanded) {
		s = NULL;
		g_object_set(cell, "text", s, (void *) 0);
		return;
	}
	
	value = zero_value;
	gtk_tree_model_get_value(model, iter, 0, &value);
	u = GPOINTER_TO_UINT(g_value_peek_pointer(&value));

	if (column == gtk_tree_view_get_column(tv, 0)) {
		switch (u) {
		case c_gnet_loc:		s = _("Location"); break;
		case c_gnet_connected:	s = _("Connected time"); break;
		case c_gnet_uptime:		s = _("Uptime"); break;
		case c_gnet_flags:		s = _("Flags"); break;
		case c_gnet_info:		s = _("Status"); break;
		case c_gnet_version:	s = _("Version"); break;
		default: 				s = NULL; break;
		}
		g_object_set(cell, "text", s, "xalign", (gfloat) 1.0, (void *) 0);
	} else {
		const struct node_data *data;
		
		value = zero_value;
		gtk_tree_model_get_value(model, &parent, 0, &value);
		data = g_value_peek_pointer(&value);

		switch (u) {
		case c_gnet_loc:		s = iso3166_country_name(data->country); break;
		case c_gnet_connected:	s = short_time(data->connected); break;
		case c_gnet_uptime:		s = short_time(data->uptime); break;
		case c_gnet_flags:		s = data->flags; break;
		case c_gnet_info:		s = data->info; break;
		case c_gnet_version:	s = data->version; break;
		default: 				s = NULL; break;
		}
		g_object_set(cell, "text", s, (void *) 0);
	}
}

static void
cell_renderer_func(GtkTreeViewColumn *column,
	GtkCellRenderer *cell, GtkTreeModel *model, GtkTreeIter *iter,
	gpointer udata)
{
	if (gtk_tree_model_iter_has_child(model, iter))
		parent_cell_renderer(cell, model, iter, GPOINTER_TO_UINT(udata));
	else
		child_cell_renderer(column, cell, model, iter);
}

/**
 * Sets up the treeview_nodes object for use by
 * settings_gui. (Uses a default width of one; actual
 * widths are set during nodes_gui_init. This
 * component must be able to be initialized before
 * width settings are initialized.)
 */
static void
nodes_gui_create_treeview_nodes(void)
{
	static const struct {
		const gchar * const title;
		const guint id;
	} columns[] = {
		{ N_("Host"),			c_gnet_host },
		{ N_("User-Agent"), 	c_gnet_user_agent },
	};
	GtkTreeView *tree;
	guint i;

    /*
     * Create a model.  We are using the store model for now, though we
     * could use any other GtkTreeModel
     */
    nodes_model = create_nodes_model();

    /*
     * Get the monitor widget
     */
	tree = GTK_TREE_VIEW(lookup_widget(main_window, "treeview_nodes"));
	treeview_nodes = tree;
	gtk_tree_view_set_model(tree, GTK_TREE_MODEL(nodes_model));
	gtk_tree_selection_set_mode(gtk_tree_view_get_selection(tree),
		GTK_SELECTION_MULTIPLE);

	for (i = 0; i < G_N_ELEMENTS(columns); i++)
		add_column(tree, _(columns[i].title), cell_renderer_func,
			GUINT_TO_POINTER(columns[i].id));
}

static inline void
nodes_gui_remove_selected_helper(GtkTreeModel *model,
		GtkTreePath *unused_path, GtkTreeIter *iter, gpointer list_ptr)
{
	GtkTreeIter parent;
	GSList **list = list_ptr;
	const struct node_data *data;
	gboolean has_parent;

	(void) unused_path;

	has_parent = gtk_tree_model_iter_parent(model, &parent, iter);
	if (!has_parent) {
		gtk_tree_model_get(model, iter, 0, &data, (-1));
		*list = g_slist_prepend(*list, GUINT_TO_POINTER(data->handle));
	}
}

/**
 * Fetches the node_data that holds the data about the given node
 * and knows the GtkTreeIter.
 */
static inline struct node_data *
find_node(gnet_node_t n)
{
	return g_hash_table_lookup(nodes_handles, GUINT_TO_POINTER(n));
}

/**
 * Updates vendor, version and info column.
 */
static void
nodes_gui_update_node_info(struct node_data *data, gnet_node_info_t *info)
{
    gnet_node_status_t status;
	const gchar *s;
	size_t size;

    g_assert(info != NULL);

    if (data == NULL)
        data = find_node(info->node_handle);

	g_assert(NULL != data);
	g_assert(data->handle == info->node_handle);

    guc_node_get_status(info->node_handle, &status);
    gm_snprintf(data->version, sizeof data->version, "%u.%u",
		info->proto_major, info->proto_minor);
	if (data->user_agent)
		atom_str_free(data->user_agent);
	data->user_agent = info->vendor ? atom_str_get(info->vendor) : NULL;
	data->country = info->country;

	s = nodes_gui_common_status_str(&status);
	size = 1 + strlen(s);
	if (size > data->info_size) {
		WFREE_NULL(data->info, data->info_size);
		data->info = wcopy(s, size);
		data->info_size = size;
	} else {
		memcpy(data->info, s, size);
	}
}

/**
 *
 */
static void
nodes_gui_update_node_flags(struct node_data *data, gnet_node_flags_t *flags)
{
	g_assert(NULL != data);

	g_strlcpy(data->flags, nodes_gui_common_flags_str(flags),
		sizeof data->flags);
#if 0
	concat_strings(data->flags, sizeof data->flags,
		"<tt>", nodes_gui_common_flags_str(flags), "</tt>", (void *) 0);

	if (NODE_P_LEAF == flags->peermode || NODE_P_NORMAL == flags->peermode) {
	    GdkColor *color = &(gtk_widget_get_style(GTK_WIDGET(treeview_nodes))
				->fg[GTK_STATE_INSENSITIVE]);
	    gtk_tree_store_set(nodes_model, iter, c_gnet_fg, color, (-1));
	}
#endif
}

static const gchar *
peermode_to_string(node_peer_t m)
{
	switch (m) {
	case NODE_P_LEAF:
		return _("Leaf");
	case NODE_P_ULTRA:
		return _("Ultrapeer");
	case NODE_P_NORMAL:
		return _("Legacy");
	case NODE_P_CRAWLER:
		return _("Crawler");
	case NODE_P_UDP:
		return _("UDP");
	case NODE_P_AUTO:
	case NODE_P_UNKNOWN:
		break;
	}

	return _("Unknown");
}

static void
update_tooltip(GtkTreeView *tv, GtkTreePath *path)
{
	GtkTreeModel *model;
	GtkTreeIter iter;
	const struct node_data *data;
	gnet_node_t n;

	g_assert(tv != NULL);
	if (!path) {
		n = (gnet_node_t) -1;
	} else {
		GtkTreeIter parent;
		
		model = gtk_tree_view_get_model(tv);
		if (!gtk_tree_model_get_iter(model, &iter, path)) {
			g_warning("gtk_tree_model_get_iter() failed");
			return;
		}
		if (gtk_tree_model_iter_parent(model, &parent, &iter))
			iter = parent;

		gtk_tree_model_get(model, &iter, 0, &data, (-1));
		n = data->handle;
	}

	if ((gnet_node_t) -1 == n || !find_node(n)) {
		GtkWidget *w;

		gtk_tooltips_set_tip(settings_gui_tooltips(), GTK_WIDGET(tv),
			_("Move the cursor over a row to see details."), NULL);
		w = settings_gui_tooltips()->tip_window;
		if (w)
			gtk_widget_hide(w);
	} else {
		gnet_node_info_t info;
		gnet_node_flags_t flags;
		gchar text[1024];

		guc_node_fill_flags(n, &flags);
		guc_node_fill_info(n, &info);
		g_assert(info.node_handle == n);
		gm_snprintf(text, sizeof text,
			"%s %s\n"
			"%s %s (%s)\n"
			"%s %s (%s)\n"
			"%s %.64s",
			_("Peer:"),
			host_addr_port_to_string(info.addr, info.port),
			_("Peermode:"),
			peermode_to_string(flags.peermode),
			flags.incoming ? _("incoming") : _("outgoing"),
			_("Country:"),
			iso3166_country_name(info.country),
			iso3166_country_cc(info.country),
			_("Vendor:"),
			info.vendor ? info.vendor : _("Unknown"));

		guc_node_clear_info(&info);
		gtk_tooltips_set_tip(settings_gui_tooltips(),
			GTK_WIDGET(tv), text, NULL);
	}
}

static gboolean
on_leave_notify(GtkWidget *widget, GdkEventCrossing *unused_event,
	gpointer unused_udata)
{
	(void) unused_event;
	(void) unused_udata;

	update_tooltip(GTK_TREE_VIEW(widget), NULL);
	return FALSE;
}

static void
host_lookup_callback(const gchar *hostname, gpointer key)
{
	gnet_node_t n = GPOINTER_TO_UINT(key);
	gnet_node_info_t info;
	struct node_data *data;
	GtkTreeStore *store;
	GtkTreeView *tv;
	host_addr_t addr;
	guint16 port;

	if (
		!ht_pending_lookups ||
		!g_hash_table_lookup(ht_pending_lookups, key)
	)
		return;

	g_hash_table_remove(ht_pending_lookups, key);

	tv = GTK_TREE_VIEW(lookup_widget(main_window, "treeview_nodes"));
	store = GTK_TREE_STORE(gtk_tree_view_get_model(tv));
	data = find_node(n);
	if (!data)
		return;

	guc_node_fill_info(n, &info);
	g_assert(n == info.node_handle);
	addr = info.addr;
	port = info.port;
	guc_node_clear_info(&info);

	WFREE_NULL(data->host, data->host_size);
	
	if (hostname) {
		const gchar *host;
		gchar *to_free;

		if (utf8_is_valid_string(hostname)) {
			to_free = NULL;
			host = hostname;
		} else {
			to_free = locale_to_utf8_normalized(hostname, UNI_NORM_GUI);
			host = to_free;
		}
		
		data->host_size = w_concat_strings(&data->host,
							host, " (",
							host_addr_port_to_string(addr, port), ")",
							(void *) 0);

		G_FREE_NULL(to_free);
	} else {
		statusbar_gui_warning(10,
			_("Reverse lookup for %s failed"), host_addr_to_string(addr));
		data->host_size = w_concat_strings(&data->host,
							host_addr_port_to_string(addr, port),
							(void *) 0);
	}
}

static void
on_cursor_changed(GtkTreeView *tv, gpointer unused_udata)
{
	GtkTreePath *path = NULL;

	(void) unused_udata;
	g_assert(tv != NULL);

	gtk_tree_view_get_cursor(tv, &path, NULL);
	if (path) {
		update_tooltip(tv, path);
		gtk_tree_path_free(path);
		path = NULL;
	}
}

/***
 *** Public functions
 ***/


/**
 * Initialize the widgets. Include creation of the actual treeview for
 * other init functions that manipulate it, notably settings_gui_init.
 */
void
nodes_gui_early_init(void)
{
    popup_nodes = create_popup_nodes();
    nodes_gui_create_treeview_nodes();
}

/**
 * Initialize the nodes controller. Register callbacks in the backend.
 */
void
nodes_gui_init(void)
{
	treeview_nodes = GTK_TREE_VIEW(lookup_widget(
		main_window, "treeview_nodes"));

	tree_view_restore_widths(treeview_nodes, PROP_NODES_COL_WIDTHS);

#if GTK_CHECK_VERSION(2, 4, 0)
    g_object_set(treeview_nodes, "fixed_height_mode", TRUE, (void *) 0);
#endif /* GTK+ >= 2.4.0 */

	nodes_handles = g_hash_table_new_full(NULL, NULL, NULL, node_data_free);

    ht_node_info_changed = g_hash_table_new(NULL, NULL);
    ht_node_flags_changed = g_hash_table_new(NULL, NULL);
    ht_pending_lookups = g_hash_table_new(NULL, NULL);

    guc_node_add_node_added_listener(nodes_gui_node_added);
    guc_node_add_node_removed_listener(nodes_gui_node_removed);
    guc_node_add_node_info_changed_listener(nodes_gui_node_info_changed);
    guc_node_add_node_flags_changed_listener(nodes_gui_node_flags_changed);

	g_signal_connect(GTK_OBJECT(treeview_nodes), "cursor-changed",
		G_CALLBACK(on_cursor_changed), treeview_nodes);

	g_signal_connect(GTK_OBJECT(treeview_nodes), "leave-notify-event",
		G_CALLBACK(on_leave_notify), treeview_nodes);

	tvm_nodes = tree_view_motion_set_callback(treeview_nodes,
					update_tooltip, 400);
}

/**
 * Unregister callbacks in the backend and clean up.
 */
void
nodes_gui_shutdown(void)
{
	if (tvm_nodes) {
		tree_view_motion_clear_callback(treeview_nodes, tvm_nodes);
		tvm_nodes = NULL;
	}

	tree_view_save_widths(treeview_nodes, PROP_NODES_COL_WIDTHS);

    guc_node_remove_node_added_listener(nodes_gui_node_added);
    guc_node_remove_node_removed_listener(nodes_gui_node_removed);
    guc_node_remove_node_info_changed_listener(nodes_gui_node_info_changed);
    guc_node_remove_node_flags_changed_listener(nodes_gui_node_flags_changed);

	gtk_tree_store_clear(nodes_model);
	g_object_unref(G_OBJECT(nodes_model));
	nodes_model = NULL;
	gtk_tree_view_set_model(treeview_nodes, NULL);

	g_hash_table_destroy(nodes_handles);
	nodes_handles = NULL;

    g_hash_table_destroy(ht_node_info_changed);
    g_hash_table_destroy(ht_node_flags_changed);
    g_hash_table_destroy(ht_pending_lookups);

    ht_node_info_changed = NULL;
    ht_node_flags_changed = NULL;
    ht_pending_lookups = NULL;
}

/**
 * Removes all references to the given node handle in the gui.
 */
void
nodes_gui_remove_node(gnet_node_t n)
{
	struct node_data *data;

    /*
     * Make sure node is remove from the "changed" hash tables so
     * we don't try an update later.
     */
    g_hash_table_remove(ht_node_info_changed, GUINT_TO_POINTER(n));
    g_hash_table_remove(ht_node_flags_changed, GUINT_TO_POINTER(n));
    g_hash_table_remove(ht_pending_lookups, GUINT_TO_POINTER(n));

	data = find_node(n);
	g_assert(NULL != data);
	g_assert(n == data->handle);

	gtk_tree_store_remove(nodes_model, &data->iter);
	g_hash_table_remove(nodes_handles, GUINT_TO_POINTER(n));
}

/**
 * Adds the given node to the gui.
 */
void
nodes_gui_add_node(gnet_node_info_t *info)
{
	static const struct node_data zero_data;
	struct node_data *data;
	GtkTreeIter iter;
	static const guint columns[] = {
		c_gnet_flags,
		c_gnet_info,
		c_gnet_loc,
		c_gnet_connected,
		c_gnet_uptime,
		c_gnet_version,
	};
	guint i;

    g_assert(info != NULL);

	data = walloc(sizeof *data);
	*data = zero_data;

    gtk_tree_store_append(nodes_model, &data->iter, NULL);
    gtk_tree_store_set(nodes_model, &data->iter, 0, data, (-1));

	data->handle = info->node_handle;
	data->user_agent = info->vendor ? atom_str_get(info->vendor) : NULL;
	data->country = info->country;
	data->host_size = w_concat_strings(&data->host,
						host_addr_port_to_string(info->addr, info->port),
						(void *) 0);
	gm_snprintf(data->version, sizeof data->version, "%u.%u",
		info->proto_major, info->proto_minor);
	
	for (i = 0; i < G_N_ELEMENTS(columns); i++) {
    	gtk_tree_store_append(nodes_model, &iter, &data->iter);
    	gtk_tree_store_set(nodes_model, &iter,
			0, GUINT_TO_POINTER(columns[i]), (-1));
	}

	g_hash_table_insert(nodes_handles, GUINT_TO_POINTER(data->handle), data);
}


static inline void
update_row(gpointer key, gpointer value, gpointer user_data)
{
	struct node_data *data = value;
	time_t *now_ptr = user_data, now = *now_ptr;
	const gchar *s;
	size_t size;
	gnet_node_status_t status;
	gnet_node_t n;

	g_assert(NULL != data);
	g_assert(GUINT_TO_POINTER(data->handle) == key);

	n = data->handle;
	guc_node_get_status(n, &status);

    /*
     * Update additional info too if it has recorded changes.
     */
    if (
		g_hash_table_lookup(ht_node_info_changed, GUINT_TO_POINTER(n))
	) {
        gnet_node_info_t info;

        g_hash_table_remove(ht_node_info_changed, GUINT_TO_POINTER(n));
        guc_node_fill_info(n, &info);
        nodes_gui_update_node_info(data, &info);
        guc_node_clear_info(&info);
    }

    if (g_hash_table_lookup(ht_node_flags_changed, GUINT_TO_POINTER(n))) {
        gnet_node_flags_t flags;

        g_hash_table_remove(ht_node_flags_changed, GUINT_TO_POINTER(n));
        guc_node_fill_flags(n, &flags);
        nodes_gui_update_node_flags(data, &flags);
    }

	if (status.connect_date)
		data->connected = delta_time(now, status.connect_date);

	if (status.up_date)
		data->uptime = delta_time(now, status.up_date);

	s = nodes_gui_common_status_str(&status);
	size = 1 + strlen(s);
	if (size > data->info_size) {
		WFREE_NULL(data->info, data->info_size);
		data->info = wcopy(s, size);
		data->info_size = size;
	} else {
		memcpy(data->info, s, size);
	}
}

/**
 * Update all the nodes at the same time.
 *
 * @bug
 * FIXME: we should remember for every node when it was last
 *       updated and only refresh every node at most once every
 *       second. This information should be kept in a struct pointed
 *       to by the row user_data and should be automatically freed
 *       when removing the row (see upload stats code).
 */

void
nodes_gui_update_nodes_display(time_t now)
{
#define DO_FREEZE FALSE
	static const gboolean do_freeze = DO_FREEZE;
    static time_t last_update = 0;
	gint current_page;
	static GtkNotebook *notebook = NULL;
    GtkTreeModel *model;

	if (gui_debug > 0) {
    	g_message("recorded changed: flags: %d info: %d",
        	g_hash_table_size(ht_node_flags_changed),
        	g_hash_table_size(ht_node_info_changed));
	}

    if (delta_time(now, last_update) < 2)
        return;

	/*
	 * Usually don't perform updates if nobody is watching.  However,
	 * we do need to perform periodic cleanup of dead entries or the
	 * memory usage will grow.  Perform an update every UPDATE_MIN minutes
	 * at least.
	 *		--RAM, 28/12/2003
	 */

	if (notebook == NULL)
		notebook = GTK_NOTEBOOK(lookup_widget(main_window, "notebook_main"));

    current_page = gtk_notebook_get_current_page(notebook);
	if (
		current_page != nb_main_page_gnet &&
		delta_time(now, last_update) < UPDATE_MIN
	) {
		return;
	}

    last_update = now;

	if (do_freeze) {
    	/* "Freeze" view */
    	model = gtk_tree_view_get_model(treeview_nodes);
    	g_object_ref(model);
    	gtk_tree_view_set_model(treeview_nodes, NULL);
	} else {
		model = NULL; /* dumb compiler */
	}

	g_hash_table_foreach(nodes_handles, update_row, &now);

	if (do_freeze) {
    	/* "Thaw" view */
    	gtk_tree_view_set_model(treeview_nodes, model);
    	g_object_unref(model);
	} else {
    	gtk_widget_queue_draw(GTK_WIDGET(treeview_nodes));
	}
}

/***
 *** Callbacks
 ***/

/**
 * Callback: called when a node is removed from the backend.
 *
 * Removes all references to the node from the frontend.
 */
static void
nodes_gui_node_removed(gnet_node_t n)
{
    if (gui_debug >= 5)
        g_warning("nodes_gui_node_removed(%u)\n", (guint) n);

    nodes_gui_remove_node(n);
}

/**
 * Callback: called when a node is added from the backend.
 *
 * Adds the node to the gui.
 */
static void nodes_gui_node_added(gnet_node_t n)
{
    gnet_node_info_t *info;

    if (gui_debug >= 5)
        g_warning("nodes_gui_node_added(%u)\n", n);

    info = guc_node_get_info(n);
    nodes_gui_add_node(info);
    guc_node_free_info(info);
}

/**
 * Callback: called when node information was changed by the backend.
 *
 * This updates the node information in the gui.
 */
static void
nodes_gui_node_info_changed(gnet_node_t n)
{
    g_hash_table_insert(ht_node_info_changed,
        GUINT_TO_POINTER(n), GUINT_TO_POINTER(1));
}

/**
 * Callback invoked when the node's user-visible flags are changed.
 */
static void
nodes_gui_node_flags_changed(gnet_node_t n)
{
    g_hash_table_insert(ht_node_flags_changed,
        GUINT_TO_POINTER(n), GUINT_TO_POINTER(1));
}

/**
 * Removes all selected nodes from the treeview and disconnects them.
 */
void
nodes_gui_remove_selected(void)
{
	GtkTreeView *treeview;
	GtkTreeSelection *selection;
	GSList *node_list = NULL;

	treeview = GTK_TREE_VIEW(lookup_widget(main_window, "treeview_nodes"));
	selection = gtk_tree_view_get_selection(treeview);
	gtk_tree_selection_selected_foreach(selection,
		nodes_gui_remove_selected_helper, &node_list);
	guc_node_remove_nodes_by_handle(node_list);
	g_slist_free(node_list);
}

static inline void
nodes_gui_reverse_lookup_selected_helper(GtkTreeModel *model,
		GtkTreePath *unused_path, GtkTreeIter *iter, gpointer unused_data)
{
	struct node_data *data;
	gnet_node_info_t info;
	GtkTreeIter parent;
	gboolean has_parent;
	gpointer key;

	(void) unused_path;
	(void) unused_data;

	has_parent = gtk_tree_model_iter_parent(model, &parent, iter);
	gtk_tree_model_get(model, has_parent ? &parent : iter, 0, &data, (-1));
	g_assert(NULL != find_node(data->handle));

	key = GUINT_TO_POINTER(data->handle);
	if (NULL != g_hash_table_lookup(ht_pending_lookups, key))
		return;

	guc_node_fill_info(data->handle, &info);
	g_assert(data->handle == info.node_handle);

	WFREE_NULL(data->host, data->host_size);
	data->host_size = w_concat_strings(&data->host,
		_("Reverse lookup in progress..."),
		" (", host_addr_port_to_string(info.addr, info.port), ")",
		(void *) 0);

	g_hash_table_insert(ht_pending_lookups, key, GINT_TO_POINTER(1));
	adns_reverse_lookup(info.addr, host_lookup_callback, key);
	guc_node_clear_info(&info);
}

/**
 * Performs a reverse lookup for all selected nodes.
 */
void
nodes_gui_reverse_lookup_selected(void)
{
	GtkTreeView *treeview;
	GtkTreeSelection *selection;

	treeview = GTK_TREE_VIEW(lookup_widget(main_window, "treeview_nodes"));
	selection = gtk_tree_view_get_selection(treeview);
	gtk_tree_selection_selected_foreach(selection,
		nodes_gui_reverse_lookup_selected_helper, NULL);
}

/* vi: set ts=4 sw=4 cindent: */
