/*
 * $Id$
 *
 * Copyright (c) 2003, Richard Eckart
 *
 * Displaying of file information in the gui.
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

#include "gtk/filter.h"
#include "gtk/visual_progress.h"
#include "gtk/columns.h"
#include "gtk/gtk-missing.h"

#include "if/gui_property.h"
#include "if/gui_property_priv.h"
#include "if/bridge/ui2c.h"

#include "lib/utf8.h"
#include "lib/glib-missing.h"
#include "lib/override.h"		/* Must be the last header included */


static gnet_fi_t last_shown = 0;
static gboolean  last_shown_valid = FALSE;

static GtkTreeView *treeview_fileinfo = NULL;
static GtkTreeView *treeview_fi_aliases = NULL;
static GtkEntry *entry_fi_filename = NULL;
static GtkLabel *label_fi_size = NULL;

static GtkTreeStore *store_fileinfo = NULL;
static GtkTreeStore *store_aliases = NULL;
static GHashTable *fi_gui_handles = NULL;

static GHashTable *fi_updates = NULL;

static void
fi_gui_update_row(GtkTreeStore *store, GtkTreeIter *iter, gchar **titles)
{		
	if (NULL != titles[c_fi_filename]) {
		gtk_tree_store_set(store, iter,
			c_fi_filename, lazy_locale_to_utf8(titles[c_fi_filename], 0),
			(-1));
	}
	gtk_tree_store_set(store, iter,
		c_fi_size, titles[c_fi_size],
		c_fi_done, titles[c_fi_done],
		c_fi_sources, titles[c_fi_sources],
		c_fi_status, titles[c_fi_status],
		c_fi_isize, *(guint64 *) titles[c_fi_isize],
		c_fi_idone, GPOINTER_TO_UINT(titles[c_fi_idone]),
		c_fi_isources, GPOINTER_TO_UINT(titles[c_fi_isources]),
		(-1));
}

static void
fi_gui_set_details(gnet_fi_t fih)
{
    gnet_fi_info_t *fi = NULL;
    gnet_fi_status_t fis;
    gchar **aliases;
	GtkTreeIter iter;
	gint i;

    fi = guc_fi_get_info(fih);
    g_assert(fi != NULL);

    guc_fi_get_status(fih, &fis);
    aliases = guc_fi_get_aliases(fih);

    gtk_entry_set_text(entry_fi_filename,
		lazy_locale_to_utf8(fi->file_name, 0));
    gtk_label_printf(label_fi_size, _("%s (%" PRIu64 " bytes)"),
		short_size(fis.size), fis.size);

    gtk_tree_store_clear(store_aliases);
	for (i = 0; NULL != aliases[i]; i++) {
		gtk_tree_store_append(store_aliases, &iter, NULL);
		gtk_tree_store_set(store_aliases, &iter, 0,
			lazy_locale_to_utf8(aliases[i], 0), (-1));
	}
    g_strfreev(aliases);
    guc_fi_free_info(fi);

    last_shown = fih;
    last_shown_valid = TRUE;

	vp_draw_fi_progress(last_shown_valid, last_shown);

    gtk_widget_set_sensitive(lookup_widget(main_window, "button_fi_purge"),
			     TRUE);
}

static void
fi_gui_clear_details(void)
{
    last_shown_valid = FALSE;

    gtk_entry_set_text(entry_fi_filename, "");
    gtk_label_set_text(label_fi_size, "");
    gtk_tree_store_clear(store_aliases);

    gtk_widget_set_sensitive(
        lookup_widget(main_window, "button_fi_purge"), FALSE);

    vp_draw_fi_progress(last_shown_valid, last_shown);
}

void
on_treeview_fileinfo_selected(GtkTreeView *unused_tv, gpointer unused_udata)
{
	GtkTreeSelection *selection;
	GtkTreeModel *model;
	GtkTreeIter iter;
    gnet_fi_t fih;

	(void) unused_tv;
	(void) unused_udata;
	
    selection = gtk_tree_view_get_selection(treeview_fileinfo);
    if (gtk_tree_selection_get_selected(selection, &model, &iter)) {
		gtk_tree_model_get(model, &iter, c_fi_handle, &fih, (-1));
    	fi_gui_set_details(fih);
    } else
	fi_gui_clear_details();
}

/**
 * Handle the clicking of the purge button.  Purge the selected file.
 */
