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
#include "gtk/statusbar.h"
#include "gtk/visual_progress.h"
#include "gtk/columns.h"
#include "gtk/gtk-missing.h"

#include "if/gui_property_priv.h"
#include "if/bridge/ui2c.h"

#include "lib/glib-missing.h"
#include "lib/override.h"		/* Must be the last header included */

static gnet_fi_t last_shown = 0;
static gboolean  last_shown_valid = FALSE;

/*
 * Together visible_fi and hidden_fi are a list of all fileinfo handles
 * the the gui knows about.
 */
static GSList *visible_fi = NULL;
static GSList *hidden_fi  = NULL;

static regex_t filter_re;

void
on_clist_fileinfo_resize_column(GtkCList *unused_clist,
	gint column, gint width, gpointer unused_udata)
{
	(void) unused_clist;
	(void) unused_udata;
    *(gint *) &file_info_col_widths[column] = width;
}

/* Cache for fi_gui_fill_info. This is global so it can be freed
 * when fi_gui_shutdown is called. */
static gnet_fi_info_t *last_fi = NULL;

/*
 * fi_gui_fill_info:
 *
 * Fill in the cell data. Calling this will always break the data
 * it filled in last time!
 *
 * WARNING: returns pointer to global data: the gnet_fi_info_t structure
 * filled from the given `fih'.
 */
static gnet_fi_info_t *
fi_gui_fill_info(gnet_fi_t fih, gchar *titles[c_fi_num])
{
    /* Clear info from last call. We keep this around so we don't
     * have to strdup entries from it when passing them to the
     * outside through titles[]. */

    if (last_fi != NULL)
        guc_fi_free_info(last_fi);

    /* Fetch new info */
    last_fi = guc_fi_get_info(fih);
    g_assert(last_fi != NULL);

    titles[c_fi_filename] = last_fi->file_name;

	return last_fi;
}

static void
fi_gui_fill_status(gnet_fi_t fih, gchar *titles[c_fi_num])
{
    static gchar fi_sources[32];
    static gchar fi_status[256];
    static gchar fi_done[SIZE_FIELD_MAX+10];
    static gchar fi_size[SIZE_FIELD_MAX];
    gnet_fi_status_t s;

    guc_fi_get_status(fih, &s);

    gm_snprintf(fi_sources, sizeof(fi_sources), "%d/%d/%d",
        s.recvcount, s.aqueued_count+s.pqueued_count, s.lifecount);
    titles[c_fi_sources] = fi_sources;

    if (s.done) {
        gm_snprintf(fi_done, sizeof(fi_done), "%s (%.1f%%)",
            short_size(s.done), ((float) s.done / s.size)*100.0);
        titles[c_fi_done] = fi_done;
    } else {
        titles[c_fi_done] = "-";
    }

    g_strlcpy(fi_size, short_size(s.size), sizeof(fi_size));
    titles[c_fi_size]    = fi_size;

    if (s.recvcount) {
        gm_snprintf(fi_status, sizeof(fi_status),
            _("Downloading (%.1f k/s)"), s.recv_last_rate / 1024.0);
        titles[c_fi_status] = fi_status;
    } else if (s.done == s.size){
        titles[c_fi_status] = _("Finished");
    } else if (s.lifecount == 0) {
        titles[c_fi_status] = _("No sources");
    } else if (s.aqueued_count || s.pqueued_count) {
        gm_snprintf(fi_status, sizeof(fi_status),
            _("Queued (%d active/ %d passive)"),
            s.aqueued_count, s.pqueued_count);
        titles[c_fi_status] = fi_status;
    } else {
        titles[c_fi_status] = _("Waiting");
    }
}

/**
 * Display details for the given fileinfo entry in the details pane.
 * It is expected, that the given handle is really used. If not, an
 * assertion will be triggered.
 */
