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
 * Common GUI search routines.
 */

#include "gui.h"

RCSID("$Id$");

#include "search.h"
#include "settings.h"

#ifdef HAS_LIBXML2
#include "search_xml.h"
#include <libxml/parser.h>
#endif

#include "if/gui_property_priv.h"
#include "if/gnet_property.h"
#include "if/core/downloads.h"
#include "if/core/guid.h"
#include "if/bridge/ui2c.h"

#include "lib/atoms.h"
#include "lib/fuzzy.h"
#include "lib/file.h"
#include "lib/glib-missing.h"
#include "lib/vendors.h"
#include "lib/utf8.h"
#include "lib/zalloc.h"
#include "lib/walloc.h"
#include "lib/override.h"	/* Must be the last header included */

static search_t *current_search  = NULL; /* The search currently displayed */

static zone_t *rs_zone;		/* Allocation of results_set */
static zone_t *rc_zone;		/* Allocation of record */

static const gchar search_file[] = "searches"; /* "old" file to searches */

static gchar tmpstr[1024];

static GSList *accumulated_rs = NULL;

static GtkLabel *label_items_found = NULL;

/**
 * Human readable translation of servent trailer open flags.
 * Decompiled flags are listed in the order of the table.
 */
static struct {
	guint32 flag;
	const gchar *status;
} open_flags[] = {
	{ ST_BUSY,			N_("busy") },
	{ ST_UPLOADED,		N_("stable") },		/* Allows uploads -> stable */
	{ ST_FIREWALL,		N_("push") },
	{ ST_PUSH_PROXY,	N_("proxy") },
};

search_t *search_gui_get_current_search(void)	{ return current_search; }
void search_gui_forget_current_search(void)		{ current_search = NULL; }
void search_gui_current_search(search_t *sch)	{ current_search = sch; }


/**
 * Free the alternate locations held within a file record.
 */
void
search_gui_free_alt_locs(record_t *rc)
{
	gnet_host_vec_t *alt = rc->alt_locs;

	g_assert(alt != NULL);

	wfree(alt->hvec, alt->hvcnt * sizeof(*alt->hvec));
	wfree(alt, sizeof(*alt));

	rc->alt_locs = NULL;
}

/**
 * Free the push proxies held within a result set.
 */
void
search_gui_free_proxies(results_set_t *rs)
{
	gnet_host_vec_t *v = rs->proxies;

	g_assert(v != NULL);

	wfree(v->hvec, v->hvcnt * sizeof(*v->hvec));
	wfree(v, sizeof(*v));

	rs->proxies = NULL;
}

/**
 * Free one file record.
 *
 * Those records may be inserted into some `dups' tables, at which time they
 * have their refcount increased.  They may later be removed from those tables
 * and they will have their refcount decreased.
 *
 * To ensure some level of sanity, we ask our callers to explicitely check
 * for a refcount to be zero before calling us.
 */
void
search_gui_free_record(record_t *rc)
{
	g_assert(rc->refcount == 0);

	atom_str_free(rc->name);
	if (rc->ext != NULL)
        atom_str_free(rc->ext);
	if (rc->tag != NULL)
		atom_str_free(rc->tag);
	if (rc->info != NULL)
		atom_str_free(rc->info);
	if (rc->sha1 != NULL)
		atom_sha1_free(rc->sha1);
	if (rc->alt_locs != NULL)
		search_gui_free_alt_locs(rc);
	rc->refcount = -1;
	rc->sha1 = GUINT_TO_POINTER(0x01020304);
	zfree(rc_zone, rc);
}

/**
 * This routine must be called when the results_set has been dispatched to
 * all the opened searches.
 *
 * All the records that have not been used by a search are removed.
 */
void
search_gui_clean_r_set(results_set_t *rs)
{
	GSList *sl;
    GSList *sl_remove = NULL;

	g_assert(rs->refcount);		/* If not dispatched, should be freed */

    /*
     * Collect empty searches.
     */
    for (sl = rs->records; sl != NULL; sl = g_slist_next(sl)) {
		record_t *rc = (record_t *) sl->data;

		if (rc->refcount == 0)
			sl_remove = g_slist_prepend(sl_remove, (gpointer) rc);
    }

    /*
     * Remove empty searches from record set.
     */
	for (sl = sl_remove; sl != NULL; sl = g_slist_next(sl)) {
		record_t *rc = (record_t *) sl->data;

		search_gui_free_record(rc);
		rs->records = g_slist_remove(rs->records, rc);
		rs->num_recs--;
	}

    g_slist_free(sl_remove);
}