void
on_button_fi_purge_clicked(GtkButton *unused_button, gpointer unused_udata)
{
	(void) unused_button;
	(void) unused_udata;
	
    if (last_shown_valid) {
		guc_fi_purge(last_shown);
		fi_gui_clear_details();
    }
}

static void
fi_gui_append_row(GtkTreeStore *store, gnet_fi_t fih, gchar **titles)
{
	GtkTreeIter iter;

	gtk_tree_store_append(store, &iter, NULL);
	g_hash_table_insert(fi_gui_handles,
        GUINT_TO_POINTER(fih), w_tree_iter_copy(&iter));
	gtk_tree_store_set(store, &iter, c_fi_handle, fih, (-1));
	fi_gui_update_row(store, &iter, titles);
}

/**
 * Fill in the cell data. Calling this will always break the data
 * it filled in last time!
 */
static void
fi_gui_fill_info(gnet_fi_t fih, gchar *titles[c_fi_num])
{
    static gnet_fi_info_t *fi = NULL;

    /* Clear info from last call. We keep this around so we don't
     * have to strdup entries from it when passing them to the 
     * outside through titles[]. */
    if (fi != NULL) {
        guc_fi_free_info(fi);
    }
        
    /* Fetch new info */
    fi = guc_fi_get_info(fih);
    g_assert(fi != NULL);

    titles[c_fi_filename] = fi->file_name;
}

static void
fi_gui_fill_status(gnet_fi_t fih, gchar *titles[c_fi_num])
{
    gnet_fi_status_t s;
    static gchar fi_sources[32];
    static gchar fi_status[256];
    static gchar fi_done[SIZE_FIELD_MAX+10];
    static gchar fi_size[SIZE_FIELD_MAX];
    static guint64 isize;
	guint idone;

    guc_fi_get_status(fih, &s);

    gm_snprintf(fi_sources, sizeof(fi_sources), "%d/%d/%d",
        s.recvcount, s.aqueued_count+s.pqueued_count, s.lifecount);
    titles[c_fi_sources] = fi_sources;
    titles[c_fi_isources] = GUINT_TO_POINTER(s.refcount);

    if (s.done && s.size) {
		gfloat done = ((gfloat) s.done / s.size) * 100.0;
 
        gm_snprintf(fi_done, sizeof(fi_done), "%s (%.1f%%)", 
            short_size(s.done), done);
        titles[c_fi_done] = fi_done;
		idone = done * ((1 << 30) / 101);
    } else {
        titles[c_fi_done] = "-";
		idone = 0;
    }
        
    g_strlcpy(fi_size, short_size(s.size), sizeof(fi_size));
	titles[c_fi_size]  = fi_size;
	isize = s.size;
    titles[c_fi_isize] = (gpointer) &isize;
    titles[c_fi_idone] = GUINT_TO_POINTER(idone);

    if (s.recvcount) {
		gm_snprintf(fi_status, sizeof(fi_status), 
            _("Downloading (%.1f k/s)"), s.recv_last_rate / 1024.0);
        titles[c_fi_status] = fi_status;
    } else if (s.done == s.size) {
        titles[c_fi_status] = _("Finished");
    } else if (s.lifecount == 0) {
        titles[c_fi_status] = _("No sources");
    } else if (s.aqueued_count || s.pqueued_count) {
        gm_snprintf(fi_status, sizeof(fi_status), 
            _("Queued (%d active, %d passive)"),
            s.aqueued_count, s.pqueued_count);
        titles[c_fi_status] = fi_status;
    } else {
        titles[c_fi_status] = _("Waiting");
    }
    titles[c_fi_handle] = GUINT_TO_POINTER(fih);
}

static void
fi_gui_update(gnet_fi_t fih, gboolean full)
{
	GtkTreeIter *iter;
	gchar *titles[c_fi_num];

	if (
		!g_hash_table_lookup_extended(fi_gui_handles, GUINT_TO_POINTER(fih),
			NULL, (gpointer) &iter)
	) {
        g_warning("fi_gui_update: no matching iter found");
        return;
    }

    memset(titles, 0, sizeof(titles));
    if (full)
        fi_gui_fill_info(fih, titles);
    fi_gui_fill_status(fih, titles);

    fi_gui_update_row(store_fileinfo, iter, titles);

	vp_draw_fi_progress(last_shown_valid, last_shown);
}