static void
fi_gui_set_details(gnet_fi_t fih)
{
    gnet_fi_info_t *fi = NULL;
    gnet_fi_status_t fis;
    gchar **aliases;
    guint n;
    GtkCList *cl_aliases;
	gboolean in_progress = FALSE;
	gchar bytes[32];

    fi = guc_fi_get_info(fih);
    g_assert(fi != NULL);

    guc_fi_get_status(fih, &fis);
    aliases = guc_fi_get_aliases(fih);

    cl_aliases = GTK_CLIST(lookup_widget(main_window, "clist_fi_aliases"));

    gtk_label_set_text(
        GTK_LABEL(lookup_widget(main_window, "label_fi_filename")),
        fi->file_name);
	gm_snprintf(bytes, sizeof bytes, "%" PRIu64, (guint64) fis.size);
    gtk_label_printf(
        GTK_LABEL(lookup_widget(main_window, "label_fi_size")),
        _("%s (%s bytes)"), short_size(fis.size), bytes);

    gtk_clist_freeze(cl_aliases);
    gtk_clist_clear(cl_aliases);
    for (n = 0; aliases[n] != NULL; n++)
        gtk_clist_append(cl_aliases, &aliases[n]);
    gtk_clist_thaw(cl_aliases);

	in_progress = (fis.done != fis.size);

    g_strfreev(aliases);
    guc_fi_free_info(fi);

    last_shown = fih;
    last_shown_valid = TRUE;

	vp_draw_fi_progress(last_shown_valid, last_shown);

	gtk_widget_set_sensitive(lookup_widget(main_window, "button_fi_purge"),
							 in_progress);
}

/**
 * Clear the details pane.
 */
static void
fi_gui_clear_details(void)
{
    last_shown_valid = FALSE;

    gtk_label_set_text(
        GTK_LABEL(lookup_widget(main_window, "label_fi_filename")),
        "");
    gtk_label_set_text(
        GTK_LABEL(lookup_widget(main_window, "label_fi_size")),
        "");
    gtk_clist_clear(
        GTK_CLIST(lookup_widget(main_window, "clist_fi_aliases")));
    gtk_widget_set_sensitive(lookup_widget(main_window, "button_fi_purge"),
        FALSE);

    vp_draw_fi_progress(last_shown_valid, last_shown);
}

/**
 * @return TRUE if the given string matches with the currntly set
 * row filter. Returns FALSE otherwise.
 */
static inline gboolean
fi_gui_match_filter(const gchar *s)
{
    gint n;

    n = regexec(&filter_re, s, 0, NULL, 0);

    if (n == REG_ESPACE) {
        g_warning("fi_gui_match_filter: regexp memory overflow");
    }

    return n == 0;
}

/**
 * Add a fileinfo entry to the list if it matches the currently set
 * row filter. visible_fi and hidden_fi are properly updated wether
 * the entry is displayed or not and no matter if the line was already
 * shown/hidden or is newly added.
 */
static void
fi_gui_add_row(gnet_fi_t fih)
{
    GtkCList *clist;
    gint row;
    guint n;
    gchar *titles[c_fi_num];
	gnet_fi_info_t *info;
	gboolean filter_match;
	GSList *l;

    memset(titles, 0, sizeof(titles));
    info = fi_gui_fill_info(fih, titles);

    /*
	 * If the entry doesn't match the filter, register it as hidden and
     * return.
	 */

	filter_match = fi_gui_match_filter(info->file_name);

	for (l = info->aliases; !filter_match && l; l = g_slist_next(l)) {
		const gchar *alias = (const gchar *) l->data;
		filter_match = fi_gui_match_filter(alias);
	}

    if (!filter_match) {
        if (!g_slist_find(hidden_fi, GUINT_TO_POINTER(fih))) {
            hidden_fi = g_slist_prepend(hidden_fi, GUINT_TO_POINTER(fih));
            visible_fi = g_slist_remove(visible_fi, GUINT_TO_POINTER(fih));
        }
        return;
    }

    visible_fi = g_slist_prepend(visible_fi, GUINT_TO_POINTER(fih));
    hidden_fi = g_slist_remove(hidden_fi, GUINT_TO_POINTER(fih));

    fi_gui_fill_status(fih, titles);

    clist = GTK_CLIST(lookup_widget(main_window, "clist_fileinfo"));

    for (n = 0; n < G_N_ELEMENTS(titles); n ++) {
        if (titles[n] == NULL)
            titles[n] = "";
    }

    row = gtk_clist_append(clist, titles);
    gtk_clist_set_row_data(clist, row, GUINT_TO_POINTER(fih));
}

/**
 * Remove a fileinfo entry from the list. If it is not displayed, then
 * nothing happens. If hide is TRUE, then the row is not unregistered
 * and only moved to the hidden_fi list.
 */
