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

#include "common.h"

#include "walloc.h"
#include "filter.h"
#include "filter_gui.h"
#include "search_gui.h"
#include "gtk-missing.h"

#ifdef USE_GTK2
#include "interface-glade2.h"
#else
#include "interface-glade1.h"
#endif

#include "override.h"		/* Must be the last header included */

RCSID("$Id$");

/*
 * If FILTER_HIDE_ON_CLOSE is defined, the filter dialog is only hidden
 * when the dialog is close instead the of the dialog being destroyed.
 */
#define FILTER_HIDE_ON_CLOSE

typedef struct shadow {
    filter_t *filter;
    GList *current;
    GList *removed;
    GList *added;
    gint32 refcount;
    guint16 flags;
    guint32 match_count;
    guint32 fail_count;
} shadow_t;



/*
 * Private functions prototypes
 */
static gint shadow_filter_eq(const shadow_t *a, const filter_t *b);
static shadow_t *shadow_new(filter_t *s);
static shadow_t *shadow_find(filter_t *s);
static int filter_apply(filter_t *, record_t *, filter_result_t *);
static void filter_remove_rule(filter_t *f, rule_t *r);
static void filter_free(filter_t *filter);
static void filter_refresh_display(GList *filter_list);

void dump_ruleset(GList *ruleset);
void dump_filter(filter_t *filter);
void dump_shadow(shadow_t *shadow);

/*
 * Public variables
 */
filter_t *work_filter = NULL;

/*
 * Private variables
 */
static GList *shadow_filters = NULL;
static gchar f_tmp[1024];
static GList *filters_added = NULL;
static GList *filters_removed = NULL;

/* built-in targets */
filter_t *filter_drop = NULL;
filter_t *filter_show = NULL;
filter_t *filter_download = NULL;
filter_t *filter_nodownload = NULL;
filter_t *filter_return = NULL;

/* global filters */
filter_t *filter_global_pre = NULL;
filter_t *filter_global_post = NULL;

/* not static because needed in search_xml. */
GList *filters = NULL;
GList *filters_current = NULL;


/***
 *** Implementation
 ***/
void dump_ruleset(GList *ruleset)
{
    GList *r;
    gint n = 0;

    for (r = ruleset; r != NULL; r=r->next)
        printf("       rule %3d : %s\n", n, filter_rule_to_gchar(r->data));
}

void dump_filter(filter_t *filter)
{
    g_assert(filter != NULL);
    printf("Filter name     : %s\n", filter->name);
    printf("       bound    : %p\n", filter->search);
    printf("       refcount : %d\n", filter->refcount);
    dump_ruleset(filter->ruleset);
}

void dump_shadow(shadow_t *shadow)
{
    g_assert(shadow != NULL);
    printf("Shadow for filt : %s\n", shadow->filter->name);
    printf("       bound    : %p\n", shadow->filter->search);
    printf("       refcount : %d\n", shadow->refcount);
    printf("       flt. ref : %d\n", shadow->filter->refcount);
    printf("  Added:\n");
    dump_ruleset(shadow->added);
    printf("  Removed:\n");
    dump_ruleset(shadow->removed);
    printf("  Current:\n");
    dump_ruleset(shadow->current);
    printf("  Original:\n");
    dump_ruleset(shadow->filter->ruleset);
}



/*
 * shadow_filter_eq:
 *
 * Comparator function to match a shadow and a filter.
 */
static gint shadow_filter_eq(const shadow_t *a, const filter_t *b)
{
    if((a != NULL) && (b != NULL)) {
        if(a->filter == b)
            return 0;
    }

    return 1;
}



/*
 * shadow_find:
 *
 * Get the shadow for the given filter. Returns NULL if the filter
 * does not have a shadow yet.
 */
static shadow_t *shadow_find(filter_t *f)
{
    GList * l;

    g_assert(f != NULL);

    l = g_list_find_custom
        (shadow_filters, f, (GCompareFunc) shadow_filter_eq);

    if (l != NULL) {
        if (gui_debug >= 6)
            printf("shadow found for: %s\n", f->name);
        return l->data;
    } else {
        if (gui_debug >= 6)
            printf("no shadow found for: %s\n", f->name);
        return NULL;
    }
}



/*
 * shadow_new:
 *
 * Creates a new shadow for a given filter and registers it with
 * our current shadow list.
 */
static shadow_t *shadow_new(filter_t *f)
{
    shadow_t *shadow;

    g_assert(f != NULL);
    g_assert(f->name != NULL);

    if (gui_debug >= 6)
        printf("creating shadow for: %s\n", f->name);

    shadow = g_new0(shadow_t, 1);

    shadow->filter   = f;
    shadow->current  = g_list_copy(f->ruleset);
    shadow->added    = NULL;
    shadow->removed  = NULL;
    shadow->refcount = f->refcount;
    shadow->flags    = f->flags;

    shadow_filters = g_list_append(shadow_filters, shadow);

    return shadow;
}



/*
 * shadow_cancel:
 *
 * Forgets all about a given shadow and free's ressourcs for it.
 * At this point we can no longer assume that the shadow->current
 * field contains a valid pointer. We may have been called to 
 * clean up a shadow for a filter whose ruleset has already been
 * cleared. We don't clean up any memory that is owned by the 
 * associated filter.
 */
static void shadow_cancel(shadow_t *shadow)
{
    GList *r;

    g_assert(shadow != NULL);
    g_assert(shadow->filter != NULL);

    if (gui_debug >= 6)
        printf("cancel shadow for filter: %s\n", shadow->filter->name);

    for (r = shadow->added; r != NULL; r = r->next)
        filter_free_rule(r->data);

    /* 
     * Since we cancel the shadow, we also free the added,
     * removed and current lists now. Then we remove the shadow
     * kill it also.
     */
    g_list_free(shadow->removed);
    g_list_free(shadow->added);
    g_list_free(shadow->current);
    shadow->removed = shadow->added = shadow->current = NULL;

    shadow_filters = g_list_remove(shadow_filters, shadow);
    G_FREE_NULL(shadow);
}



/*
 * shadow_commit:
 *
 * Commit all the changes for a given shadow and then forget and free
 * it.
 */
static void shadow_commit(shadow_t *shadow)
{
    GList *f;
    filter_t *realf;

    g_assert(shadow != NULL);
    g_assert(shadow->filter != NULL); 

    realf = shadow->filter;

    if (gui_debug >= 6) {
        printf("committing shadow for filter:\n");
        dump_shadow(shadow);
    }

    /*
     * Free memory for all removed rules
     */
    for (f = shadow->removed; f != NULL; f = f->next)
        filter_free_rule(f->data);

    /*
     * Remove the SHADOW flag from all new rules.
     */
    for (f = shadow->added; f != NULL; f = f->next)
        clear_flags(((rule_t*) f->data)->flags, RULE_FLAG_SHADOW);

    /* 
     * We also free the memory of the filter->ruleset GList.
     * We don't need them anymore.
     */
    g_list_free(shadow->filter->ruleset);

    /*
     * Now the actual filter is corrupted, because
     * we have freed memory its rules.
     * But we have a copy of the ruleset without exactly those 
     * rules we freed now. We use this as new ruleset.
     */
    shadow->filter->ruleset = shadow->current;

    /*
     * Not forgetting to update the refcount. There is a chance
     * that this shadow only existed because of a change in the
     * refcount.
     */
    shadow->filter->refcount = shadow->refcount;

    shadow->filter->flags = shadow->flags; 
    
    /* 
     * Now that we have actually commited the changes for this
     * shadow, we remove this shadow from our shadow list
     * and free it's ressources. Note that we do not free
     * shadow->current because this is the new filter ruleset.
     */
    g_list_free(shadow->added);
    g_list_free(shadow->removed);
    shadow->added = shadow->removed = shadow->current = NULL;
    shadow->filter = NULL;
    shadow_filters = g_list_remove(shadow_filters, shadow); 
    G_FREE_NULL(shadow);

    if (gui_debug >= 6) {
        printf("after commit filter looks like this\n");
        dump_filter(realf);
    }
}



/*
 * filter_refresh_display:
 *
 * Regenerates the filter tree and rules display from after a apply/revert.
 */
static void filter_refresh_display(GList *filter_list)
{
    GList *l;

    filter_gui_freeze_filters();
    filter_gui_filter_clear_list();
    for (l = filter_list; l != NULL; l = l->next) {
        filter_t *filter = (filter_t *)l->data;
        shadow_t *shadow;
        GList *ruleset;
        gboolean enabled;
    
        shadow = shadow_find(filter);
        ruleset = (shadow != NULL) ? shadow->current : filter->ruleset;
        enabled = (shadow != NULL) ? 
            filter_is_active(shadow) : 
            filter_is_active(filter);
                        
        filter_gui_filter_add(filter, ruleset);
        filter_gui_filter_set_enabled(filter, enabled);
    }
    filter_gui_thaw_filters();
}