/**
 * Free one results_set.
 *
 * Those records may be shared between several searches.  So while the refcount
 * is positive, we just decrement it and return without doing anything.
 */
void
search_gui_free_r_set(results_set_t *rs)
{
	GSList *sl;

    g_assert(rs != NULL);


	/*
	 * It is conceivable that some records were used solely by the search
	 * dropping the result set.  Therefore, if the refcount is not 0,  we
	 * pass through search_clean_r_set().
	 */

	if (--(rs->refcount) > 0) {
		search_gui_clean_r_set(rs);
		return;
	}

    /* 
     * Free list of searches set was intended for.
     */

    if (rs->schl) {
        g_slist_free(rs->schl);
        rs->schl = NULL;
    }

	/*
	 * Because noone refers to us any more, we know that our embedded records
	 * cannot be held in the hash table anymore.  Hence we may call the
	 * search_free_record() safely, because rc->refcount must be zero.
	 */

	for (sl = rs->records; sl != NULL; sl = g_slist_next(sl))
		search_gui_free_record((record_t *) sl->data);

    if (rs->guid)
		atom_guid_free(rs->guid);
	if (rs->version)
		atom_str_free(rs->version);
	if (rs->proxies)
		search_gui_free_proxies(rs);
	if (rs->hostname)
		atom_str_free(rs->hostname);

	g_slist_free(rs->records);
	zfree(rs_zone, rs);
}

/**
 * Dispose of an empty search results, whose records have all been
 * unreferenced by the searches.  The results_set is therefore an
 * empty shell, useless.
 */
void
search_gui_dispose_results(results_set_t *rs)
{
	gint refs = 0;
	const GList *l;

	g_assert(rs->num_recs == 0);
	g_assert(rs->refcount > 0);

	/*
	 * A results_set does not point back to the searches that still
	 * reference it, so we have to do that manually.
	 */

	for (l = search_gui_get_searches(); NULL != l; l = g_list_next(l)) {
		search_t *sch = (search_t *) l->data;
	
		if (NULL != sch->r_sets && hash_list_contains(sch->r_sets, rs)) {
			refs++;			/* Found one more reference to this search */
			hash_list_remove(sch->r_sets, rs);
		}
	}

	g_assert(rs->refcount == refs);		/* Found all the searches */

	rs->refcount = 1;
	search_gui_free_r_set(rs);
}

/**
 * Add a reference to the record but don't dare to redeem it!
 */ 
void
search_gui_ref_record(record_t *rc)
{
	g_assert(rc->refcount >= 0);
	rc->refcount++;
}

/**
 * Remove one reference to a file record.
 *
 * If the record has no more references, remove it from its parent result
 * set and free the record physically.
 */
void
search_gui_unref_record(record_t *rc)
{
	results_set_t *rs;

	g_assert(rc->refcount > 0);

	if (--(rc->refcount) > 0)
		return;

	/*
	 * Free record, and remove it from the parent's list.
	 */
	rs = rc->results_set;
	search_gui_free_record(rc);

	rs->records = g_slist_remove(rs->records, rc);
	rs->num_recs--;

	g_assert(rs->num_recs || rs->records == NULL);

	/*
	 * We can't free the results_set structure right now if it does not
	 * hold anything because we don't know which searches reference it.
	 */

	if (rs->num_recs == 0)
		search_gui_dispose_results(rs);
}

/** 
 * Free all the results_set's of a search 
 */
static inline void
free_r_sets_helper(gpointer data, gpointer user_data)
{
	results_set_t *rs = data;
	search_gui_free_r_set(rs);
}

void
search_gui_free_r_sets(search_t *sch)
{
	g_assert(sch != NULL);
	g_assert(sch->dups != NULL);
	g_assert(g_hash_table_size(sch->dups) == 0); /* All records were cleaned */

	if (NULL != sch->r_sets) {
		hash_list_foreach(sch->r_sets, free_r_sets_helper, NULL);
		hash_list_free(sch->r_sets);
		sch->r_sets = NULL;
	}
}

guint
search_gui_hash_func(const record_t *rc)
{
	/* Must use same fields as search_hash_key_compare() --RAM */
	return
		GPOINTER_TO_UINT(rc->sha1) ^	/* atom! (may be NULL) */
		GPOINTER_TO_UINT(rc->results_set->guid) ^	/* atom! */
		(NULL != rc->sha1 ? 0 : g_str_hash(rc->name)) ^
		rc->size ^
		rc->results_set->ip ^
		rc->results_set->port;
}

