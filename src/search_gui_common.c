/*
 * $Id$
 *
 * Copyright (c) 2003, Raphael Manfredi
 *
 * Common GUI search routines.
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

#include "gui.h"
#include "gnet.h"

/* GUI includes  */
#include "search_gui_common.h"
#include "search_gui.h"

RCSID("$Id$");

static zone_t *rs_zone;		/* Allocation of results_set */
static zone_t *rc_zone;		/* Allocation of record */

/*
 * search_gui_free_alt_locs
 *
 * Free the alternate locations held within a file record.
 */
void search_gui_free_alt_locs(record_t *rc)
{
	alt_locs_t *alt = rc->alt_locs;

	g_assert(alt != NULL);

	wfree(alt->hvec, alt->hvcnt * sizeof(*alt->hvec));
	wfree(alt, sizeof(*alt));

	rc->alt_locs = NULL;
}

/*
 * search_gui_free_record
 *
 * Free one file record.
 *
 * Those records may be inserted into some `dups' tables, at which time they
 * have their refcount increased.  They may later be removed from those tables
 * and they will have their refcount decreased.
 *
 * To ensure some level of sanity, we ask our callers to explicitely check
 * for a refcount to be zero before calling us.
 */
void search_gui_free_record(record_t *rc)
{
	g_assert(rc->refcount == 0);

	atom_str_free(rc->name);
	if (rc->tag != NULL)
		atom_str_free(rc->tag);
	if (rc->sha1 != NULL)
		atom_sha1_free(rc->sha1);
	if (rc->alt_locs != NULL)
		search_gui_free_alt_locs(rc);
	zfree(rc_zone, rc);
}

/*
 * search_gui_clean_r_set
 *
 * This routine must be called when the results_set has been dispatched to
 * all the opened searches.
 *
 * All the records that have not been used by a search are removed.
 */
void search_gui_clean_r_set(results_set_t *rs)
{
	GSList *m;
    GSList *sl_remove = NULL;

	g_assert(rs->refcount);		/* If not dispatched, should be freed */

    /*
     * Collect empty searches.
     */
    for (m = rs->records; m != NULL; m = m->next) {
		record_t *rc = (record_t *) m->data;

		if (rc->refcount == 0)
			sl_remove = g_slist_prepend(sl_remove, (gpointer) rc);
    }

    /*
     * Remove empty searches from record set.
     */
	for (m = sl_remove; m != NULL; m = g_slist_next(m)) {
		record_t *rc = (record_t *) m->data;

		search_gui_free_record(rc);
		rs->records = g_slist_remove(rs->records, rc);
		rs->num_recs--;
	}

    g_slist_free(sl_remove);
}

/*
 * search_gui_free_r_set
 *
 * Free one results_set.
 *
 * Those records may be shared between several searches.  So while the refcount
 * is positive, we just decrement it and return without doing anything.
 */
void search_gui_free_r_set(results_set_t *rs)
{
	GSList *m;

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
	 * Because noone refers to us any more, we know that our embedded records
	 * cannot be held in the hash table anymore.  Hence we may call the
	 * search_free_record() safely, because rc->refcount must be zero.
	 */

	for (m = rs->records; m != NULL; m = m->next)
		search_gui_free_record((record_t *) m->data);

    if (rs->guid)
		atom_guid_free(rs->guid);
	if (rs->version)
		atom_str_free(rs->version);

	g_slist_free(rs->records);
	zfree(rs_zone, rs);
}

/*
 * search_gui_dispose_results
 *
 * Dispose of an empty search results, whose records have all been
 * unreferenced by the searches.  The results_set is therefore an
 * empty shell, useless.
 */
void search_gui_dispose_results(results_set_t *rs)
{
	gint refs = 0;
	GList *l;

	g_assert(rs->num_recs == 0);
	g_assert(rs->refcount > 0);

	/*
	 * A results_set does not point back to the searches that still
	 * reference it, so we have to do that manually.
	 */

	for (l = searches; l; l = l->next) {
		GSList *lnk;
		search_t *sch = (search_t *) l->data;

		lnk = g_slist_find(sch->r_sets, rs);
		if (lnk == NULL)
			continue;

		refs++;			/* Found one more reference to this search */

		sch->r_sets = g_slist_remove_link(sch->r_sets, lnk);
    
        /* FIXME: I have the strong impression that there is a memory leak
         *        here. We find the link and unlink it from r_sets, but
         *        then it does become a self-contained list and it is not
         *        freed anywhere, does it?
		 */
	}

	g_assert(rs->refcount == refs);		/* Found all the searches */

	rs->refcount = 1;
	search_gui_free_r_set(rs);
}

/*
 * search_gui_unref_record
 *
 * Remove one reference to a file record.
 *
 * If the record has no more references, remove it from its parent result
 * set and free the record physically.
 */