/*
 * filter_open_dialog:
 *
 * Open and initialize the filter dialog.
 */
void filter_open_dialog(void) {
    search_t *current_search;

    current_search = search_gui_get_current_search();

    if (filter_dialog == NULL) {
        filter_dialog = filter_gui_create_dlg_filters();
        g_assert(filter_dialog != NULL);
   
        filter_gui_init();
        filter_refresh_display(filters_current);
    }
    
    if (current_search != NULL) {
        filter_set(current_search->filter);
    } else {
        filter_set(NULL);
    }

    filter_gui_show_dialog();
}



/*
 * filter_close_dialog:
 *
 * Close the filter dialog. If commit is TRUE the changes
 * are committed, otherwise dropped.
 */
void filter_close_dialog(gboolean commit)
{
    if (commit) {
        filter_apply_changes();
    } else
        filter_revert_changes();

    if (filter_dialog != NULL) {
        gint32 coord[4] = {0, 0, 0, 0};

        gdk_window_get_root_origin(filter_dialog->window, &coord[0], &coord[1]);
        gdk_drawable_get_size(filter_dialog->window, &coord[2], &coord[3]);

        gui_prop_set_guint32(PROP_FILTER_DLG_COORDS, (guint32 *) coord, 0, 4);
        
        filter_main_divider_pos =
            gtk_paned_get_position
                (GTK_PANED(lookup_widget(filter_dialog, "hpaned_filter_main")));

#ifdef FILTER_HIDE_ON_CLOSE        
        gtk_widget_hide(filter_dialog);
#else
        gtk_object_destroy(GTK_OBJECT(filter_dialog));
        filter_dialog = NULL;
#endif /* FILTER_HIDE_ON_CLOSE */
    }
}



/*
 * filter_duplicate_rule:
 *
 * returns a new rule created with information based on the given rule
 * with the appropriate filter_new_*_rule call. Defaults set by those
 * calls (like RULE_FLAG_VALID) will also apply to the the returned rule.
 */
rule_t *filter_duplicate_rule(rule_t *r)
{
    g_assert(r != NULL);

    switch(r->type) {
    case RULE_TEXT:
        return filter_new_text_rule
            (r->u.text.match, r->u.text.type, r->u.text.case_sensitive,
            r->target, r->flags);
    case RULE_IP:
        return filter_new_ip_rule
            (r->u.ip.addr, r->u.ip.mask, r->target, r->flags);
    case RULE_SIZE:
        return filter_new_size_rule
            (r->u.size.lower, r->u.size.upper, r->target, r->flags);
    case RULE_JUMP:
        return filter_new_jump_rule(r->target, r->flags);
    case RULE_SHA1:
        return filter_new_sha1_rule
            (r->u.sha1.hash, r->u.sha1.filename, r->target, r->flags);
    case RULE_FLAG:
        return filter_new_flag_rule
            (r->u.flag.stable, r->u.flag.busy, r->u.flag.push, 
            r->target, r->flags);
    case RULE_STATE:
        return filter_new_state_rule
            (r->u.state.display, r->u.state.download, r->target, r->flags);
    default:
        g_error("filter_duplicate_rule: unknown rule type: %d", r->type);
        return NULL;
    }
}



rule_t *filter_new_text_rule(gchar *match, gint type, 
    gboolean case_sensitive, filter_t *target, guint16 flags)
{
  	rule_t *r;
    gchar *buf;

    g_assert(match != NULL);
    g_assert(target != NULL);

  	r = g_new0(rule_t, 1);

   	r->type                  = RULE_TEXT;
    r->flags                 = flags;
    r->target                = target;
    r->u.text.case_sensitive = case_sensitive;
    r->u.text.type           = type;
    r->u.text.match          = g_strdup(match);
    r->u.text.matchlen       = strlen(match);
    set_flags(r->flags, RULE_FLAG_VALID);

    if (!r->u.text.case_sensitive)
        strlower(r->u.text.match, r->u.text.match);

    buf = g_strdup(r->u.text.match);

  	if (r->u.text.type == RULE_TEXT_WORDS) {
		gchar *s;
		GList *l = NULL;

		for (s = strtok(buf, " \t\n"); s; s = strtok(NULL, " \t\n"))
			l = g_list_append(l, pattern_compile(s));

		r->u.text.u.words = l;
	} else 
    if (r->u.text.type == RULE_TEXT_REGEXP) {
		int err;
		regex_t *re;

		re = g_new0(regex_t, 1);
		err = regcomp(re, buf,
			REG_EXTENDED|REG_NOSUB|(r->u.text.case_sensitive ? 0 : REG_ICASE));

		if (err) {
			gchar regbuf[1000];
			regerror(err, re, regbuf, sizeof(regbuf));

			g_warning(
                "problem in regular expression: %s"
				"; falling back to substring match", buf);

			r->u.text.type = RULE_TEXT_SUBSTR;
            G_FREE_NULL(re);
		} else {
			r->u.text.u.re = re;
		}
	}

	/* no "else" because REGEXP can fall back here */
	if (r->u.text.type == RULE_TEXT_SUBSTR)
		r->u.text.u.pattern = pattern_compile(buf);

    G_FREE_NULL(buf);

    return r;
}



rule_t *filter_new_ip_rule
    (guint32 addr, guint32 mask, filter_t *target, guint16 flags)
{
	rule_t *r;

    g_assert(target != NULL);

	r = g_new0(rule_t, 1);

   	r->type = RULE_IP;

	r->u.ip.addr  = addr;
	r->u.ip.mask  = mask;
	r->u.ip.addr &= r->u.ip.mask;
    r->target     = target;
    r->flags      = flags;
    set_flags(r->flags, RULE_FLAG_VALID);

    return r;
}



rule_t *filter_new_size_rule
    (size_t lower, size_t upper, filter_t *target, guint16 flags)
{
   	rule_t *f;

    g_assert(target != NULL);

    f = g_new0(rule_t, 1);

    f->type = RULE_SIZE;

    if (lower > upper) {
        f->u.size.lower = upper;
        f->u.size.upper = lower;
    } else {
        f->u.size.lower = lower;
        f->u.size.upper = upper;
    }

  	f->target = target;
    f->flags  = flags;
    set_flags(f->flags, RULE_FLAG_VALID);

    return f;
}




rule_t *filter_new_jump_rule(filter_t *target, guint16 flags)
{
   	rule_t *f;

    g_assert(target != NULL);

    f = g_new0(rule_t, 1);

    f->type = RULE_JUMP;

  	f->target = target;
    f->flags  = flags;
    set_flags(f->flags, RULE_FLAG_VALID);

    return f;
}




rule_t *filter_new_sha1_rule
    (gchar *sha1, gchar *filename, filter_t *target, guint16 flags)
{
   	rule_t *f;

    g_assert(target != NULL);
    g_assert(filename != NULL);

    f = g_new0(rule_t, 1);

    f->type = RULE_SHA1;

  	f->target = target;
    if (sha1 != NULL)
        f->u.sha1.hash = g_memdup(sha1, SHA1_RAW_SIZE);
    f->u.sha1.filename = g_strdup(filename);
    f->flags  = flags;
    set_flags(f->flags, RULE_FLAG_VALID);

    return f;
}



rule_t *filter_new_flag_rule
    (enum rule_flag_action stable, enum rule_flag_action busy,
     enum rule_flag_action push, filter_t *target, guint16 flags)
{
   	rule_t *f;

    g_assert(target != NULL);

    f = g_new0(rule_t, 1);

    f->type = RULE_FLAG;

    f->u.flag.stable = stable;
    f->u.flag.busy = busy;
    f->u.flag.push = push;
  	f->target = target;
    f->flags  = flags;
    set_flags(f->flags, RULE_FLAG_VALID);

    return f;
}



rule_t *filter_new_state_rule
    (enum filter_prop_state display, enum filter_prop_state download,
    filter_t *target, guint16 flags)
{
       	rule_t *f;

    g_assert(target != NULL);

    f = g_new0(rule_t, 1);

    f->type = RULE_STATE;

    f->u.state.display = display;
    f->u.state.download = download;
  	f->target = target;
    f->flags  = flags;
    set_flags(f->flags, RULE_FLAG_VALID);

    return f;
}



/*
 * filter_set:
 *
 * Start working on the given filter. Set this filter as 
 * work_filter so we can commit the changed rules to this
 * filter.
 */