gint
search_gui_hash_key_compare(const record_t *rc1, const record_t *rc2)
{
	/* Must compare same fields as search_hash_func() --RAM */
	return rc1->size == rc2->size
		&& rc1->results_set->ip == rc2->results_set->ip
		&& rc1->results_set->port == rc2->results_set->port
		&& rc1->results_set->guid == rc2->results_set->guid	/* atom! */
		&& (rc1->sha1 != NULL /* atom! */
				? rc1->sha1 == rc2->sha1 : (0 == strcmp(rc1->name, rc2->name)));
}

/**
 * Remove reference to results in our search.
 * Last one to remove it will trigger a free.
 */
void
search_gui_remove_r_set(search_t *sch, results_set_t *rs)
{
	hash_list_remove(sch->r_sets, rs);
	search_gui_free_r_set(rs);
}

/**
 * Check to see whether we already have a record for this file.
 * If we do, make sure that the index is still accurate,
 * otherwise inform the interested parties about the change.
 *
 * Returns true if the record is a duplicate.
 */
gboolean
search_gui_result_is_dup(search_t *sch, record_t *rc)
{
	union {
		record_t *rc;
		gpointer ptr;
	} old;
	gpointer dummy;
	gboolean found;

	found = g_hash_table_lookup_extended(sch->dups, rc, &old.ptr, &dummy);

	if (!found)
		return FALSE;

	/*
	 * Actually, if the index is the only thing that changed,
	 * we want to overwrite the old one (and if we've
	 * got the download queue'd, replace it there too.
	 *		--RAM, 17/12/2001 from a patch by Vladimir Klebanov
	 *
	 * XXX needs more care: handle is_old, and use GUID for patching.
	 * XXX the client may change its GUID as well, and this must only
	 * XXX be used in the hash table where we record which downloads are
	 * XXX queued from whom.
	 * XXX when the GUID changes for a download in push mode, we have to
	 * XXX change it.  We have a new route anyway, since we just got a match!
	 */

	if (rc->index != old.rc->index) {
		if (gui_debug)
			g_warning("Index changed from %u to %u at %s for %s",
				old.rc->index, rc->index, guid_hex_str(rc->results_set->guid),
				rc->name);
		guc_download_index_changed(
			rc->results_set->ip,		/* This is for optimizing lookups */
			rc->results_set->port,
			rc->results_set->guid,		/* This is for formal identification */
			old.rc->index,
			rc->index);
		old.rc->index = rc->index;
	}

	return TRUE;		/* yes, it's a duplicate */
}

/**
 * Returns a pointer to gui_search_t from gui_searches which has
 * sh as search_handle. If none is found, return NULL.
 */
search_t *
search_gui_find(gnet_search_t sh) 
{
    const GList *l;
    
    for (l = search_gui_get_searches(); l != NULL; l = g_list_next(l)) {
		search_t *s = l->data;

        if (s->search_handle == sh) {
            if (gui_debug >= 15)
                printf("search [%s] matched handle %x\n", s->query, sh);

            return s;
        }
    }

    return NULL;
}

/**
 * Create a new GUI record within `rs' from a Gnutella record.
 */
record_t *
search_gui_create_record(results_set_t *rs, gnet_record_t *r) 
{
    record_t *rc;

    g_assert(r != NULL);
    g_assert(rs != NULL);

    rc = (record_t *) zalloc(rc_zone);

    rc->results_set = rs;
    rc->refcount = 0;

#ifdef USE_GTK2
    rc->name = atom_str_get(r->name);
    /* Gtk2 extracts this in search_gui2.c because of UTF8 issues. */
    rc->ext  = NULL; 
#else
	rc->name = NULL;
	if (!is_ascii_string(r->name)) {
		size_t len;

		len = strlen(r->name);
		if (utf8_is_valid_string(r->name, len))
    		rc->name = utf8_to_locale(r->name, len);
	}
	rc->name = atom_str_get(rc->name ? rc->name : r->name);
    rc->ext  = atom_str_get(search_gui_extract_ext(rc->name));
#endif
    rc->size = r->size;
    rc->index = r->index;
    rc->sha1 = r->sha1 != NULL ? atom_sha1_get(r->sha1) : NULL;
    rc->tag = r->tag != NULL ? atom_str_get(r->tag) : NULL;
	rc->info = NULL;
   	rc->flags = r->flags;
	rc->alt_locs = NULL;

	if (r->alt_locs != NULL) {
		gnet_host_vec_t *a = r->alt_locs;				/* Original from core */
		gnet_host_vec_t *alt = walloc(sizeof(*alt));	/* GUI copy */
		gint hlen = a->hvcnt * sizeof(*a->hvec);

		alt->hvec = walloc(hlen);
		alt->hvcnt = a->hvcnt;
		memcpy(a->hvec, alt->hvec, hlen);

		rc->alt_locs = alt;
	}
	
    return rc;
}