static void
fi_gui_fi_added(gnet_fi_t fih)
{
	gchar *titles[c_fi_num];

    memset(titles, 0, sizeof(titles));
    fi_gui_fill_info(fih, titles);
    fi_gui_fill_status(fih, titles);

    fi_gui_append_row(store_fileinfo, fih, titles);
}

static void
fi_gui_fi_removed(gnet_fi_t fih)
{
	GtkTreeIter *iter;
	
	g_hash_table_remove(fi_updates, GUINT_TO_POINTER(fih));
	
	if (
		!g_hash_table_lookup_extended(fi_gui_handles, GUINT_TO_POINTER(fih),
			NULL, (gpointer) &iter)
	) {
        g_warning("fi_gui_fi_removed: no matching iter found");
        return;
    }

    gtk_tree_store_remove(store_fileinfo, iter);
	g_hash_table_remove(fi_gui_handles, GUINT_TO_POINTER(fih));
}

static void
fi_gui_fi_status_changed(gnet_fi_t fih)
{
	g_hash_table_insert(fi_updates, GUINT_TO_POINTER(fih), GINT_TO_POINTER(1));
}

static gboolean 
fi_gui_update_queued(gpointer key, gpointer unused_value, gpointer unused_udata)
{
	gnet_fi_t fih = GPOINTER_TO_UINT(key);

	(void) unused_value;
	(void) unused_udata;

  	fi_gui_update(fih, FALSE);
	return TRUE; /* Remove the handle from the hashtable */
}

static gint
compare_uint_func(GtkTreeModel *model, GtkTreeIter *i, GtkTreeIter *j,
		gpointer user_data)
{
	guint a, b;

    gtk_tree_model_get(model, i, GPOINTER_TO_INT(user_data), &a, (-1));
    gtk_tree_model_get(model, j, GPOINTER_TO_INT(user_data), &b, (-1));
    return CMP(b, a);
}

static gint
compare_uint64_func(GtkTreeModel *model, GtkTreeIter *i, GtkTreeIter *j,
		gpointer user_data)
{
	guint64 a, b;

    gtk_tree_model_get(model, i, GPOINTER_TO_INT(user_data), &a, (-1));
    gtk_tree_model_get(model, j, GPOINTER_TO_INT(user_data), &b, (-1));
    return CMP(b, a);
}

static void add_column(
    GtkTreeView *tree,
	gint column_id,
	const gchar *title,
	gint width,
	gfloat xalign)
{
    GtkTreeViewColumn *column;
	GtkCellRenderer *renderer;

	renderer = gtk_cell_renderer_text_new();
    gtk_cell_renderer_text_set_fixed_height_from_font(
        GTK_CELL_RENDERER_TEXT(renderer), 1);
    g_object_set(renderer,
        "mode", GTK_CELL_RENDERER_MODE_INERT,
        "xalign", xalign,
        "ypad", GUI_CELL_RENDERER_YPAD,
        NULL);
    column = gtk_tree_view_column_new_with_attributes(
        title, renderer, "text", column_id, NULL);
	g_object_set(G_OBJECT(column),
		"fixed-width", MAX(1, width),
		"min-width", 1,
		"reorderable", TRUE,
		"resizable", TRUE,
		"sizing", GTK_TREE_VIEW_COLUMN_FIXED,
		NULL);

	gtk_tree_view_column_set_sort_column_id(column, column_id);
    gtk_tree_view_append_column(GTK_TREE_VIEW (tree), column);
}

void
fi_gui_update_display(time_t now) 
{
	static time_t last;

	if (!last || delta_time(now, last) > 3) {
		last = now;

		g_hash_table_foreach_remove(fi_updates, fi_gui_update_queued, NULL);
	}
}

