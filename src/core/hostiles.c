/*
 * Copyright (c) 2004, Raphael Manfredi
 * Copyright (c) 2003, Markus Goetz
 *
 * This file is based a lot on the whitelist stuff by vidar.
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
 * Support for the hostiles.txt of BearShare
 */

#include "common.h"

RCSID("$Id$");

#include "hostiles.h"
#include "settings.h"
#include "nodes.h"

#include "lib/file.h"
#include "lib/misc.h"
#include "lib/glib-missing.h"
#include "lib/walloc.h"
#include "lib/watcher.h"

#include "if/gnet_property_priv.h"
#include "if/bridge/c2ui.h"

#include "lib/override.h"		/* Must be the last header included */

GSList *sl_hostiles = NULL;

static const gchar hostiles_file[] = "hostiles.txt";
static const gchar hostiles_what[] = "hostile IP addresses";

/*
 * Pre-sorted addresses to match against.
 */

static GSList *hostiles_exact[256];		/* Indexed by LAST byte */
static GSList *hostiles_wild = NULL;	/* Addresses with mask less than /8 */
static GSList *hostiles_narrow[256];	/* Indexed by FIRST byte */

/**
 * Hash an hostile structure.
 */
static guint
hostile_hash(gconstpointer key)
{
	const struct hostile *h = key;

	return (guint) (h->ip_masked ^ h->netmask);
}

/**
 * Check whether two hostile structures are equal.
 */
static gint
hostile_eq(gconstpointer a, gconstpointer b)
{
	const struct hostile *ha = a;
	const struct hostile *hb = b;

	return ha->ip_masked == hb->ip_masked && ha->netmask == hb->netmask;
}

/**
 * Load hostile data from the supplied FILE.
 * Returns the amount of entries loaded.
 */
static gint
hostiles_load(FILE *f)
{
	gchar line[1024];
	gchar *p;
	guint32 ip, netmask;
	struct hostile *n;
	int linenum = 0;
	gint count = 0;
	GHashTable *seen;

	seen = g_hash_table_new(hostile_hash, hostile_eq);

	while (fgets(line, sizeof(line), f)) {
		linenum++;
		if (*line == '\0' || *line == '#')
			continue;

		/*
		 * Remove all trailing spaces in string.
		 * Otherwise, lines which contain only spaces would cause a warning.
		 */
	
		p = strchr(line, '\0');	
		while (--p >= line) {
			guchar c = (guchar) *p;
			if (!is_ascii_space(c))
				break;
			*p = '\0';
		}
		if ('\0' == *line)
			continue;

		if (!gchar_to_ip_and_mask(line, &ip, &netmask)) {
			g_warning("hostiles_retrieve(): "
				"line %d: invalid IP or netmask\"%s\"", linenum, line);
			continue;
		}

		n = walloc0(sizeof(*n));
		n->ip_masked = ip & netmask;
		n->netmask = netmask;

		if (g_hash_table_lookup(seen, n)) {
			g_warning("hostiles_retrieve(): line %d: "
				"ignoring duplicate entry \"%s\" (%s/%s)",
				linenum, line, ip_to_gchar(ip), ip2_to_gchar(netmask));
			wfree(n, sizeof(*n));
			continue;
		}

		g_hash_table_insert(seen, n, n);
		sl_hostiles = g_slist_prepend(sl_hostiles, n);
		count++;
	}

	sl_hostiles = g_slist_reverse(sl_hostiles);
	g_hash_table_destroy(seen);		/* Keys/values are in `sl_hostiles' */

	if (dbg)
		g_message("loaded %d hostile IP addresses/netmasks\n", count);

	return count;
}

/**
 * Watcher callback, invoked when the file from which we read the hostile
 * addresses changed.
 */
static void
hostiles_changed(const gchar *filename, gpointer udata)
{
	FILE *f;
	gchar buf[80];
	gint count;

	f = file_fopen(filename, "r");
	if (f == NULL)
		return;

	hostiles_close();
	count = hostiles_load(f);

	gm_snprintf(buf, sizeof(buf), "Reloaded %d hostile IP addresses.", count);
	gcu_statusbar_message(buf);
}