/**
 * Create a new GUI result set from a Gnutella one.
 */
results_set_t *
search_gui_create_results_set(GSList *schl, const gnet_results_set_t *r_set)
{
    results_set_t *rs;
    GSList *sl;
	gint ignored = 0;
    
    rs = (results_set_t *) zalloc(rs_zone);

    rs->refcount = 0;
    rs->schl = g_slist_copy(schl);

    rs->guid = atom_guid_get(r_set->guid);
    rs->ip = r_set->ip;
    rs->port = r_set->port;
    rs->status = r_set->status;
    rs->speed = r_set->speed;
    rs->stamp = r_set->stamp;
    memcpy(rs->vendor, r_set->vendor, sizeof(rs->vendor));
	rs->version = r_set->version ? atom_str_get(r_set->version) : NULL;
	rs->hostname = r_set->hostname ? atom_str_get(r_set->hostname) : NULL;
	rs->udp_ip = r_set->udp_ip;

    rs->num_recs = 0;
    rs->records = NULL;
	rs->proxies = NULL;

    for (sl = r_set->records; sl != NULL; sl = g_slist_next(sl)) {
        record_t *rc;
		gnet_record_t *grc = (gnet_record_t *) sl->data;

		if (!(grc->flags & SR_DONT_SHOW)) {
			rc = search_gui_create_record(rs, grc);
			rs->records = g_slist_prepend(rs->records, rc);
			rs->num_recs ++;
		} else
			ignored++;
    }

    g_assert(rs->num_recs + ignored == r_set->num_recs);

    return rs;
}

/**
 * Initialize common structures.
 */
void
search_gui_common_init(void)
{
	rs_zone = zget(sizeof(results_set_t), 1024);
	rc_zone = zget(sizeof(record_t), 1024);

	label_items_found = GTK_LABEL(
		lookup_widget(main_window, "label_items_found"));
}

/**
 * Destroy common structures.
 */
void
search_gui_common_shutdown(void)
{
	zdestroy(rs_zone);
	zdestroy(rc_zone);

	rs_zone = rc_zone = NULL;
}

/**
 * Check for alternate locations in the result set, and enqueue the downloads
 * if there are any.  Then free the alternate location from the record.
 */
void
search_gui_check_alt_locs(results_set_t *rs, record_t *rc)
{
	gint i;
	gnet_host_vec_t *alt = rc->alt_locs;

	g_assert(alt != NULL);
	g_assert(rs->proxies == NULL);	/* Since we downloaded record already */

	for (i = alt->hvcnt - 1; i >= 0; i--) {
		gnet_host_t *h = &alt->hvec[i];

		if (!host_is_valid(h->ip, h->port))
			continue;

		guc_download_auto_new(rc->name, rc->size, URN_INDEX, 
			h->ip, h->port, blank_guid, rs->hostname,
			rc->sha1, rs->stamp, FALSE, TRUE, NULL, NULL);
	}

	search_gui_free_alt_locs(rc);
}

#ifndef HAS_LIBXML2
/*
 * search_store_old
 *
 * Store pending non-passive searches.
 */
static void
search_store_old(void)
{
	const GList *l;
	FILE *out;
	file_path_t fp;

	file_path_set(&fp, settings_gui_config_dir(), search_file);
	out = file_config_open_write("searches", &fp);

	if (!out)
		return;

	file_config_preamble(out, "Searches");
	
	for (l = search_gui_get_searches(); l; l = g_list_next(l)) {
		const search_t *sch = (const search_t *) l->data;
		if (!sch->passive)
			fprintf(out, "%s\n", sch->query);
	}

	file_config_close(out, &fp);
}
#endif /* HAS_LIBXML2 */

/**
 * Persist searches to disk.
 */
void
search_gui_store_searches(void)
{
#ifdef HAS_LIBXML2
	char *path;

	search_store_xml();
    
	path = make_pathname(settings_gui_config_dir(), search_file);
	g_return_if_fail(NULL != path);

    if (file_exists(path)) {
		char *path_old;

      	path_old = g_strdup_printf("%s.old", path);
		if (NULL != path_old) {
        	g_warning(
            	_("Found old searches file. The search information has been\n"
            	"stored in the new XML format and the old file is renamed to\n"
            	"%s"), path_old);
        	if (-1 == rename(path, path_old))
          		g_warning(_("could not rename %s as %s: %s\n"
                	"The XML file will not be used "
					"unless this problem is resolved."),
                path, path_old, g_strerror(errno));
			G_FREE_NULL(path_old);
		}
    }
	G_FREE_NULL(path);
#else
    search_store_old();
#endif
}