void filter_set(filter_t *f)
{
    if (f != NULL) {
        shadow_t *shadow;
        gboolean removable;
        gboolean active;
        GList *ruleset;
    
        shadow = shadow_find(f);
        if (shadow != NULL) {
            removable = 
                (shadow->refcount == 0) && !filter_is_builtin(f) && 
                !filter_is_global(f) && !filter_is_bound(f);
            active = filter_is_active(shadow);
            ruleset = shadow->current;
        } else {
            removable = 
                (f->refcount == 0) && !filter_is_builtin(f) && 
                !filter_is_global(f) && !filter_is_bound(f);
            active = filter_is_active(f);
            ruleset = f->ruleset;
        }
    
        filter_gui_filter_set(f, removable, active, ruleset);
    } else {
        filter_gui_filter_set(NULL, FALSE, FALSE, NULL);
    }

    /* 
     * don't want the work_filter to be selectable as a target
     * so we changed it... we have to rebuild.
     */
    filter_update_targets();
}



/*
 * filter_close_search:
 *
 * Clear the searches shadow, update the combobox and the filter
 * bound to this search (search->ruleser).
 */
void filter_close_search(search_t *s)
{
    shadow_t *shadow;

    g_assert(s != NULL);
    g_assert(s->filter != NULL);

    if (gui_debug >= 6)
        printf("closing search (freeing filter): %s\n", s->query);

    shadow = shadow_find(s->filter);
    if (shadow != NULL) {
		GList *copy;

		copy = g_list_copy(shadow->removed);
		G_LIST_FOREACH_SWAPPED(copy, filter_append_rule_to_session, s->filter);
        g_list_free(copy);

		copy = g_list_copy(shadow->added);
		G_LIST_FOREACH_SWAPPED(copy,
			filter_remove_rule_from_session, s->filter);
        g_list_free(copy);

        shadow_cancel(shadow);
    }          

    /*
     * If this is the filter currently worked on, clear the display.
     */
    if (s->filter == work_filter)
        filter_set(NULL);

    filter_gui_filter_remove(s->filter);

    filter_free(s->filter);
    s->filter = NULL;
}



/*
 * filter_apply_changes:
 *
 * Go through all the shadow filters, and commit the recorded
 * changes to the assosicated filter. We walk through the 
 * shadow->current list. Every item in shadow->removed will be
 * removed from the searchs filter and the memory will be freed.
 * Then shadow->current will be set as the new filter for that
 * search.
 */
void filter_apply_changes(void) 
{
    GList *s;

    /*
     * Free memory for all removed filters;
     */
    for (s = shadow_filters; s != NULL; s = shadow_filters)
        shadow_commit((shadow_t*)s->data);

    g_list_free(filters);
    filters = g_list_copy(filters_current);

    /*
     * Remove the SHADOW flag from all added filters
     */
    for (s = filters_added; s != NULL; s = s->next)
        clear_flags(((filter_t *)s->data)->flags, FILTER_FLAG_SHADOW);

    g_list_free(filters_added);
    filters_added = NULL;
    
    /*
     * Free all removed filters. Don't iterate since filter_free removes
     * the filter from filters_removed.
     */
    for (s = filters_removed; s != NULL; s = filters_removed) {
        filter_free(s->data);
    }
    g_assert(filters_removed == NULL);

    filter_update_targets();
    filter_set(work_filter);
}



/*
 * filter_revert_changes:
 *
 * Free the ressources for all added filters and forget all shadows.
 * A running session will not be ended by this.
 */
void filter_revert_changes(void)
{
    GList *s;
    gint n;

    if (gui_debug >= 5)
        printf("Canceling all changes to filters/rules\n");

    filter_gui_freeze_filters();
    filter_gui_freeze_rules();

    /*
     * Free memory for all added filters and for the shadows.
     */
    for (s = shadow_filters; s != NULL; s = shadow_filters)
        shadow_cancel((shadow_t *)s->data);

    if (g_list_find(filters, work_filter) != NULL)
        filter_set(work_filter);
    else
        filter_set(NULL);

    g_list_free(filters_current);
    filters_current = g_list_copy(filters);
    
    /*
     * Free and remove all added filters. We don't iterate explicitly,
     * because filter_free removes the added filter from filters_added
     * for us.
     */
    n = 0;
    for (s = filters_added; s != NULL; s = filters_added) {
        filter_t *filter = (filter_t *) s->data;
    
        filter_gui_filter_remove(filter);
        filter_free(filter);
    }
    g_assert(filters_added == NULL);

    /*
     * Restore all removed filters.
     */
    for (s = filters_removed; s != NULL; s = s->next) {
        filter_t *filter = (filter_t *) s->data;

        filter_gui_filter_add(filter, filter->ruleset);
    }
    g_list_free(filters_removed);
    filters_removed = NULL;

    /*
     * Update the rulecounts. Since we don't have any shadows anymore, we
     * can just use f->ruleset. Also update the 'enabled' state of the
     * filters while we are at it.
     */
    for (s = filters_current; s != NULL; s = g_list_next(s)) {
        filter_t *filter = (filter_t *) s->data;

        filter_gui_update_rule_count(filter, filter->ruleset);
        filter_gui_filter_set_enabled(filter, filter_is_active(filter));
    }

    filter_gui_thaw_rules();
    filter_gui_thaw_filters();
    
    filter_update_targets();
}



/*
 * filter_rule_condition_to_gchar:
 *
 * Convert a rule condition to a human readable string.
 */
gchar *filter_rule_condition_to_gchar(const rule_t *r)
{
    static gchar tmp[256];

    g_assert(r != NULL);
    
    switch (r->type) {
    case RULE_TEXT:
        switch (r->u.text.type) {
        case RULE_TEXT_PREFIX:
           	gm_snprintf(
                tmp, sizeof(tmp), 
                "If filename begins with \"%s\" %s",
                r->u.text.match,
                r->u.text.case_sensitive ? "(case sensitive)" : "");
            break;
        case RULE_TEXT_WORDS:
           	gm_snprintf(
                tmp, sizeof(tmp), 
                "If filename contains the words \"%s\" %s",
                r->u.text.match,
                r->u.text.case_sensitive ? "(case sensitive)" : "");
            break;
        case RULE_TEXT_SUFFIX:
          	gm_snprintf(
                tmp, sizeof(tmp), 
                "If filename ends with \"%s\" %s",
                r->u.text.match,
                r->u.text.case_sensitive ? "(case sensitive)" : "");
            break;
        case RULE_TEXT_SUBSTR:
           	gm_snprintf(
                tmp, sizeof(tmp), 
                "If filename contains the substring \"%s\" %s",
                r->u.text.match,
                r->u.text.case_sensitive ? "(case sensitive)" : "");
            break;
        case RULE_TEXT_REGEXP:
           	gm_snprintf(
                tmp, sizeof(tmp), 
                "If filename matches the regex \"%s\" %s",
                r->u.text.match,
                r->u.text.case_sensitive ? "(case sensitive)" : "");
            break;
        case RULE_TEXT_EXACT:
           	gm_snprintf(
                tmp, sizeof(tmp), 
                "If filename is \"%s\" %s",
                r->u.text.match,
                r->u.text.case_sensitive ? "(case sensitive)" : "");
            break;
        default:
            g_error("filter_rule_condition_to_gchar:" 
                    "unknown text rule type: %d", r->u.text.type);
        };
        break;
    case RULE_IP:
        {
            gchar *mask;
            gchar *addr;

            mask = g_strdup(ip_to_gchar(r->u.ip.mask));
            addr = g_strdup(ip_to_gchar(r->u.ip.addr));

            gm_snprintf(tmp, sizeof(tmp), 
                "If IP address matches %s/%s", addr, mask);

            G_FREE_NULL(addr);
            G_FREE_NULL(mask);
        }
        break;
    case RULE_SIZE:
		if (r->u.size.lower == 0)
			gm_snprintf(tmp, sizeof(tmp),
				"If filesize is smaller than %d (%s)",
				(gint) r->u.size.upper, short_size(r->u.size.upper));
		else if (r->u.size.upper == r->u.size.lower)
			gm_snprintf(tmp, sizeof(tmp),
				"If filesize is exactly %d (%s)",
				(gint) r->u.size.upper, short_size(r->u.size.upper));
        else {
            gchar *s1;
            gchar *s2;

            s1 = g_strdup(short_size(r->u.size.lower));
            s2 = g_strdup(short_size(r->u.size.upper));
    
			gm_snprintf(tmp, sizeof(tmp),
				"If filesize is between %d and %d (%s - %s)",
				(gint) r->u.size.lower, (int)r->u.size.upper, s1, s2);

            G_FREE_NULL(s1);
            G_FREE_NULL(s2);
        }
        break;
    case RULE_SHA1:
        if (r->u.sha1.hash != NULL) {
            gm_snprintf(tmp, sizeof(tmp), "If urn:sha1 is same as for \"%s\"",
                r->u.sha1.filename);
        } else 
            gm_snprintf(tmp, sizeof(tmp), "If urn:sha1 is not available");
        break;
    case RULE_JUMP:
       	gm_snprintf(
            tmp, sizeof(tmp), 
            "Always");
        break;
    case RULE_FLAG:
        {
            gchar *busy_str = "";
            gchar *push_str = "";
            gchar *stable_str = "";
            gchar *s1 = "";
            gchar *s2 = "";
            gboolean b = FALSE;

            switch (r->u.flag.busy) {
            case RULE_FLAG_SET:
                busy_str = "busy is set";
                b = TRUE;
                break;
            case RULE_FLAG_UNSET:
                busy_str = "busy is not set";
                b = TRUE;
                break;
            case RULE_FLAG_IGNORE:
                break;
            }
    
            switch (r->u.flag.push) {
            case RULE_FLAG_SET:
                if (b) s1 = ", ";
                push_str = "push is set";
                b = TRUE;
                break;
            case RULE_FLAG_UNSET:
                if (b) s1 = ", ";
                push_str = "push is not set";
                b = TRUE;
                break;
            case RULE_FLAG_IGNORE:
                break;
            }
    
            switch (r->u.flag.stable) {
            case RULE_FLAG_SET:
                if (b) s2 = ", ";
                stable_str = "stable is set";
                b = TRUE;
                break;
            case RULE_FLAG_UNSET:
                if (b) s2 = ", ";
                stable_str = "stable is not set";
                b = TRUE;
                break;
            case RULE_FLAG_IGNORE:
                break;
            }

            if (b)
                gm_snprintf(
                    tmp, sizeof(tmp),
                    "If flag %s%s%s%s%s", 
                    busy_str, s1, push_str, s2, stable_str);
            else
                 gm_snprintf(
                    tmp, sizeof(tmp),
                    "Always (all flags ignored)");
        }
        break;
    case RULE_STATE:
        {
            gchar *display_str = "";
            gchar *download_str = "";
            gchar *s1 = "";
            gboolean b = FALSE;

            switch (r->u.state.display) {
            case FILTER_PROP_STATE_UNKNOWN:
                display_str = "DISPLAY is undefined";
                b = TRUE;
                break;
            case FILTER_PROP_STATE_DO:
                display_str = "DISPLAY";
                b = TRUE;
                break;
            case FILTER_PROP_STATE_DONT:
                display_str = "DON'T DISPLAY";
                b = TRUE;
                break;
            case FILTER_PROP_STATE_IGNORE:
                break;
            default:
                g_assert_not_reached();
            }
    
            switch (r->u.state.download) {
            case FILTER_PROP_STATE_UNKNOWN:
                if (b) s1 = ", ";
                download_str = "DOWNLOAD is undefined";
                b = TRUE;
                break;
            case FILTER_PROP_STATE_DO:
                if (b) s1 = ", ";
                download_str = "DOWNLOAD";
                b = TRUE;
                break;
            case FILTER_PROP_STATE_DONT:
                if (b) s1 = ", ";
                download_str = "DON'T DOWNLOAD";
                b = TRUE;
                break;
            case FILTER_PROP_STATE_IGNORE:
                break;
            default:
                g_assert_not_reached();
            }
     
            if (b)
                gm_snprintf(
                    tmp, sizeof(tmp),
                    "If flag %s%s%s", 
                    display_str, s1, download_str);
            else
                 gm_snprintf(
                    tmp, sizeof(tmp),
                    "Always (all states ignored)");
        }
        break;
    default:
        g_error("filter_rule_condition_to_gchar: "
                "unknown rule type: %d", r->type);
        return NULL;
    };

    return tmp;
}