void search_gui_unref_record(struct record *rc)
{
	struct results_set *rs;

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

/* Free all the results_set's of a search */

void search_gui_free_r_sets(search_t *sch)
{
	GSList *l;

	g_assert(sch != NULL);
	g_assert(sch->dups != NULL);
	g_assert(g_hash_table_size(sch->dups) == 0); /* All records were cleaned */

	for (l = sch->r_sets; l; l = l->next)
		search_gui_free_r_set((results_set_t *) l->data);

	g_slist_free(sch->r_sets);
	sch->r_sets = NULL;
}

guint search_gui_hash_func(gconstpointer key)
{
	const struct record *rc = (const struct record *) key;
	/* Must use same fields as search_hash_key_compare() --RAM */
	return
		g_str_hash(rc->name) ^
		g_int_hash(&rc->size) ^
		g_int_hash(&rc->results_set->ip) ^
		g_int_hash(&rc->results_set->port) ^
		g_int_hash(&rc->results_set->guid[0]) ^
		g_int_hash(&rc->results_set->guid[4]) ^
		g_int_hash(&rc->results_set->guid[8]) ^
		g_int_hash(&rc->results_set->guid[12]);
}

gint search_gui_hash_key_compare(gconstpointer a, gconstpointer b)
{
	const struct record *rc1 = (const struct record *) a;
	const struct record *rc2 = (const struct record *) b;

	/* Must compare same fields as search_hash_func() --RAM */
	return rc1->size == rc2->size
		&& rc1->results_set->ip == rc2->results_set->ip
		&& rc1->results_set->port == rc2->results_set->port
		&& 0 == memcmp(rc1->results_set->guid, rc2->results_set->guid, 16)
		&& 0 == strcmp(rc1->name, rc2->name);
}

/*
 * search_gui_remove_r_set
 *
 * Remove reference to results in our search.
 * Last one to remove it will trigger a free.
 */
void search_gui_remove_r_set(search_t *sch, results_set_t *rs)
{
	sch->r_sets = g_slist_remove(sch->r_sets, rs);
	search_gui_free_r_set(rs);
}

/*
 * search_gui_result_is_dup
 *
 * Check to see whether we already have a record for this file.
 * If we do, make sure that the index is still accurate,
 * otherwise inform the interested parties about the change.
 *
 * Returns true if the record is a duplicate.
 */
gboolean search_gui_result_is_dup(search_t * sch, struct record * rc)
{
	union {
		struct record *rc;
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
		download_index_changed(
			rc->results_set->ip,		/* This is for optimizing lookups */
			rc->results_set->port,
			rc->results_set->guid,		/* This is for formal identification */
			old.rc->index,
			rc->index);
		old.rc->index = rc->index;
	}

	return TRUE;		/* yes, it's a duplicate */
}

/*
 * search_gui_find:
 *
 * Returns a pointer to gui_search_t from gui_searches which has
 * sh as search_handle. If none is found, return NULL.
 */
struct search *search_gui_find(gnet_search_t sh) 
{
    GList *l;
    
    for (l = searches; l != NULL; l = g_list_next(l)) {
        if (((search_t *)l->data)->search_handle == sh) {
            if (gui_debug >= 15)
                printf("search [%s] matched handle %x\n", (
                    (search_t *)l->data)->query, sh);

            return (struct search *) l->data;
        }
    }

    return NULL;
}

/*
 * search_gui_create_record
 *
 * Create a new GUI record within `rs' from a Gnutella record.
 */
record_t *search_gui_create_record(results_set_t *rs, gnet_record_t *r) 
{
    record_t *rc;

    g_assert(r != NULL);
    g_assert(rs != NULL);

    rc = (record_t *) zalloc(rc_zone);

    rc->results_set = rs;
    rc->refcount = 0;

    rc->name = atom_str_get(r->name);
    rc->size = r->size;
    rc->index = r->index;
    rc->sha1 = (r->sha1 != NULL) ? atom_sha1_get(r->sha1) : NULL;
    rc->tag = (r->tag != NULL) ? atom_str_get(r->tag) : NULL;
    rc->flags = r->flags;

	if (r->alt_locs != NULL) {
		gnet_alt_locs_t *a = r->alt_locs;
		alt_locs_t *alt = walloc(sizeof(*alt));
		gint hlen = a->hvcnt * sizeof(*a->hvec);

		alt->hvec = walloc(hlen);
		alt->hvcnt = a->hvcnt;
		memcpy(a->hvec, alt->hvec, hlen);

		rc->alt_locs = alt;
	}

    return rc;
}

/*
 * search_gui_create_results_set
 *
 * Create a new GUI result set from a Gnutella one.
 */
results_set_t *search_gui_create_results_set(const gnet_results_set_t *r_set)
{
    results_set_t *rs;
    GSList *sl;
    
    rs = (results_set_t *) zalloc(rs_zone);

    rs->refcount = 0;

    rs->guid = atom_guid_get(r_set->guid);
    rs->ip = r_set->ip;
    rs->port = r_set->port;
    rs->status = r_set->status;
    rs->speed = r_set->speed;
    rs->stamp = r_set->stamp;
    memcpy(rs->vendor, r_set->vendor, sizeof(rs->vendor));
	rs->version = r_set->version ? atom_str_get(r_set->version) : NULL;

    rs->num_recs = 0;
    rs->records = NULL;
    
    for (sl = r_set->records; sl != NULL; sl = g_slist_next(sl)) {
        record_t *rc = search_gui_create_record(rs, (gnet_record_t *) sl->data);

        rs->records = g_slist_prepend(rs->records, rc);
        rs->num_recs ++;
    }

    g_assert(rs->num_recs == r_set->num_recs);

    return rs;
}

/*
 * search_gui_common_init
 *
 * Initialize common structures.
 */
void search_gui_common_init(void)
{
	rs_zone = zget(sizeof(results_set_t), 1024);
	rc_zone = zget(sizeof(record_t), 1024);
}

/*
 * search_gui_common_shutdown
 *
 * Destroy common structures.
 */
void search_gui_common_shutdown(void)
{
	zdestroy(rs_zone);
	zdestroy(rc_zone);

	rs_zone = rc_zone = NULL;
}


// Remove older functions from search_gui.c and search_gui2.c
// Rename all calls in those files.
// Update the record structure to hold the alt_locs.
// Fix the download_new() calls to handle ALT