/**
 * Retrieve search list and restart searches.
 * The searches are normally retrieved from ~/.gtk-gnutella/searches.
 */
static gboolean
search_retrieve_old(void)
{
	FILE *in;
	gint line;				/* File line number */
	file_path_t fp;

	file_path_set(&fp, settings_gui_config_dir(), search_file);
	in = file_config_open_read("old searches (gtkg pre v0.90)", &fp, 1);
	if (!in)
		return FALSE;

	/*
	 * Retrieval of each searches.
	 */

	line = 0;

	while (fgets(tmpstr, sizeof(tmpstr) - 1, in)) {	/* Room for trailing NUL */
		line++;

		if (tmpstr[0] == '#')
			continue;				/* Skip comments */

		if (tmpstr[0] == '\n')
			continue;				/* Allow arbitrary blank lines */

		(void) str_chomp(tmpstr, 0);	/* The search string */

		search_gui_new_search(tmpstr, 0, NULL);
		tmpstr[0] = '\0';
	}

	fclose(in);

    return TRUE;
}

/**
 * Retrieve searches from disk.
 */
void
search_gui_retrieve_searches(void)
{
#ifdef HAS_LIBXML2
	LIBXML_TEST_VERSION

    if (!search_retrieve_xml()) {
		if (search_retrieve_old()) {
        	g_warning(_("Found old searches file. Loaded it.\n"
            	"On exit the searches will be saved in the new XML format\n"
            	"You may remove \"searches.orig\"."));
    	}
	}

#else
    search_retrieve_old();
#endif /* HAS_LIBXML2 */
}

/**
 * Called to dispatch results to the search window.
 */