/*
 * filter_rule_to_gchar:
 *
 * Convert the filter to a human readable string.
 */
gchar *filter_rule_to_gchar(rule_t *r) 
{
    gchar *cond;

    g_assert(r != NULL);

    cond = g_strdup(filter_rule_condition_to_gchar(r));

	gm_snprintf(f_tmp, sizeof(f_tmp), "%s%s %s jump to \"%s\"", 
        RULE_IS_NEGATED(r) ? "(Negated) " : "",
        RULE_IS_ACTIVE(r) ? "" : "(deactivated)",
        cond,
        RULE_IS_VALID(r) ? r->target->name : "(invalid)");

    G_FREE_NULL(cond);
  
    return f_tmp;
}



/*
 * filter_new:
 *
 * Create a new filter with the given name.
 */
filter_t *filter_new(gchar *name)
{
    filter_t *f;

    g_assert(name != NULL);

    f = g_new0(filter_t, 1);
    f->name = g_strdup(name);
    f->ruleset = NULL;
    f->search = NULL;
    f->visited = FALSE;
    set_flags(f->flags, FILTER_FLAG_ACTIVE);

    return f;
}



/*
 * filter_add_to_session:
 *
 * Add a filter to the current editing session. Never try to add
 * a filter twice. Returns a error code on failure and 0 on success.
 */
void filter_add_to_session(filter_t *f)
{
    g_assert(g_list_find(filters_current, f) == NULL);
    g_assert(f != NULL);
   

    /*
     * Either remove from removed or add to added list.
     */
    if (g_list_find(filters_removed, f) != NULL)
        filters_removed = g_list_remove(filters_removed, f);
    else {
        filters_added = g_list_append(filters_added, f);

        /*
         * Since the filter is new and not yet used for filtering
         * we set the FILTER_FLAG_SHADOW flag.
         */
        set_flags(f->flags, FILTER_FLAG_SHADOW);
    }

    filters_current = g_list_append(filters_current, f);

    filter_gui_filter_add(f, f->ruleset);
}



/*
 * filter_new_for_search:
 *
 * Create a new filter bound to a search and register it.
 */
void filter_new_for_search(search_t *s)
{
    filter_t *f;

    g_assert(s != NULL);
    g_assert(s->query != NULL);

    f = g_new0(filter_t, 1);
    f->name = g_strdup(s->query);
    f->ruleset = NULL;
    f->search = NULL;
    f->visited = FALSE;
    set_flags(f->flags, FILTER_FLAG_ACTIVE);

    /*
     * Add filter to current and session lists
     */
    filters = g_list_append(filters, f);
    filters_current = g_list_append(filters_current, f);

    /*
     * Crosslink filter and search
     */
    f->search = s;
    s->filter = f;

    /*
     * It's important to add the filter here, because it was not
     * bound before and would have been sorted in as a free filter.
     */
    filter_gui_filter_add(f, f->ruleset);
}



/*
 * filter_remove_from_session:
 *
 * Mark the given filter as removed and delete it when the
 * dialog changes are committed.
 */
void filter_remove_from_session(filter_t *f) 
{
    g_assert(g_list_find(filters_removed, f) == NULL);
    g_assert(g_list_find(filters_current, f) != NULL);

    /*
     * Either remove from added list or add to removed list.
     */
    if (g_list_find(filters_added, f) != NULL)
        filters_added = g_list_remove(filters_added, f);
    else
        filters_removed = g_list_append(filters_removed, f);

    filters_current = g_list_remove(filters_current, f);

    /*
     * If this is the filter currently worked on, clear the display.
     */
    if (work_filter == f) 
        filter_set(NULL);

    filter_gui_filter_remove(f);
}



/*
 * filter_free:
 *
 * Frees a filter and the filters assiciated with it and
 * unregisters it from current and session filter lists.
 */
static void filter_free(filter_t *f) 
{
    GList *copy;

    g_assert(f != NULL);

    if (shadow_find(f) != NULL)
        g_error("Unable to free shadowed filter \"%s\" with refcount %d",
            f->name, f->refcount);

    if (f->refcount != 0)
        g_error("Unable to free referenced filter \"%s\" with refcount %d",
            f->name, f->refcount);
    
    /*
     * Remove the filter from current and session data
     */
    if (g_list_find(filters, f) != NULL)
        filters = g_list_remove(filters, f);
    if (g_list_find(filters_current, f) != NULL)
        filters_current = g_list_remove(filters_current, f);
    if (g_list_find(filters_added, f) != NULL)
        filters_added = g_list_remove(filters_added, f);
    if (g_list_find(filters_removed, f) != NULL)
        filters_removed = g_list_remove(filters_removed, f);

	copy = g_list_copy(f->ruleset);
	G_LIST_FOREACH_SWAPPED(copy, filter_remove_rule, f);
    g_list_free(copy);

    G_FREE_NULL(f->name);
    G_FREE_NULL(f);
}



/*
 * filter_free_rule:
 *
 * Free memory reserved by rule respecting the type of the rule.
 */