static void
fi_gui_remove_row(gnet_fi_t fih, gboolean hide)
{
    GtkCList *clist;
    gint row;

    clist = GTK_CLIST(lookup_widget(main_window, "clist_fileinfo"));

    row = gtk_clist_find_row_from_data(clist, GUINT_TO_POINTER(fih));
    gtk_clist_remove(clist, row);

    if (hide) {
        visible_fi = g_slist_remove(visible_fi, GUINT_TO_POINTER(fih));
        hidden_fi  = g_slist_prepend(hidden_fi, GUINT_TO_POINTER(fih));
    } else {
        visible_fi = g_slist_remove(visible_fi, GUINT_TO_POINTER(fih));
        hidden_fi  = g_slist_remove(hidden_fi,  GUINT_TO_POINTER(fih));
    }
}

/**
 * Takes a string containing a regular expression updates the list to
 * only show files matching that expression.
 */
static void
fi_gui_set_filter_regex(gchar *s)
{
    gint err;
    GSList *sl;
    gint row;
    GSList *old_hidden = g_slist_copy(hidden_fi);
    GtkCList *clist_fi;
    char *fallback_re = ".";

    if (s == NULL) {
        s = fallback_re;
    }

    /* Recompile the row filter*/
    err = regcomp(&filter_re, s,
                  REG_EXTENDED|REG_NOSUB|(fi_regex_case ? 0 : REG_ICASE));

   	if (err) {
        gchar buf[1024];
		regerror(err, &filter_re, buf, sizeof buf);
        statusbar_gui_warning(15, "*** ERROR: %s", buf);

        /* If an error occurs turn filter off. If this doesn't work,
         * then we probably have a serious problem. */
        err = regcomp(&filter_re, fallback_re, REG_EXTENDED|REG_NOSUB);
        g_assert(!err);
    }

    clist_fi = GTK_CLIST(
        lookup_widget(main_window, "clist_fileinfo"));

    /* now really apply the filter */
    gtk_clist_unselect_all(clist_fi);
	gtk_clist_freeze(clist_fi);

    /* first remove non-matching from the list. */
    row = 0;
    while (row < clist_fi->rows) {
        gchar *text;

        if (!gtk_clist_get_text(clist_fi, row, c_fi_filename, &text)) {
            continue;
        }

        if (!fi_gui_match_filter(text)) {
            gnet_fi_t fih;

            fih = GPOINTER_TO_UINT(gtk_clist_get_row_data(clist_fi, row));
            fi_gui_remove_row(fih, TRUE); /* decreases clist_fi->rows */
        } else {
            row ++;
        }
    }

    /* now add matching hidden to list */
    for (sl = old_hidden; NULL != sl; sl = g_slist_next(sl)) {
        /* We simply try to add all hidden rows. If they match
         * the new filter they will be unhidden */
        fi_gui_add_row(GPOINTER_TO_UINT(sl->data));
    }

    gtk_clist_thaw(clist_fi);
}

static void
fi_gui_update(gnet_fi_t fih, gboolean full)
{
    GtkCList *clist;
	gchar    *titles[c_fi_num];
    gint      row;
    guint     n;

    clist = GTK_CLIST(lookup_widget(main_window, "clist_fileinfo"));

    row = gtk_clist_find_row_from_data(clist, GUINT_TO_POINTER(fih));
    if (row == -1) {
        /* This can happen if we get an update event for a hidden row. */
        return;
    }

    memset(titles, 0, sizeof(titles));
    if (full)
        fi_gui_fill_info(fih, titles);
    fi_gui_fill_status(fih, titles);

    for (n = 0; n < G_N_ELEMENTS(titles); n ++) {
        if (titles[n] != NULL)
            gtk_clist_set_text(clist, row, n, titles[n]);
    }

    /*
     * If this entry is currently selected we should also update the progress
     */
	vp_draw_fi_progress(last_shown_valid, last_shown);
}

static void
fi_gui_fi_added(gnet_fi_t fih)
{
    fi_gui_add_row(fih);
}

static void
fi_gui_fi_removed(gnet_fi_t fih)
{
	if (fih == last_shown) {
		last_shown_valid = FALSE;
	}

    fi_gui_remove_row(fih, FALSE);
}

static void
fi_gui_fi_status_changed(gnet_fi_t fih)
{
    fi_gui_update(fih, FALSE);
}