void
search_matched(search_t *sch, results_set_t *rs)
{
	guint32 old_items = sch->items;
   	gboolean need_push;			/* Would need a push to get this file? */
	gboolean skip_records;		/* Shall we skip those records? */
	GString *vinfo = g_string_sized_new(40);
	gchar *vendor;
    GdkColor *download_color;
    GdkColor *ignore_color;
    GdkColor *mark_color;
    GSList *l;
    gboolean send_pushes;
    gboolean is_firewalled;
	guint i;
	guint32 results_kept = 0;

    g_assert(sch != NULL);
    g_assert(rs != NULL);

	gui_search_get_colors(sch, &mark_color, &ignore_color, &download_color);

    vendor = lookup_vendor_name(rs->vendor);

   	if (vendor) {
		g_string_append(vinfo, vendor);
		if (rs->version) {
			g_string_append(vinfo, "/");
			g_string_append(vinfo, rs->version);
		}
	}

	for (i = 0; i < G_N_ELEMENTS(open_flags); i++) {
		if (rs->status & open_flags[i].flag) {
			if (vinfo->len)
				g_string_append(vinfo, ", ");
			g_string_append(vinfo, gettext(open_flags[i].status));
		}
	}

	if (vendor && !(rs->status & ST_PARSED_TRAILER)) {
		if (vinfo->len)
			g_string_append(vinfo, ", ");
		g_string_append(vinfo, _("<unparsed>"));
	}

	if (rs->status & ST_UDP) {
		sch->udp_qhits++;
		if (vinfo->len)
			g_string_append(vinfo, ", ");
		if (rs->ip == rs->udp_ip)
			g_string_append(vinfo, "udp");
		else {
			g_string_append(vinfo, "udp ");
			g_string_append(vinfo, ip_to_gchar(rs->udp_ip));
		}
	} else
		sch->tcp_qhits++;

	/*
	 * If we're firewalled, or they don't want to send pushes, then don't
	 * bother displaying results if they need a push request to succeed.
	 *		--RAM, 10/03/2002
	 */
    gnet_prop_get_boolean
		(PROP_SEND_PUSHES, &send_pushes, 0, 1);
    gnet_prop_get_boolean(PROP_IS_FIREWALLED, &is_firewalled, 0, 1);

	need_push = (rs->status & ST_FIREWALL) || !host_is_valid(rs->ip, rs->port);
	skip_records = (!send_pushes || is_firewalled) && need_push;

	if (gui_debug > 6)
		printf("search_matched: [%s] got hit with %d record%s (from %s) "
			"need_push=%d, skipping=%d\n",
			sch->query, rs->num_recs, rs->num_recs == 1 ? "" : "s",
			ip_port_to_gchar(rs->ip, rs->port), need_push, skip_records);

  	for (l = rs->records; l && !skip_records; l = l->next) {
		record_t *rc = (record_t *) l->data;
        filter_result_t *flt_result;
        gboolean downloaded = FALSE;

        if (gui_debug > 7)
            printf("search_matched: [%s] considering %s (%s)\n",
				sch->query, rc->name, vinfo->str);

        if (rc->flags & SR_DOWNLOADED)
			sch->auto_downloaded++;

        /*
	     * If the size is zero bytes,
		 * or we don't send pushes and it's a private IP,
		 * or if this is a duplicate search result,
		 *
		 * Note that we pass ALL records through search_gui_result_is_dup(),
		 * to be able to update the index/GUID of our records correctly, when
		 * we detect a change.
		 */

		if (search_gui_result_is_dup(sch, rc)) {
			sch->duplicates++;
			continue;
		}

       	if (skip_records) {
			sch->skipped++;
			continue;
		}

		if (rc->size == 0) {
			sch->ignored++;
			continue;
		}

        flt_result = filter_record(sch, rc);

        /*
         * Check whether this record was already scheduled for
         * download by the backend.
         */
        downloaded = rc->flags & SR_DOWNLOADED;
        
        /*
         * Now we check for the different filter result properties.
         */

        /*
         * Check for FILTER_PROP_DOWNLOAD:
         */
        if (!downloaded &&
            (flt_result->props[FILTER_PROP_DOWNLOAD].state ==
				FILTER_PROP_STATE_DO)
		) {
            guc_download_auto_new(rc->name, rc->size, rc->index, 
				rs->ip, rs->port, rs->guid, rs->hostname, rc->sha1, rs->stamp, 
				need_push, TRUE, NULL, rs->proxies);

			if (rs->proxies != NULL)
				search_gui_free_proxies(rs);

            downloaded = TRUE;
			sch->auto_downloaded++;
        }

		/*
		 * Don't show something we downloaded if they don't want it.
		 */

		if (downloaded && search_hide_downloaded) {
            filter_free_result(flt_result);
			results_kept++;
			sch->hidden++;
			continue;
        }
    
        /*
         * We start with FILTER_PROP_DISPLAY:
         */
        if (!(flt_result->props[FILTER_PROP_DISPLAY].state == 
                FILTER_PROP_STATE_DONT &&
            flt_result->props[FILTER_PROP_DISPLAY].user_data == 0) &&
			(int) results_kept++ >= 0 && /* Count as kept even if max results */
            sch->items < search_max_results
		) {
            GdkColor *fg_color = NULL;
            gboolean mark;

            sch->items++;
            g_hash_table_insert(sch->dups, rc, GINT_TO_POINTER(1));
            search_gui_ref_record(rc);

            mark = 
                (flt_result->props[FILTER_PROP_DISPLAY].state == 
                    FILTER_PROP_STATE_DONT) &&
                (flt_result->props[FILTER_PROP_DISPLAY].user_data == 
					GINT_TO_POINTER(1));

            if (rc->flags & SR_IGNORED) {
                /*
                 * Check whether this record will be ignored by the backend.
                 */
                fg_color = ignore_color;
            } else if (downloaded)
                fg_color = download_color;
            else
                fg_color = NULL;

            search_gui_add_record(sch, rc, vinfo, fg_color,
                mark ? mark_color : NULL);
        } else
			sch->ignored++;

        filter_free_result(flt_result);
    }

    /*
     * A result set may not be added more then once to a search!
     */

	if (NULL != sch->r_sets)
    	g_assert(!hash_list_contains(sch->r_sets, rs));
	else
		sch->r_sets = hash_list_new(); 

	/* Adds the set to the list */
	hash_list_prepend(sch->r_sets, (gpointer) rs);
	rs->refcount++;
   	g_assert(hash_list_contains(sch->r_sets, rs));
	g_assert(hash_list_first(sch->r_sets) == rs);

	if (old_items == 0 && sch == current_search && sch->items > 0)
		search_gui_set_clear_button_sensitive(TRUE);

	/*
	 * Update counters in the core-side of the search.
	 *
	 * NB: we need to call guc_search_add_kept() even if we kept nothing,
	 * that is required for proper dynamic querying support by leaf nodes.
	 */

	guc_search_update_items(sch->search_handle, sch->items);
	guc_search_add_kept(sch->search_handle, results_kept);

	/*
	 * Disable search when the maximum amount of items is shown: they need
	 * to make some room to allow the search to continue.
	 */

	if (sch->items >= search_max_results && !sch->passive)
		gui_search_set_enabled(sch, FALSE);

	/*
	 * XXX When not for current_search, unseen_items is increased even if
	 * XXX we're not at the search pane.  Is this a problem?
	 */

	if (sch == current_search)
		search_gui_update_items(sch);
	else
		sch->unseen_items += sch->items - old_items;

	if (time(NULL) - sch->last_update_time < TAB_UPDATE_TIME)
		gui_search_update_tab_label(sch);

  	g_string_free(vinfo, TRUE);
}