void filter_free_rule(rule_t *r)
{
    g_assert(r != NULL);

    if (gui_debug >= 6)
        printf("freeing rule: %s\n", filter_rule_to_gchar(r));

    switch (r->type) {
    case RULE_TEXT:
        G_FREE_NULL(r->u.text.match);

        switch (r->u.text.type) {
        case RULE_TEXT_WORDS:
            g_list_foreach(r->u.text.u.words, (GFunc)pattern_free, NULL);
            g_list_free(r->u.text.u.words);
            r->u.text.u.words = NULL;
            break;
        case RULE_TEXT_SUBSTR:
            pattern_free(r->u.text.u.pattern);
            r->u.text.u.pattern = NULL;
            break;
        case RULE_TEXT_REGEXP:
            regfree(r->u.text.u.re);
            r->u.text.u.re = NULL;
            break;
        case RULE_TEXT_PREFIX:
        case RULE_TEXT_SUFFIX:
        case RULE_TEXT_EXACT:
            break;
        default:
            g_error("filter_free_rule: unknown text rule type: %d", r->u.text.type);
        }
        break;
    case RULE_SHA1:
        G_FREE_NULL(r->u.sha1.hash);
        G_FREE_NULL(r->u.sha1.filename);
        break;
    case RULE_SIZE:
    case RULE_JUMP:
    case RULE_IP:
    case RULE_FLAG:
    case RULE_STATE:
        break;
    default:
        g_error("filter_free_rule: unknown rule type: %d", r->type);
    }
	G_FREE_NULL(r);
}



/*
 * filter_append_rule:
 *
 * Append a new rule to a filter. If necessary also update the shadow.
 * The addition of the rule cannot be cancelled by canceling the
 * shadow. If no shadow for the filters exists, none is created.
 */
void filter_append_rule(filter_t *f, rule_t * const r)
{
    shadow_t *shadow;
    shadow_t *target_shadow;

    g_assert(f != NULL);
    g_assert(r != NULL);
    g_assert(r->target != NULL);

    shadow = shadow_find(f);
    target_shadow = shadow_find(r->target);

    if (g_list_find(f->ruleset, r) != NULL)
        g_error("rule already exists in filter \"%s\"", f->name);

    if ((shadow != NULL) && g_list_find(shadow->current, r))
        g_error("rule already exists in shadow for filter \"%s\"",
            f->name);

    /*
     * We add the rule to the filter increase the refcount on the target.
     */
    f->ruleset = g_list_append(f->ruleset, r);
    r->target->refcount ++;
    if (gui_debug >= 6)
        printf("increased refcount on \"%s\" to %d\n",
            r->target->name, r->target->refcount);

    /*
     * If a shadow for our filter exists, we add it there also.
     */
    if (shadow != NULL)
        shadow->current = g_list_append(shadow->current, r);

    /*
     * If a shadow for the target exists, we increase refcount there too.
     */
    if (target_shadow != NULL) {
        target_shadow->refcount ++;

        if (gui_debug >= 6)
            printf("increased refcount on shadow of \"%s\" to %d\n",
                target_shadow->filter->name, target_shadow->refcount);
    }
    
    /*
     * Update dialog if necessary.
     */
    {
        GList *ruleset;

        ruleset = (shadow != NULL) ? shadow->current : f->ruleset;

        if (work_filter == f)
            filter_gui_set_ruleset(ruleset);
        filter_gui_update_rule_count(f, ruleset);
    }
}



/*
 * filter_append_rule_to_session:
 *
 * Append a new rule to the filter shadow. This call will fail
 * with an assertion error if the rule is already existing in
 * the shadow.
 */
void filter_append_rule_to_session(filter_t *f, rule_t * const r)
{
    shadow_t *shadow = NULL;
    shadow_t *target_shadow = NULL;

    g_assert(r != NULL);
    g_assert(f != NULL);
    g_assert(r->target != NULL);

    if (gui_debug >= 4)
        printf("appending rule to filter: %s <- %s (%p)\n",
            f->name, filter_rule_to_gchar(r), r->target);

    /*
     * The rule is added to a session, so we set the shadow flag.
     */
    set_flags(r->flags, RULE_FLAG_SHADOW);

    /*
     * Create a new shadow if necessary.
     */
    shadow = shadow_find(f);
    if (shadow == NULL)
        shadow = shadow_new(f);
    else {
        /*
         * You can never add a filter to a shadow or filter
         * twice!
         */
        g_assert(g_list_find(shadow->current, r) == NULL);
    }

    if (g_list_find(shadow->removed, r) == NULL) {
        shadow->added = g_list_append(shadow->added, r);
    } else {
        shadow->removed = g_list_remove(shadow->removed, r);
    }
    shadow->current = g_list_append(shadow->current, r);

    /*
     * We need to increase the refcount on the target.
     */
    target_shadow = shadow_find(r->target);
    if (target_shadow == NULL)
        target_shadow = shadow_new(r->target);

    target_shadow->refcount ++;
    if (gui_debug >= 6)
        printf("increased refcount on shadow of \"%s\" to %d\n",
            target_shadow->filter->name, target_shadow->refcount);

    /*
     * Update dialog if necessary.
     */
    if (work_filter == f)
        filter_gui_set_ruleset(shadow->current);
    filter_gui_update_rule_count(f, shadow->current);
}



/*
 * filter_remove_rule:
 *
 * Removes a rule directly. The removal cannot be reversed by
 * cancelling the shadow. The filter is removed from the active
 * filter and from a potentially existing shadow as well.
 * If no shadow exists, no shadow is created.
 */
void filter_remove_rule(filter_t *f, rule_t *r)
{
    shadow_t *shadow;
    shadow_t *target_shadow;
    gboolean in_shadow_current;
    gboolean in_shadow_removed;
    gboolean in_filter;

    g_assert(f != NULL);
    g_assert(r != NULL);
    g_assert(r->target != NULL);

    shadow = shadow_find(f);
    target_shadow = shadow_find(r->target);

    in_filter = g_list_find(f->ruleset, r) != NULL;

    /*
     * We need to check where the rule is actually located... in the
     * shadow, in the real filter or in both.
     */
    if (shadow != NULL) {
        in_shadow_current = g_list_find(shadow->current, r) != NULL;
        in_shadow_removed = g_list_find(shadow->removed, r) != NULL;
    } else {
        /*
         * If there is no shadow, we pretend that the shadow is 
         * equal to the filter, so we set in_shadow_current to in_filter.
         */
        in_shadow_current = in_filter; 
        in_shadow_removed = FALSE;
    }

    /*
     * We have to purge the rule from the shadow where necessary.
     */
    if (in_shadow_current && (shadow != NULL)) {
        shadow->current = g_list_remove(shadow->current, r);
        shadow->added = g_list_remove(shadow->added, r);
    }

    if (in_shadow_removed && (shadow != NULL))
       shadow->removed = g_list_remove(shadow->removed, r);

    if (in_filter)
        f->ruleset = g_list_remove(f->ruleset, r);

    /*
     * Now we need to clean up the refcounts that may have been
     * caused by this rule. We have these possibilities:
     *
     *   in    in shadow   in shadow  in shadow   |   refcounted in
     * filter   current      added     removed    |  filter | shadow
     * ------------------------------------------------------------
     *   yes     yes          yes        yes      |   - failure A -
     *   yes     yes          yes        no       |   - failure C -
     *   yes     yes          no         yes      |   - failure D -
     * 1 yes     yes          no         no       |   yes       yes
     *   yes     no           yes        yes      |   - failure A -
     *   yes     no           yes        no       |   - failure B -
     * 2 yes     no           no         yes      |   yes       no
     *   yes     no           no         no       |   - failure E -
     *   no      yes          yes        yes      |   - failure A -
     * 3 no      yes          yes        no       |   no        yes
     *   no      yes          no         yes      |   - failure D -
     *   no      yes          no         no       |   - failure F -
     *   no      no           yes        yes      |   - failure A -
     *   no      no           yes        no       |   - failure B -
     *   no      no           no         yes      |   - failure G -
     * 4 no      no           no         no       |   no        no
     *   
     * Possibilities:
     * 1) the rule has been there when the shadow was created and
     *    has not been removed since then. (Or has been removed and
          added back)
     * 2) the rule has been there when the shadow was created, but
     *    was removed from the shadow. The target shadow already
     *    knows that so we only need to remove from the target filter
     *    to bring the target shadow and the target filter in sync.
     * 3) the rule was added during the session. When it was added
     *    a shadow for the target has also been created to increase
     *    the refcount on that. We don't know wether the shadow contains
     *    other changes, but we must reduce the refcount on that shadow.
     * 4) the rule is neither in the shadow nor in the filter, we
     *    issue a warning and do nothing.
     *
     * Failures:
     * A) a rule can never be in shadow->added and shadow->removed at 
     *    the same time.
     * B) a rule cannot be in added but not in current
     * C) a rule can't be added if it was already in the original filter
     * D) a rule can't be in current and also in removed
     * E) if a rule is in the original filter but not in current it 
     *    must have been removed
     * F) if the rule is in current but not in the original filter, it
     *    must have been added.
     * G) if a rule is in removed, it must have been in the original
     *    filter.
     */
    if (in_filter) {
        r->target->refcount --;

        if (gui_debug >= 6)
            printf("decreased refcount on \"%s\" to %d\n",
                r->target->name, r->target->refcount);
    }

    if (in_shadow_current) {
        if (target_shadow != NULL) {
            target_shadow->refcount --;
        
            if (gui_debug >= 6)
                printf("decreased refcount on shadow of \"%s\" to %d\n",
                    target_shadow->filter->name, target_shadow->refcount);
        }
    }

    if (!in_filter && !in_shadow_current) {
        g_warning("rule unknown in context: aborting removal without freeing");
        return;
    }

    filter_free_rule(r);
    
    /*
     * Update dialog if necessary.
     */
     {
        GList *ruleset;

        ruleset = (shadow != NULL) ? shadow->current : f->ruleset;

        if (work_filter == f)
            filter_gui_set_ruleset(ruleset);
        filter_gui_update_rule_count(f, ruleset);
    }
}