/**
 * Loads the hostiles.txt into memory, choosing the first file we find
 * among the several places we look at, typically:
 *
 *    ~/.gtk-gnutella/hostiles.txt
 *    /usr/share/gtk-gnutella/hostiles.txt
 *    /home/src/gtk-gnutella/hostiles.txt
 *
 * The selected file will then be monitored and a reloading will occur
 * shortly after a modification.
 */
static void
hostiles_retrieve(void)
{
	FILE *f;
	gint idx;
	gchar *filename;
#ifndef OFFICIAL_BUILD 
	file_path_t fp[3];
#else
	file_path_t fp[2];
#endif

	file_path_set(&fp[0], settings_config_dir(), hostiles_file);
	file_path_set(&fp[1], PRIVLIB_EXP, hostiles_file);
#ifndef OFFICIAL_BUILD 
	file_path_set(&fp[2], PACKAGE_SOURCE_DIR, hostiles_file);
#endif

	f = file_config_open_read_norename_chosen(
			hostiles_what, fp, G_N_ELEMENTS(fp), &idx);

	if (!f)
	   return;

	filename = make_pathname(fp[idx].dir, fp[idx].name);
	watcher_register(filename, hostiles_changed, NULL);

	hostiles_load(f);
}

/**
 * Called on startup. Loads the hostiles.txt into memory.
 */
void
hostiles_init(void)
{
	GSList *sl;
	gint i;

	hostiles_retrieve();

	/*
	 * Pre-compile addresses so that we don't have to check too many rules
	 * each time to see if an address is part of the hostile set:
	 *
	 * The addresses whose mask is /32 are put in a special array, indexed by
	 * the LAST byte of the address: `hostiles_exact'.
	 *
	 * The addresses with /8 or less are put in a special list that is
	 * parsed in the second place: `hostiles_wild'.  There should not be
	 * much in there.
	 *
	 * All remaining addresses are places in an array, indexed by the FIRST byte
	 * of the address: `hostiles_narrow'.
	 */

	for (i = 0; i < 256; i++)
		hostiles_exact[i] = hostiles_narrow[i] = NULL;

	for (sl = sl_hostiles; sl; sl = g_slist_next(sl)) {
		struct hostile *h = (struct hostile *) sl->data;
		if (h->netmask == 0xffffffff) {
			i = h->ip_masked & 0x000000ff;
			hostiles_exact[i] = g_slist_prepend(hostiles_exact[i], h);
		} else if (h->netmask < 0xff000000)
			hostiles_wild = g_slist_prepend(hostiles_wild, h);
		else {
			i = (h->ip_masked & 0xff000000) >> 24;
			hostiles_narrow[i] = g_slist_prepend(hostiles_narrow[i], h);
		}
	}
}

/**
 * Frees all entries in the hostiles
 */
void
hostiles_close(void)
{
	GSList *sl;
	gint i;

	for (i = 0; i < 256; i++) {
		g_slist_free(hostiles_exact[i]);
		g_slist_free(hostiles_narrow[i]);
	}
	g_slist_free(hostiles_wild);
	hostiles_wild = NULL;

	for (sl = sl_hostiles; sl; sl = g_slist_next(sl)) 
		wfree(sl->data, sizeof(struct hostile));

	g_slist_free(sl_hostiles);
	sl_hostiles = NULL;
}

/**
 * Check the given IP agains the entries in the hostiles.
 * Returns TRUE if found, and FALSE if not.
 *
 */
gboolean
hostiles_check(guint32 ip)
{
	GSList *sl;
	struct hostile *h;
	gint i;

	/*
	 * Look for an exact match.
	 */

	i = ip & 0x000000ff;

	for (sl = hostiles_exact[i]; sl; sl = g_slist_next(sl)) {
		h = (struct hostile *) sl->data;
		if (ip == h->ip_masked)
			return TRUE;
	}

	/*
	 * Look for a wild match.
	 */

	for (sl = hostiles_wild; sl; sl = g_slist_next(sl)) {
		h = (struct hostile *) sl->data;
		if ((ip & h->netmask) == h->ip_masked)
			return TRUE;
	}

	/*
	 * Look for a narrow match.
	 */

	i = (ip & 0xff000000) >> 24;

	for (sl = hostiles_narrow[i]; sl; sl = g_slist_next(sl)) {
		h = (struct hostile *) sl->data;
		if ((ip & h->netmask) == h->ip_masked)
			return TRUE;
	}

	return FALSE;
}

/* vi: set ts=4: */