/**
 * Update the label string showing search stats.
 */
void
search_gui_update_items(struct search *sch)
{
    if (sch) {
        const gchar *str = sch->passive ? _("(passive search) ") : "";
    
		gm_snprintf(tmpstr, sizeof(tmpstr), _("%s%s%u %s "
			"(%u skipped, %u ignored, %u hidden, %u auto-d/l, %u dups)"
			" Hits: %u (%u TCP, %u UDP)"),
			sch->enabled ? "" : _("[stopped] "),
			str, sch->items, (sch->items > 1) ? _("items") : _("item"),
			sch->skipped, sch->ignored, sch->hidden, sch->auto_downloaded,
			sch->duplicates,
			sch->tcp_qhits + sch->udp_qhits, sch->tcp_qhits, sch->udp_qhits);
    } else
        g_strlcpy(tmpstr, _("No search"), sizeof(tmpstr));

	gtk_label_set(label_items_found, tmpstr);
}

/**
 * Determines wether two records are equal enough to warrant
 * autoselection. fuzzy_threshold (etc) is an argument to avoid
 * fetching the property too often.
 */
gboolean
search_gui_autoselect_cmp(record_t *rc, record_t *rc2, 
    gboolean search_autoselect,
    gboolean search_autoselect_ident,
    gboolean search_autoselect_fuzzy,
    guint32 fuzzy_threshold)
{
    gboolean size_match;
    
    if (NULL == rc || NULL == rc2)
        return FALSE;

    /* Ok, this should be obvious, right? */
    if (rc == rc2)
        return TRUE;

    /* Records with the same SHA1 (if available) always match. Since the
	 * sha1 member is an atom, comparing the addresses is sufficient. */
    if (rc->sha1 != NULL && rc->sha1 == rc2->sha1)
        return TRUE;

    if (!search_autoselect)
        return FALSE;

    /* Check whether sizes match */;
    size_match = (search_autoselect_ident) ?
        (rc->size == rc2->size) :
        (rc2->size >= rc->size);
    
    if (!size_match)
        return FALSE;

	/* ``name'' is an atom */
	if (rc2->name == rc->name)
		return TRUE;

	if (!search_autoselect_fuzzy)
		return FALSE;

    /* Check whether the names match more or less */
    return fuzzy_compare(rc2->name, rc->name) * 100
				>= (fuzzy_threshold << FUZZY_SHIFT);
}


/***
 *** Callbacks
 ***/

/**
 * Called when the core has finished parsing the result set, and the results
 * need to be dispatched to the searches listed in `schl'.
 */
void
search_gui_got_results(GSList *schl, const gnet_results_set_t *r_set)
{
    results_set_t *rs;

    /*
     * Copy the data we got from the backend.
     */
    rs = search_gui_create_results_set(schl, r_set);

    if (gui_debug >= 12)
        printf("got incoming results...\n");

    g_assert(!g_slist_find(accumulated_rs, rs));

    accumulated_rs = g_slist_prepend(accumulated_rs, rs);
}

/**
 * Periodic timer to flush the accumulated hits during the period and
 * dispatch them to the GUI.
 */