/*
 * filter_remove_rule_from_session:
 *
 * Remove rule from a filter shadow. This call will fail
 * with an assertion error if the rule has already been 
 * removed from the shadow or if it never was in the shadow.
 * The memory associated with the rule will be freed.
 */
void filter_remove_rule_from_session(filter_t *f, rule_t * const r)
{
    shadow_t *shadow;
    shadow_t *target_shadow;
    GList *l = NULL;
       
    g_assert(r != NULL);
    g_assert(f != NULL);

    if (gui_debug >= 4)
        printf("removing rule in filter: %s -> %s\n", 
            f->name, filter_rule_to_gchar(r));

    /*
     * Create a new shadow if necessary.
     */
    shadow = shadow_find(f);
    if (shadow == NULL)
        shadow = shadow_new(f);

    g_assert(g_list_find(shadow->current, r) != NULL);

    shadow->current = g_list_remove(shadow->current, r);

    /*
     * We need to decrease the refcount on the target. We need to do this
     * now because soon the rule may be freed and we may not access it
     * after that.
     */
    target_shadow = shadow_find(r->target);
    if (target_shadow == NULL)
        target_shadow = shadow_new(r->target);

    target_shadow->refcount --;
    if (gui_debug >= 6)
        printf("decreased refcount on shadow of \"%s\" to %d\n",
            target_shadow->filter->name, target_shadow->refcount);

    l = g_list_find(shadow->added, r);
    if (l != NULL) {
        /*
         * The rule was added only to the shadow and was
         * not committed. We removed it from the added list
         * and free the ressources.
         */
        if (gui_debug >= 4)
            printf("while removing from %s: removing from added: %s\n",
                f->name, filter_rule_to_gchar(r));
        shadow->added = g_list_remove(shadow->added, r);
        filter_free_rule(r);
    } else {
        /*
         * The rule was not added, so it must be existent.
         * If it is, we remember it on shadow->removed.
         */
        g_assert(g_list_find(shadow->removed, r) == NULL);

        if (gui_debug >= 4)
            printf("while removing from %s: adding to removed: %s\n",
                f->name, filter_rule_to_gchar(r));
      
        shadow->removed = g_list_append(shadow->removed, r);
    }

    /*
     * Update dialog if necessary.
     */
    if (work_filter == f)
        filter_gui_set_ruleset(shadow->current);
    filter_gui_update_rule_count(f, shadow->current);
}



/*
 * filter_replace_rule_in_session:
 *
 * Replaces filter rule A with filter rule B in filter . A
 * must already be in the shadow and B must not! 
 *
 * CAUTION: ACTUALLY B MUST NOT BE IN ANY OTHER SEARCH !!!
 *
 * The memory for A is freed in the process.
 */
void filter_replace_rule_in_session(filter_t *f, 
    rule_t * const old_rule, rule_t * const new_rule)
{
    GList *filter;
    GList *added;
    shadow_t *shadow;
    shadow_t *target_shadow;

    g_assert(old_rule != new_rule);
    g_assert(old_rule != NULL);
    g_assert(new_rule != NULL);

    /*
     * Create a new shadow if necessary.
     */
    shadow = shadow_find(f);
    if (shadow == NULL)
        shadow = shadow_new(f);

    /*
     * Find the list node where we have to replace the
     * rule.
     */
    filter = g_list_find(shadow->current, old_rule);
    g_assert(filter != NULL);

    if (gui_debug >= 4) {
        gchar * f1 = g_strdup(filter_rule_to_gchar(old_rule));
        gchar * f2 = g_strdup(filter_rule_to_gchar(new_rule));

        printf("replacing rules (old <- new): %s <- %s\n", f1, f2);

        G_FREE_NULL(f1);
        G_FREE_NULL(f2);
    }

    /*
     * In any case we have to reduce the refcount on the old rule's
     * target.
     */
    target_shadow = shadow_find(old_rule->target);
    if (target_shadow == NULL)
        target_shadow = shadow_new(old_rule->target);

    target_shadow->refcount --;
    if (gui_debug >= 6)
        printf("decreased refcount on shadow of \"%s\" to %d\n",
            target_shadow->filter->name, target_shadow->refcount);

    /*
     * Find wether the node to be replaced is in shadow->added. 
     * If so, we may free the memory of the old rule.
     */
    added = g_list_find(shadow->added, old_rule);

    if (added != NULL) {
        /*
         * If it was added, then free and remove the rule.
         */
        shadow->added = g_list_remove(shadow->added, old_rule);
        filter_free_rule(old_rule);
    } else {
        /*
         * If the filter was not added, then it must be marked
         * for begin removed.
         */
        shadow->removed = g_list_append(shadow->removed, old_rule);
    }
     
    /*
     * The new rule can't be in the original filter, so we mark it
     * as added.
     */
    shadow->added = g_list_append(shadow->added, new_rule);
    set_flags(new_rule->flags, RULE_FLAG_SHADOW);

    /*
     * And we also need to increase the refcount on the new rule's
     * target
     */
    target_shadow = shadow_find(new_rule->target);
    if (target_shadow == NULL)
        target_shadow = shadow_new(new_rule->target);

    target_shadow->refcount ++;
    if (gui_debug >= 6)
        printf("increased refcount on shadow of \"%s\" to %d\n",
            target_shadow->filter->name, target_shadow->refcount);
        
    /*
     * In shadow->current we just replace the rule.
     */
    filter->data = new_rule;
    
    /*
     * Update dialog if necessary.
     */
    if (work_filter == f)
        filter_gui_set_ruleset(shadow->current);
}



/*
 * filter_adapt_order:
 *
 * Reorders the filter according to the order in the user's
 * table in the gui. This should only be used after the
 * user has reordered the table. It cannot properly cope
 * with added or deleted items. This will also only work
 * if a filter is currently being displayed in the table.
 * If the filter dialog has not been initialized or not
 * filter is currently worked on, it will silently fail.
 */
void filter_adapt_order(void)
{
    GList *neworder = NULL;
    gint row;
    shadow_t *shadow;
    GtkCList *clist;

    if (!work_filter || filter_dialog == NULL)
        return;
   
    clist = GTK_CLIST(lookup_widget(filter_dialog, "clist_filter_rules"));

    /*
     * Create a new shadow if necessary.
     */
    shadow = shadow_find(work_filter);
    if (shadow == NULL)
        shadow = shadow_new(work_filter);

    /*
     * Assumption: every rule in shadow->current is also
     * bound to a row in the filter table. So we can free
     * this list and rebuild it in the right order from the
     * row data.
     */
    g_list_free(shadow->current);

    for (row = 0; row < clist->rows; row ++) {
        filter_t *f;

        f = gtk_clist_get_row_data(clist, row);
        g_assert(f != NULL);
        
        neworder = g_list_append(neworder, f);
    }

    shadow->current = neworder;
}


#define MATCH_RULE(filter, r, res)                                \
    (res)->props_set ++;                                          \
    (r)->match_count ++;                                          \
    (prop_count) ++;                                              \
    (r)->target->match_count ++;                                  \
                                                                  \
    if (gui_debug >= 10)                                                 \
        printf("matched rule: %s\n", filter_rule_to_gchar((r)));
   
/*
 * filter_apply:
 *
 * returns the number of properties set with this filter chain.
 * a property which was already set is not set again. The res
 * argument is changed depending on the rules that match.
 */