void
fi_gui_init(void) 
{
	static const struct {
		const gint id;
		const gchar * const title;
		const gfloat align;
		const GtkTreeIterCompareFunc sort_func;
		const gint sort_by;
	} columns[] = {
		{ c_fi_filename, N_("File"),	0.0, NULL, -1 },
    	{ c_fi_size,	 N_("Size"),	1.0, compare_uint64_func, c_fi_isize },
    	{ c_fi_done,	 N_("Done"),	1.0, compare_uint_func, c_fi_idone },
    	{ c_fi_sources,  N_("Sources"), 1.0, compare_uint_func, c_fi_isources },
    	{ c_fi_status,   N_("Status"),	0.0, NULL, -1 }
	};
	static GType types[] = {
		G_TYPE_STRING,	/* Filename */
		G_TYPE_STRING,	/* Size		*/
		G_TYPE_STRING,	/* Done		*/
		G_TYPE_STRING,	/* Sources	*/
		G_TYPE_STRING,	/* Status	*/
		G_TYPE_UINT,	/* Fileinfo handle		*/
		G_TYPE_UINT64,	/* Size (for sorting)	*/
		G_TYPE_UINT,	/* Done (for sorting)	*/
		G_TYPE_UINT		/* Sources (for sorting */
	};
	guint32 width[G_N_ELEMENTS(columns)];
	guint i;

	STATIC_ASSERT(FILEINFO_VISIBLE_COLUMNS == G_N_ELEMENTS(columns));
	STATIC_ASSERT(c_fi_num == G_N_ELEMENTS(types));

	fi_gui_handles = g_hash_table_new_full(
        NULL, NULL, NULL, (gpointer) w_tree_iter_free);

	fi_updates = g_hash_table_new(NULL, NULL);

    treeview_fi_aliases = GTK_TREE_VIEW(lookup_widget(main_window,
		"treeview_fi_aliases"));
	treeview_fileinfo = GTK_TREE_VIEW(lookup_widget(main_window,
		"treeview_fileinfo"));
	entry_fi_filename = GTK_ENTRY(lookup_widget(main_window,
		"entry_fi_filename"));
	label_fi_size = GTK_LABEL(lookup_widget(main_window,
		"label_fi_size"));

	store_fileinfo = gtk_tree_store_newv(G_N_ELEMENTS(types), types);
	gtk_tree_view_set_model(treeview_fileinfo, GTK_TREE_MODEL(store_fileinfo));
	g_signal_connect(GTK_OBJECT(treeview_fileinfo), "cursor-changed",
        G_CALLBACK(on_treeview_fileinfo_selected), NULL);

	gui_prop_get_guint32(PROP_FILE_INFO_COL_WIDTHS, width, 0,
		G_N_ELEMENTS(width));

	for (i = 0; i < G_N_ELEMENTS(columns); i++) {
    	add_column(treeview_fileinfo, columns[i].id, _(columns[i].title),
			width[i], columns[i].align);

		if (columns[i].sort_func) {
			gtk_tree_sortable_set_sort_func(GTK_TREE_SORTABLE(store_fileinfo),
				columns[i].id,
				columns[i].sort_func,
				GINT_TO_POINTER(columns[i].sort_by),
				NULL);
		}
	}

	store_aliases = gtk_tree_store_new(1, G_TYPE_STRING);
	gtk_tree_view_set_model(treeview_fi_aliases, GTK_TREE_MODEL(store_aliases));

    add_column(treeview_fi_aliases, 0, _("Aliases"), 0.0, 0);

    guc_fi_add_listener(fi_gui_fi_added, EV_FI_ADDED, FREQ_SECS, 0);
    guc_fi_add_listener(fi_gui_fi_removed, EV_FI_REMOVED, FREQ_SECS, 0);
    guc_fi_add_listener(fi_gui_fi_status_changed, EV_FI_STATUS_CHANGED,
		FREQ_SECS, 0);
}

void
fi_gui_shutdown(void)
{
    guc_fi_remove_listener(fi_gui_fi_removed, EV_FI_REMOVED);
    guc_fi_remove_listener(fi_gui_fi_added, EV_FI_ADDED);
    guc_fi_remove_listener(fi_gui_fi_status_changed, EV_FI_STATUS_CHANGED);

	tree_view_save_widths(treeview_fileinfo, PROP_FILE_INFO_COL_WIDTHS);
	gtk_tree_store_clear(store_fileinfo);
	g_object_unref(G_OBJECT(store_fileinfo));
	gtk_tree_view_set_model(treeview_fileinfo, NULL);
	store_fileinfo = NULL;
	gtk_tree_store_clear(store_aliases);
	g_object_unref(G_OBJECT(store_aliases));
	store_aliases = NULL;
	gtk_tree_view_set_model(treeview_fi_aliases, NULL);
	g_hash_table_destroy(fi_gui_handles);
	fi_gui_handles = NULL;
	g_hash_table_destroy(fi_updates);
	fi_updates = NULL;
}

/* vi: set ts=4 sw=4 cindent: */