void
search_gui_flush(time_t now)
{
    GSList *sl;
    GSList *curs;
    static time_t last = 0;
	guint32 period;
    GSList *frozen = NULL;

	gui_prop_get_guint32_val(PROP_SEARCH_ACCUMULATION_PERIOD, &period);
    if (now && (difftime(now, last) < period))
        return;

    last = now;

    if (accumulated_rs && (gui_debug >= 6)) {
        guint32 recs = 0;
        guint32 rscount = 0;

        for (sl = accumulated_rs; sl != NULL; sl = g_slist_next(sl)) {
            recs += ((results_set_t *)sl->data)->num_recs;
            rscount ++;
        }

        printf("flushing %d rsets (%d recs, %d recs avg)...\n", 
            rscount, recs, recs / rscount);
    }

    for (curs = accumulated_rs; curs != NULL; curs = g_slist_next(curs)) {
        results_set_t *rs = (results_set_t *) curs->data;
        GSList *schl = g_slist_copy(rs->schl);
 
        /*
         * Dispatch to all searches and freeze display where necessary
         * remembering what was frozen.
         */
        for (sl = schl; sl != NULL; sl = g_slist_next(sl)) {
            search_t *sch;

            sch = search_gui_find((gnet_search_t) GPOINTER_TO_UINT(sl->data));

            /*
             * Since we keep results around for a while, the search may have
             * been closed until they get dispatched... so we need to check 
             * that.
             *     --BLUE, 4/1/2004
             */

            if (sch) {
                search_gui_start_massive_update(sch);
                frozen = g_slist_prepend(frozen, sch);
                search_matched(sch, rs);
            } else if (gui_debug >= 6) printf(
				"no search for cached search result while dispatching\n");
        }

        /*
         * Some of the records might have not been used by searches, and need
         * to be freed.  If no more records remain, we request that the
         * result set be removed from all the dispatched searches, the last one
         * removing it will cause its destruction.
         */

        if (gui_debug >= 15)
            printf("cleaning phase\n");

        if (rs->refcount == 0) {
            search_gui_free_r_set(rs);
			g_slist_free(schl);
            continue;
        }

        search_gui_clean_r_set(rs);
    
        if (gui_debug >= 15)
            printf("trash phase\n");

        /*
         * If the record set does not contain any records after the cleansing,
         * we have only an empty shell left which we can safely remove from 
         * all the searches.
         */

        if (rs->num_recs == 0) {
            for (sl = schl; sl; sl = sl->next) {
                search_t *sch = search_gui_find(
                    (gnet_search_t) GPOINTER_TO_UINT(sl->data));

                /*
                 * Since we keep results around for a while, the search may 
                 * have been closed until they get dispatched... so we need to 
                 * check that.
                 * --BLUE, 4/1/2004
                 */

                if (sch)
                    search_gui_remove_r_set(sch, rs);
                else if (gui_debug >= 6) printf(
					"no search for cached search result while cleaning\n");
            }
        } 
		g_slist_free(schl);
    }

    /*
     * Unfreeze all we have frozen before.
     */
    for (sl = frozen; sl != NULL; sl = g_slist_next(sl)) {
		search_gui_end_massive_update((search_t *) sl->data);
    }
    g_slist_free(frozen);

    g_slist_free(accumulated_rs);
    accumulated_rs = NULL;
}

/**
 * Tries to extract the extenstion of a file from the filename.
 * The return value is only valid until the function is called again.
 */
gchar *
search_gui_extract_ext(gchar *filename) 
{
    static gchar ext[32];
	const gchar *p;
	size_t rw = 0;

    g_assert(NULL != filename);
    
    ext[0] = '\0';
    p = strrchr(filename, '.');

	rw = g_strlcpy(ext, p ? ++p : "", sizeof ext);
	if (rw >= sizeof ext) {
		/* If the guessed extension is really this long, assume the
		 * part after the dot isn't an extension at all. */
		ext[0] = '\0';
	} else {
		/* Using g_utf8_strdown() (for GTK2) would be cleaner but it 
         * allocates a new string which is ugly. Nobody uses such file 
         * extensions anyway. */
		ascii_strlower(ext, ext);
	}

    return ext;
}

/**
 * Creates a new search based on the filename found and adds a filter
 * to it based on the sha1 hash if it has one or the exact filename if
 * it hasn't.
 * 
 * @author Andrew Meredith <andrew@anvil.org>
 */
void
search_gui_add_targetted_search(record_t *rec, filter_t *noneed)
{
    search_t *new_search;
    rule_t *rule;

    g_assert(rec != NULL);
    g_assert(rec->name != NULL);

    /* create new search item with search string set to filename */
    search_gui_new_search(rec->name, 0, &new_search);
    g_assert(new_search != NULL);

    if (rec->sha1) {
        rule = filter_new_sha1_rule(rec->sha1, rec->name,
            filter_get_download_target(), RULE_FLAG_ACTIVE);
    } else {
        rule = filter_new_text_rule(rec->name, RULE_TEXT_EXACT, TRUE,
            filter_get_download_target(), RULE_FLAG_ACTIVE);
    }
    g_assert(rule != NULL);

    filter_append_rule(new_search->filter, rule);
}

/**
 * Restart a search from scratch, clearing all existing content.
 */
void
search_gui_restart_search(search_t *sch)
{
	if (!sch->enabled)
		gui_search_set_enabled(sch, TRUE);
	search_gui_reset_search(sch);	
	sch->items = sch->unseen_items = sch->hidden = 0;
	sch->tcp_qhits = sch->udp_qhits = 0;
	sch->skipped = sch->ignored = sch->auto_downloaded = sch->duplicates = 0;

	search_gui_update_items(sch);
	guc_search_update_items(sch->search_handle, sch->items);
	guc_search_reissue(sch->search_handle);	
}

/* vi: set ts=4 sw=4 cindent: */