static int filter_apply
    (filter_t *filter, struct record *rec, filter_result_t *res)
{
    size_t namelen;
	char *l_name;
    GList *list;
    gint prop_count = 0;
    gboolean do_abort = FALSE;

    g_assert(filter != NULL);
    g_assert(rec != NULL);
    g_assert(res != NULL);

    /*
     * We only try to prevent circles or the filter is inactive.
     */
    if ((filter->visited == TRUE) || !filter_is_active(filter)) {
        return 0;
    }

    filter->visited = TRUE;

    list = filter->ruleset;

	namelen = strlen(rec->name);
	l_name = g_malloc(sizeof(char) * (namelen + 1));
	strlower(l_name, rec->name);

	list = g_list_first(list);
	while ((list != NULL) && (res->props_set < MAX_FILTER_PROP) && !do_abort) {
		size_t n;
		int i;
		rule_t *r; 
        gboolean match = FALSE;

        r = (rule_t *)list->data;
        if (gui_debug >= 10)
            printf("trying to match against: %s\n", filter_rule_to_gchar(r));

        if (RULE_IS_ACTIVE(r)) {
            switch (r->type) {
            case RULE_JUMP:
                match = TRUE;
                break;
            case RULE_TEXT:
                switch (r->u.text.type) {
                case RULE_TEXT_EXACT:
                    if (strcmp(r->u.text.case_sensitive ? rec->name : l_name,
                            r->u.text.match) == 0)
                        match = TRUE;
                    break;
                case RULE_TEXT_PREFIX:
                    if (strncmp(r->u.text.case_sensitive ? rec->name : l_name,
                            r->u.text.match, r->u.text.matchlen) == 0)
                        match = TRUE;
                    break;
                case RULE_TEXT_WORDS:	/* Contains ALL the words */
                    {
                        GList *l;
						gboolean failed = FALSE;
        
                        for (
                            l = g_list_first(r->u.text.u.words);
                            l && !failed; 
                            l = g_list_next(l)
                        ) {
                            if (
								pattern_qsearch((cpattern_t *)l->data,
                                 r->u.text.case_sensitive ? rec->name : l_name,
								 0, 0, qs_any) == NULL
							)
                                failed = TRUE;
                        }

						match = !failed;
                    }
                    break;
                case RULE_TEXT_SUFFIX:
                    n = r->u.text.matchlen;
                    if (namelen > n
                        && strcmp((r->u.text.case_sensitive
                               ? rec->name : l_name) + namelen
                              - n, r->u.text.match) == 0)
                        match = TRUE;
                    break;
                case RULE_TEXT_SUBSTR: 
                    if (
						pattern_qsearch(r->u.text.u.pattern,
                                r->u.text.case_sensitive ? rec->name : l_name,
								0, 0, qs_any) != NULL
					)
                        match = TRUE;
                    break;
                case RULE_TEXT_REGEXP:
                    if ((i = regexec(r->u.text.u.re, rec->name,
                             0, NULL, 0)) == 0)
                        match = TRUE;
                    if (i == REG_ESPACE)
                        g_warning("regexp memory overflow");
                    break;
                default:
                    g_error("Unknown text rule type: %d",
                        r->u.text.type);
                }
                break;
            case RULE_IP:
                if ((rec->results_set->ip & r->u.ip.mask) == r->u.ip.addr)
                    match = TRUE;
                break;
            case RULE_SIZE:
                if (rec->size >= r->u.size.lower && 
                    rec->size <= r->u.size.upper)
                    match = TRUE;
                break;
            case RULE_SHA1:
                if (rec->sha1 == r->u.sha1.hash)
                    match = TRUE;
                else if ((rec->sha1 != NULL) &&  r->u.sha1.hash != NULL)
                    if (memcmp(rec->sha1, r->u.sha1.hash, SHA1_RAW_SIZE) == 0)
                        match = TRUE;
                break;
            case RULE_FLAG:
                {
                    gboolean stable_match;
                    gboolean busy_match;
                    gboolean push_match;
    
                    stable_match = 
                        ((r->u.flag.busy == RULE_FLAG_SET) &&
                         (rec->results_set->status & ST_BUSY)) ||
                        ((r->u.flag.busy == RULE_FLAG_UNSET) &&
                         !(rec->results_set->status & ST_BUSY)) ||
                        (r->u.flag.busy == RULE_FLAG_IGNORE);

                    busy_match =
                        ((r->u.flag.push == RULE_FLAG_SET) &&
                         (rec->results_set->status & ST_FIREWALL)) ||
                        ((r->u.flag.push == RULE_FLAG_UNSET) &&
                         !(rec->results_set->status & ST_FIREWALL)) ||
                        (r->u.flag.push == RULE_FLAG_IGNORE);
                
                    push_match = 
                        ((r->u.flag.stable == RULE_FLAG_SET) &&
                         (rec->results_set->status & ST_UPLOADED)) ||
                        ((r->u.flag.stable == RULE_FLAG_UNSET) &&
                         !(rec->results_set->status & ST_UPLOADED)) ||
                        (r->u.flag.stable == RULE_FLAG_IGNORE);
                     
                    match = stable_match && busy_match && push_match;
                }
                break;
            case RULE_STATE:
                {
                    gboolean display_match;
                    gboolean download_match;

                    display_match =
                        (r->u.state.display == FILTER_PROP_STATE_IGNORE) ||
                        (res->props[FILTER_PROP_DISPLAY].state 
                            == r->u.state.display);
                    
                    download_match = 
                        (r->u.state.download == FILTER_PROP_STATE_IGNORE) ||
                        (res->props[FILTER_PROP_DOWNLOAD].state
                            == r->u.state.download);
            
                    match = display_match && download_match;
                }
                break;
            default:
                g_error("Unknown rule type: %d", r->type);
                break;
            }
        }
        /*
         * If negate is set, we invert the meaning of match.
         */

		if (RULE_IS_NEGATED(r) && RULE_IS_ACTIVE(r))
			match = !match;

        /*
         * Try to match the builtin rules, but don't act on matches
         * that would change a result property that was already
         * defined.
         */
        if (match) {
            if (r->target == filter_return) {
                do_abort = TRUE;
                r->match_count ++;
                r->target->match_count ++;
            } else
            if ((r->target == filter_show)) {
                if (!res->props[FILTER_PROP_DISPLAY].state) {

                    res->props[FILTER_PROP_DISPLAY].state =
                        FILTER_PROP_STATE_DO;

                    MATCH_RULE(filter, r, res);
                }
            } else 
            if (r->target == filter_drop) {
                if (!res->props[FILTER_PROP_DISPLAY].state) {

                    res->props[FILTER_PROP_DISPLAY].state =
                        FILTER_PROP_STATE_DONT;
                    res->props[FILTER_PROP_DISPLAY].user_data =
                        GINT_TO_POINTER(RULE_IS_SOFT(r) ? 1 : 0);

                    MATCH_RULE(filter, r, res);
                }
            } else 
            if (r->target == filter_download){
                if (!res->props[FILTER_PROP_DOWNLOAD].state) {
                
                    res->props[FILTER_PROP_DOWNLOAD].state =
                        FILTER_PROP_STATE_DO;

                    MATCH_RULE(filter, r, res);
                }
            } else 
            if (r->target == filter_nodownload) {
                if (!res->props[FILTER_PROP_DOWNLOAD].state) {
                
                    res->props[FILTER_PROP_DOWNLOAD].state =
                        FILTER_PROP_STATE_DONT;

                    MATCH_RULE(filter, r, res);
                }
            } else {
                /*
                 * We have a matched rule the target is not a builtin
                 * rule, so it must be a subchain. We gosub.
                 */
                prop_count += filter_apply(r->target, rec, res);
                r->match_count ++;
            }
        } else {
            r->fail_count ++;
        }

		list = g_list_next(list);
	}
    G_FREE_NULL(l_name);

    filter->visited = FALSE;
    filter->fail_count += MAX_FILTER_PROP - prop_count;
    filter->match_count += prop_count;
    return prop_count;
}



/*
 * filter_record:
 *
 * Check a particular record against the search filter and the global
 * filters. Returns a filter_property_t array with MAX_FILTER_PROP
 * rows. This must be freed with filter_free_properties.
 */
filter_result_t *filter_record(search_t *sch, record_t *rec)
{
    gboolean filtered;
    filter_result_t *result;
    gint i;

    g_assert(sch != NULL);
    g_assert(rec != NULL);

    /*
     * Initialize all properties with FILTER_PROP_STATE_UNKNOWN and
     * the props_set count with 0;
     */
    result = walloc0(sizeof(*result));

    filtered =  
        ((sch->filter->ruleset != NULL) && 
            filter_is_active(sch->filter)) ||
        ((filter_global_pre->ruleset != NULL) && 
            filter_is_active(filter_global_pre)) ||
        ((filter_global_post->ruleset != NULL) &&
            filter_is_active(sch->filter));

    filter_apply(filter_global_pre, rec, result);

    /*
     * If not decided check if the filters for this search apply.
     */
    if (result->props_set < MAX_FILTER_PROP)
        filter_apply(sch->filter, rec, result);

    /*
     * If it has not yet been decided, try the global filter
     */
	if (result->props_set < MAX_FILTER_PROP)
		filter_apply(filter_global_post, rec, result);
    
    /* FIXME: this does no longer give useful output
    if (gui_debug >= 5) {
        printf("result %d for search \"%s\" matching \"%s\" (%s)\n",
            r, sch->query, rec->name, 
            filtered ? "filtered" : "unfiltered");
    }
    */

    /*
     * Set the defaults for the props that are still in UNKNOWN state.
     */
    for (i = 0; i < MAX_FILTER_PROP; i ++) {
        switch(i) {
        case FILTER_PROP_DISPLAY:
            if (!result->props[i].state) {
                result->props[i].state = 
                    FILTER_PROP_STATE_DO;
                result->props_set ++;
            }
            break;
        case FILTER_PROP_DOWNLOAD:
            if (!result->props[i].state) {
                result->props[i].state = 
                    FILTER_PROP_STATE_DONT;
                result->props_set ++;
            }
            break;
        }
    }

	return result;
}