void
on_clist_fileinfo_select_row(GtkCList *clist, gint row, gint unused_column,
    GdkEvent *unused_event, gpointer unused_udata)
{
    gnet_fi_t fih;

	(void) unused_column;
	(void) unused_event;
	(void) unused_udata;
    fih = GPOINTER_TO_UINT(gtk_clist_get_row_data(clist, row));
    fi_gui_set_details(fih);
}

void
on_clist_fileinfo_unselect_row(GtkCList *clist, gint unused_row,
	gint unused_column, GdkEvent *unused_event, gpointer unused_udata)
{
	(void) unused_row;
	(void) unused_column;
	(void) unused_event;
	(void) unused_udata;
    if (clist->selection == NULL)
        fi_gui_clear_details();
}

void
on_button_fi_purge_clicked(GtkButton *unused_button, gpointer unused_udata)
{
    GSList *sl_handles = NULL;
    GtkCList *clist = GTK_CLIST(
        lookup_widget(main_window, "clist_fileinfo"));

	(void) unused_button;
	(void) unused_udata;
    sl_handles = clist_collect_data(clist, TRUE, NULL);
    if (sl_handles) {
		GSList *sl;

		for (sl = sl_handles; sl != NULL; sl = g_slist_next(sl))
			if (GPOINTER_TO_UINT(sl->data) == last_shown) {
				last_shown_valid = FALSE;
				break;
			}

        guc_fi_purge_by_handle_list(sl_handles);
    }

    g_slist_free(sl_handles);
}

void
on_entry_fi_regex_activate(GtkEditable *editable, gpointer unused_udata)
{
    gchar *regex;

	(void) unused_udata;
    regex = STRTRACK(gtk_editable_get_chars(GTK_EDITABLE(editable), 0, -1));
    if (regex) {
    	fi_gui_set_filter_regex(regex);
    	G_FREE_NULL(regex);
	}
}

void
fi_gui_init(void)
{
    GtkCList *clist;

    guc_fi_add_listener(fi_gui_fi_added, EV_FI_ADDED, FREQ_SECS, 0);
    guc_fi_add_listener(fi_gui_fi_removed, EV_FI_REMOVED, FREQ_SECS, 0);
    guc_fi_add_listener(fi_gui_fi_status_changed, EV_FI_STATUS_CHANGED,
		FREQ_SECS, 0);

    clist = GTK_CLIST(lookup_widget(main_window, "clist_fileinfo"));

    gtk_clist_set_column_justification(clist,
        c_fi_size, GTK_JUSTIFY_RIGHT);

    gtk_clist_column_titles_passive(clist);

    /* Initialize the row filter */
    fi_gui_set_filter_regex(NULL);
}

void
fi_gui_shutdown(void)
{
    g_slist_free(hidden_fi);
    g_slist_free(visible_fi);

    guc_fi_remove_listener(fi_gui_fi_removed, EV_FI_REMOVED);
    guc_fi_remove_listener(fi_gui_fi_added, EV_FI_ADDED);
    guc_fi_remove_listener(fi_gui_fi_status_changed, EV_FI_STATUS_CHANGED);

    if (last_fi != NULL)
        guc_fi_free_info(last_fi);

    regfree(&filter_re);
}

/**
 * Update all the fileinfo at the same time.
 */

/* FIXME: we should remember for every node when it was last
 *        updated and only refresh every node at most once every
 *        second. This information should be kept in a struct pointed
 *        to by the row user_data and should be automatically freed
 *        when removing the row (see upload stats code).
 */
void
fi_gui_update_display(time_t now)
{
#if 0
    static time_t last_update = 0;
	GtkCList *clist;
	GList *l;
	gint row = 0;

    if (last_update == now)
        return;

    last_update = now;

    clist = GTK_CLIST(lookup_widget(main_window, "clist_fileinfo"));
    gtk_clist_freeze(clist);

	for (l = clist->row_list, row = 0; l; l = l->next, row++) {
        gchar *titles[c_fi_num];
		gnet_fi_t fih = (gnet_fi_t) GPOINTER_TO_UINT(
            ((GtkCListRow *) l->data)->data);

        memset(titles, 0, sizeof(titles));
        fi_gui_fill_status(fih, titles);
        fi_gui_update_row(clist, row, titles, G_N_ELEMENTS(titles));
    }
    gtk_clist_thaw(clist);
#else
	(void) now;
#endif
}

/* vi: set ts=4 sw=4 cindent: */