/*
 * filters_shutdown
 *
 * Free global filters and save state.
 */
void filter_shutdown(void)
{
    GList *f;

    if (gui_debug >= 5)
        printf("shutting down filters\n");

    /*
     * It is important that all searches have already been closed.
     * Since it is not allowd to use a bound filter as a target,
     * a bound filter will always have a refcount of 0. So it is
     * not a problem just closing the searches.
     * But for the free filters, we have to prune all rules before
     * we may free the filers, because we have to reduce the 
     * refcount on every filter to 0 before we are allowed to free it.
     */
    for (f = filters; f != NULL; f = f->next) {
        filter_t *filter = (filter_t*) f->data;
        GList *copy = g_list_copy(filter->ruleset);

        /*
         * Since filter_remove_rule modifies filter->ruleset, we
         * have to copy the ruleset before we start puring.
         */

        /*
         * We don't want to create any shadows again since a
         * shadowed filter may not be freed, so we use 
         * filter_remove_rule. 
         */

 		G_LIST_FOREACH_SWAPPED(copy, filter_remove_rule, filter);
        g_list_free(copy);
    }

    /*
     * Now we remove the filters. So we may not traverse. We just
     * free the first filter until none is left. This will also
     * clean up the builtin filters (filter_drop/show) and the
     * global filters;
     */
    for (f = filters; f != NULL; f = filters)
        filter_free(f->data);
}



/*
 * filter_init
 *
 * Initialize global filters.
 */
void filter_init(void)
{
    filter_global_pre  = filter_new("Global (pre)");
    filter_global_post = filter_new("Global (post)");
    filter_show        = filter_new("DISPLAY");
    filter_drop        = filter_new("DON'T DISPLAY");
    filter_download    = filter_new("DOWNLOAD");
    filter_nodownload  = filter_new("DON'T DOWNLOAD");
    filter_return      = filter_new("RETURN");

    filters = g_list_append(filters, filter_global_pre);
    filters = g_list_append(filters, filter_global_post);
    filters = g_list_append(filters, filter_show);
    filters = g_list_append(filters, filter_drop);
    filters = g_list_append(filters, filter_download);
    filters = g_list_append(filters, filter_nodownload);
    filters = g_list_append(filters, filter_return);

    filters_current = g_list_copy(filters);

    popup_filter_rule = create_popup_filter_rule();
}



/*
 * filter_update_targets:
 *
 * Trigger a rebuild of the target combos.
 */
void filter_update_targets(void)
{
    filter_gui_rebuild_target_combos(filters_current);
}



/*
 * filter_timer:
 *
 * Periodically update the filter display with current data
 */
void filter_timer(void)
{
    filter_gui_update_filter_stats();
    filter_gui_update_rule_stats();
}



/*
 * filter_rule_reset_stats:
 *
 * Reset the rule stats for a given rule.
 */
inline void filter_rule_reset_stats(rule_t *rule)
{
    g_assert(rule != NULL);

    rule->match_count = rule->fail_count = 0;
}



/*
 * filter_reset_stats:
 *
 * Reset the stats for a given filter.
 */
inline void filter_reset_stats(filter_t *filter)
{
    g_assert(filter != NULL);

    filter->match_count = filter->fail_count = 0;
}



/*
 * filter_set_enabled:
 *
 * Change the "enabled" flag of a filter.
 */
void filter_set_enabled(filter_t *filter, gboolean active)
{
    shadow_t *shadow;
    static gboolean locked = FALSE;

    g_assert(filter != NULL);

    if (locked)
        return;

    locked = TRUE;

    shadow = shadow_find(filter);
    if (shadow == NULL)
        shadow = shadow_new(filter);
    
    if (active) {
        set_flags(shadow->flags, FILTER_FLAG_ACTIVE);
    } else {
        clear_flags(shadow->flags, FILTER_FLAG_ACTIVE);
    }

    filter_gui_filter_set_enabled(work_filter, active);

    locked = FALSE;
}

/*
 * filter_free_properties:
 *
 * Free a filter_result returned by filter_record
 * after it has been processed.
 */
void filter_free_result(filter_result_t *res)
{
    gint i;

    g_assert(res != NULL);

    /*
     * Since every property type can need a special handling
     * for freeing the user data, we handle that here. Currently
     * no property needs this.
     */
    for (i = 0; i < MAX_FILTER_PROP; i ++) {
        switch (i) {
        case FILTER_PROP_DISPLAY:
            break;
        case FILTER_PROP_DOWNLOAD:
            break;
        default:
            g_assert_not_reached();
        };
    }

    wfree(res, sizeof(*res));
}

/*
 * filter_is_valid_in_session:
 *
 * Checks wether a filter is existant in a filter editing session.
 * If no session is started it checks wether the filter is valid
 * in outside the session.
 */
gboolean filter_is_valid_in_session(filter_t *f)
{
    if (f == NULL)
        return FALSE;
    else
        return g_list_find(filters_current, f) != NULL;
}

/*
 * filter_find_by_name_in_session:
 *
 * Returns the filter with the given name in the session if it 
 * exists, otherwise returns NULL. If no session is started, it
 * looks in the normal filter list.
 */
filter_t *filter_find_by_name_in_session(gchar *name)
{
    GList *l;

    for (l = filters_current; l != NULL; l = l->next) {
        filter_t *filter = (filter_t *) l->data;

        if (strcmp(filter->name, name) == 0)
            return filter;
    }
    return NULL;
}

gboolean filter_is_global(filter_t *f)
{
    return ((f == filter_global_pre) || (f == filter_global_post));
}

gboolean filter_is_builtin(filter_t *f)
{
    return ((f == filter_show) || (f == filter_drop) || 
            (f == filter_download) || (f == filter_nodownload) ||
            (f == filter_return));
}

inline filter_t *filter_get_drop_target(void)
{
    return filter_drop;
}

inline filter_t *filter_get_show_target(void)
{
    return filter_show;
}

inline filter_t *filter_get_download_target(void)
{
    return filter_download;
}

inline filter_t *filter_get_nodownload_target(void)
{
    return filter_nodownload;
}

inline filter_t *filter_get_return_target(void)
{
    return filter_return;
}

inline filter_t *filter_get_global_pre(void)
{
    return filter_global_pre;
}

inline filter_t *filter_get_global_post(void)
{
    return filter_global_post;
}

/*
 * Adds a drop SHA1 rule to specified filter.
 */
void filter_add_drop_sha1_rule(record_t *rec, filter_t *filter)
{
    rule_t *rule;

    g_assert(rec != NULL);
    g_assert(filter != NULL);

    rule = filter_new_sha1_rule(rec->sha1, rec->name,
        filter_get_drop_target(), RULE_FLAG_ACTIVE);

    filter_append_rule(filter, rule);
}

/*
 * Adds a drop filename rule to specified filter.
 */
void filter_add_drop_name_rule(record_t *rec, filter_t *filter)
{
    rule_t *rule;

    g_assert(rec != NULL);
    g_assert(filter != NULL);

    rule = filter_new_text_rule(rec->name, RULE_TEXT_EXACT, TRUE,
        filter_get_drop_target(), RULE_FLAG_ACTIVE);

    filter_append_rule(filter, rule);
}

/*
 * Adds a drop host rule to specified filter.
 */
void filter_add_drop_host_rule(record_t *rec, filter_t *filter)
{
    rule_t *rule;

    g_assert(rec != NULL);
    g_assert(filter != NULL);

    rule = filter_new_ip_rule(rec->results_set->ip, 0xFFFFFFFF,
        filter_get_drop_target(), RULE_FLAG_ACTIVE);

    filter_append_rule(filter, rule);
}

/*
 * Adds a download SHA1 rule to specified filter.
 */
void filter_add_download_sha1_rule(record_t *rec, filter_t *filter)
{
    g_assert(rec != NULL);
    g_assert(filter != NULL);

    if (rec->sha1) {
        rule_t *rule;

        rule = filter_new_sha1_rule(rec->sha1, rec->name,
            filter_get_download_target(), RULE_FLAG_ACTIVE);

        filter_append_rule(filter, rule);
    }
}

/*
 * Adds a download filename rule to specified filter.
 */
void filter_add_download_name_rule(record_t *rec, filter_t *filter)
{
    rule_t *rule;

    g_assert(rec != NULL);
    g_assert(filter != NULL);

    rule = filter_new_text_rule(rec->name, RULE_TEXT_EXACT, TRUE,
        filter_get_download_target(), RULE_FLAG_ACTIVE);

    filter_append_rule(filter, rule);
}
