/*
 * $Id$
 *
 * Copyright (c) 2001-2003, Raphael Manfredi
 * Copyright (c) 2000 Daniel Walker (dwalker@cats.ucsc.edu)
 *
 * Handle sharing of our own files.
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
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/uio.h>	/* For struct iovec */
#include <unistd.h>
#include <dirent.h>
#include <string.h>
#include <ctype.h>		/* toupper() */

#include "share.h"
#include "gmsg.h"
#include "huge.h"
#include "qrp.h"
#include "extensions.h"
#include "nodes.h"
#include "uploads.h"
#include "gnet_stats.h"
#include "settings.h"
#include "ggep.h"
#include "search.h"		/* For QUERY_SPEED_MARK */
#include "dmesh.h"		/* For dmesh_fill_alternate() */
#include "hostiles.h"

#include "ui_core_interface.h"
#include "override.h"		/* Must be the last header included */

RCSID("$Id$");

#define QHIT_SIZE_THRESHOLD	2016	/* Flush query hits larger than this */
#define QHIT_MAX_RESULTS	255		/* Maximum amount of hits in a query hit */
#define QHIT_MAX_ALT		5		/* Send out 5 alt-locs per entry, at most */
#define QHIT_MAX_PROXIES	5		/* Send out 5 push-proxies at most */

static const guchar iso_8859_1[96] = {
	' ', 			/* 160 - NO-BREAK SPACE */
	' ', 			/* 161 - INVERTED EXCLAMATION MARK */
	' ', 			/* 162 - CENT SIGN */
	' ', 			/* 163 - POUND SIGN */
	' ', 			/* 164 - CURRENCY SIGN */
	' ', 			/* 165 - YEN SIGN */
	' ', 			/* 166 - BROKEN BAR */
	' ', 			/* 167 - SECTION SIGN */
	' ', 			/* 168 - DIAERESIS */
	' ', 			/* 169 - COPYRIGHT SIGN */
	'a', 			/* 170 - FEMININE ORDINAL INDICATOR */
	' ', 			/* 171 - LEFT-POINTING DOUBLE ANGLE QUOTATION MARK */
	' ', 			/* 172 - NOT SIGN */
	' ', 			/* 173 - SOFT HYPHEN */
	' ', 			/* 174 - REGISTERED SIGN */
	' ', 			/* 175 - MACRON */
	' ', 			/* 176 - DEGREE SIGN */
	' ', 			/* 177 - PLUS-MINUS SIGN */
	'2', 			/* 178 - SUPERSCRIPT TWO */
	'3', 			/* 179 - SUPERSCRIPT THREE */
	' ', 			/* 180 - ACUTE ACCENT */
	'u', 			/* 181 - MICRO SIGN */
	' ', 			/* 182 - PILCROW SIGN */
	' ', 			/* 183 - MIDDLE DOT */
	' ', 			/* 184 - CEDILLA */
	'1', 			/* 185 - SUPERSCRIPT ONE */
	'o', 			/* 186 - MASCULINE ORDINAL INDICATOR */
	' ', 			/* 187 - RIGHT-POINTING DOUBLE ANGLE QUOTATION MARK */
	' ', 			/* 188 - VULGAR FRACTION ONE QUARTER */
	' ', 			/* 189 - VULGAR FRACTION ONE HALF */
	' ', 			/* 190 - VULGAR FRACTION THREE QUARTERS */
	' ', 			/* 191 - INVERTED QUESTION MARK */
	'a', 			/* 192 - LATIN CAPITAL LETTER A WITH GRAVE */
	'a', 			/* 193 - LATIN CAPITAL LETTER A WITH ACUTE */
	'a', 			/* 194 - LATIN CAPITAL LETTER A WITH CIRCUMFLEX */
	'a', 			/* 195 - LATIN CAPITAL LETTER A WITH TILDE */
	'a', 			/* 196 - LATIN CAPITAL LETTER A WITH DIAERESIS */
	'a', 			/* 197 - LATIN CAPITAL LETTER A WITH RING ABOVE */
	' ', 			/* 198 - LATIN CAPITAL LETTER AE */
	'c', 			/* 199 - LATIN CAPITAL LETTER C WITH CEDILLA */
	'e', 			/* 200 - LATIN CAPITAL LETTER E WITH GRAVE */
	'e', 			/* 201 - LATIN CAPITAL LETTER E WITH ACUTE */
	'e', 			/* 202 - LATIN CAPITAL LETTER E WITH CIRCUMFLEX */
	'e', 			/* 203 - LATIN CAPITAL LETTER E WITH DIAERESIS */
	'i', 			/* 204 - LATIN CAPITAL LETTER I WITH GRAVE */
	'i', 			/* 205 - LATIN CAPITAL LETTER I WITH ACUTE */
	'i',			/* 206 - LATIN CAPITAL LETTER I WITH CIRCUMFLEX */
	'i',			/* 207 - LATIN CAPITAL LETTER I WITH DIAERESIS */
	' ',			/* 208 - LATIN CAPITAL LETTER ETH */
	'n',			/* 209 - LATIN CAPITAL LETTER N WITH TILDE */
	'o',			/* 210 - LATIN CAPITAL LETTER O WITH GRAVE */
	'o',			/* 211 - LATIN CAPITAL LETTER O WITH ACUTE */
	'o',			/* 212 - LATIN CAPITAL LETTER O WITH CIRCUMFLEX */
	'o',			/* 213 - LATIN CAPITAL LETTER O WITH TILDE */
	'o',			/* 214 - LATIN CAPITAL LETTER O WITH DIAERESIS */
	' ',			/* 215 - MULTIPLICATION SIGN */
	'o',			/* 216 - LATIN CAPITAL LETTER O WITH STROKE */
	'u',			/* 217 - LATIN CAPITAL LETTER U WITH GRAVE */
	'u',			/* 218 - LATIN CAPITAL LETTER U WITH ACUTE */
	'u',			/* 219 - LATIN CAPITAL LETTER U WITH CIRCUMFLEX */
	'u',			/* 220 - LATIN CAPITAL LETTER U WITH DIAERESIS */
	'y',			/* 221 - LATIN CAPITAL LETTER Y WITH ACUTE */
	' ',			/* 222 - LATIN CAPITAL LETTER THORN */
	's',			/* 223 - LATIN SMALL LETTER SHARP S */
	'a',			/* 224 - LATIN SMALL LETTER A WITH GRAVE */
	'a',			/* 225 - LATIN SMALL LETTER A WITH ACUTE */
	'a',			/* 226 - LATIN SMALL LETTER A WITH CIRCUMFLEX */
	'a',			/* 227 - LATIN SMALL LETTER A WITH TILDE */
	'a',			/* 228 - LATIN SMALL LETTER A WITH DIAERESIS */
	'a',			/* 229 - LATIN SMALL LETTER A WITH RING ABOVE */
	' ',			/* 230 - LATIN SMALL LETTER AE */
	'c',			/* 231 - LATIN SMALL LETTER C WITH CEDILLA */
	'e',			/* 232 - LATIN SMALL LETTER E WITH GRAVE */
	'e',			/* 233 - LATIN SMALL LETTER E WITH ACUTE */
	'e',			/* 234 - LATIN SMALL LETTER E WITH CIRCUMFLEX */
	'e',			/* 235 - LATIN SMALL LETTER E WITH DIAERESIS */
	'i',			/* 236 - LATIN SMALL LETTER I WITH GRAVE */
	'i',			/* 237 - LATIN SMALL LETTER I WITH ACUTE */
	'i',			/* 238 - LATIN SMALL LETTER I WITH CIRCUMFLEX */
	'i',			/* 239 - LATIN SMALL LETTER I WITH DIAERESIS */
	' ',			/* 240 - LATIN SMALL LETTER ETH */
	'n',			/* 241 - LATIN SMALL LETTER N WITH TILDE */
	'o',			/* 242 - LATIN SMALL LETTER O WITH GRAVE */
	'o',			/* 243 - LATIN SMALL LETTER O WITH ACUTE */
	'o',			/* 244 - LATIN SMALL LETTER O WITH CIRCUMFLEX */
	'o',			/* 245 - LATIN SMALL LETTER O WITH TILDE */
	'o',			/* 246 - LATIN SMALL LETTER O WITH DIAERESIS */
	' ',			/* 247 - DIVISION SIGN */
	'o',			/* 248 - LATIN SMALL LETTER O WITH STROKE */
	'u',			/* 249 - LATIN SMALL LETTER U WITH GRAVE */
	'u',			/* 250 - LATIN SMALL LETTER U WITH ACUTE */
	'u',			/* 251 - LATIN SMALL LETTER U WITH CIRCUMFLEX */
	'u',			/* 252 - LATIN SMALL LETTER U WITH DIAERESIS */
	'y',			/* 253 - LATIN SMALL LETTER Y WITH ACUTE */
	' ',			/* 254 - LATIN SMALL LETTER THORN */
	'y',			/* 255 - LATIN SMALL LETTER Y WITH DIAERESIS */
};

static const guchar cp1252[30] = {

	' ', 			/* 130 - LOW-9 QUOTE */
	' ', 			/* 131 - */
	' ', 			/* 132 - LOW-9 DOUBLE QUOTE */
	' ', 			/* 133 - ELLIPSES */
	' ', 			/* 134 - DAGGER */
	' ', 			/* 135 - DOUBLE DAGGER */
	' ', 			/* 138 - */
	' ', 			/* 137 - PER MILLE SIGN */
	's', 			/* 138 - S WITH CARON */
	' ', 			/* 139 - LEFT-POINTING ANGLE */
	' ', 			/* 140 - */
	' ', 			/* 141 - */
	' ', 			/* 142 - */
	' ', 			/* 143 - */
	' ', 			/* 144 - */
	' ', 			/* 145 - LEFT SINGLE QUOTE */
	' ', 			/* 146 - RIGHT SINGLE QUOTE  */
	' ', 			/* 147 - LEFT DOUBLE QUOTE */
	' ', 			/* 148 - RIGHT DOUBLE QUOTE */
	' ', 			/* 149 - BULLET */
	' ', 			/* 150 - EN DASH */
	' ', 			/* 151 - EM DASH */
	' ', 			/* 152 - SMALL TILDE */
	't', /* tm */	/* 153 - TRADEMARK */
	's', 			/* 154 - s WITH CARON */
	' ', 			/* 155 - RIGHT-POINTING ANGLE */
	' ', 			/* 156 - */
	' ', 			/* 157 - */
	' ', 			/* 158 - */
	'y', 			/* 159 - Y DIAERESIS */
};

static const guchar macroman[126] = {

	' ', 			/* 130 - LOW-9 QUOTE */
	' ', 			/* 131 - */
	' ', 			/* 132 - LOW-9 DOUBLE QUOTE */
	' ', 			/* 133 - ELLIPSES */
	' ', 			/* 134 - DAGGER */
	' ', 			/* 135 - DOUBLE DAGGER */
	' ', 			/* 138 - */
	' ', 			/* 137 - PER MILLE SIGN */
	's', 			/* 138 - S WITH CARON */
	' ', 			/* 139 - LEFT-POINTING ANGLE */
	' ', 			/* 140 - */
	' ', 			/* 141 - */
	' ', 			/* 142 - */
	' ', 			/* 143 - */
	' ', 			/* 144 - */
	' ', 			/* 145 - LEFT SINGLE QUOTE */
	' ', 			/* 146 - RIGHT SINGLE QUOTE  */
	' ', 			/* 147 - LEFT DOUBLE QUOTE */
	' ', 			/* 148 - RIGHT DOUBLE QUOTE */
	' ', 			/* 149 - BULLET */
	' ', 			/* 150 - EN DASH */
	' ', 			/* 151 - EM DASH */
	' ', 			/* 152 - SMALL TILDE */
	't', /* tm */	/* 153 - TRADEMARK */
	's', 			/* 154 - s WITH CARON */
	' ', 			/* 155 - RIGHT-POINTING ANGLE */
	' ', 			/* 156 - */
	' ', 			/* 157 - */
	' ', 			/* 158 - */
	'y', 			/* 159 - Y DIAERESIS */
	' ', 			/* 160 - NO-BREAK SPACE */
	' ', 			/* 161 - DEGREE */
	' ', 			/* 162 - CENT SIGN */
	' ', 			/* 163 - POUND SIGN */
	' ', 			/* 164 - CURRENCY SIGN */
	' ', 			/* 165 - BULLET */
	' ', 			/* 166 - PARAGRAPH */
	' ', 			/* 167 - SECTION SIGN */
	' ', 			/* 168 - DIAERESIS */
	' ', 			/* 169 - COPYRIGHT SIGN */
	't', /* tm */	/* 170 - TRADEMARK */
	' ', 			/* 171 - LEFT-POINTING DOUBLE ANGLE QUOTATION MARK */
	' ', 			/* 172 - NOT SIGN */
	' ', 			/* 173 - NOT EQUAL */
	' ', 			/* 174 - REGISTERED SIGN */
	' ', 			/* 175 - MACRON */
	' ', 			/* 176 - INFINITY */
	' ', 			/* 177 - PLUS-MINUS SIGN */
	' ', 			/* 178 - LESSSOREQUAL */
	' ', 			/* 179 - GREATOREQUAL */
	' ', 			/* 180 - ACUTE ACCENT */
	'u', 			/* 181 - MICRO SIGN */
	' ', 			/* 182 - DERIVATIVE */
	' ', 			/* 183 - SIGMA */
	' ', 			/* 184 - CEDILLA */
	'1', 			/* 185 - SUPERSCRIPT ONE */
	' ', 			/* 186 - INTEGRAL */
	' ', 			/* 187 - RIGHT-POINTING DOUBLE ANGLE QUOTATION MARK */
	' ', 			/* 188 - VULGAR FRACTION ONE QUARTER */
	' ', 			/* 189 - VULGAR FRACTION ONE HALF */
	' ', 			/* 190 - VULGAR FRACTION THREE QUARTERS */
	' ', 			/* 191 - INVERTED QUESTION MARK */
	'a', 			/* 192 - LATIN CAPITAL LETTER A WITH GRAVE */
	'a', 			/* 193 - LATIN CAPITAL LETTER A WITH ACUTE */
	'a', 			/* 194 - LATIN CAPITAL LETTER A WITH CIRCUMFLEX */
	' ', 			/* 195 - SQUARE ROOT */
	'a', 			/* 196 - LATIN CAPITAL LETTER A WITH DIAERESIS */
	' ', 			/* 197 - WAVY EQUAL */
	' ', 			/* 198 - DELTA */
	'c', 			/* 199 - LATIN CAPITAL LETTER C WITH CEDILLA */
	'e', 			/* 200 - LATIN CAPITAL LETTER E WITH GRAVE */
	' ', 			/* 201 - ELLIPSES */
	'e', 			/* 202 - LATIN CAPITAL LETTER E WITH CIRCUMFLEX */
	'e', 			/* 203 - LATIN CAPITAL LETTER E WITH DIAERESIS */
	'i', 			/* 204 - LATIN CAPITAL LETTER I WITH GRAVE */
	'i', 			/* 205 - LATIN CAPITAL LETTER I WITH ACUTE */
	'i',			/* 206 - LATIN CAPITAL LETTER I WITH CIRCUMFLEX */
	'i',			/* 207 - LATIN CAPITAL LETTER I WITH DIAERESIS */
	' ',			/* 208 - EN DASH */
	' ',			/* 209 - EM DASH */
	' ',			/* 210 - LEFT DOUBLE QUOTE */
	' ',			/* 211 - RIGHT DOUBLE QUOTE */
	' ',			/* 212 - LEFT SINGLE QUOTE */
	' ',			/* 213 - RIGHT SINGLE QUOTE */
	'o',			/* 214 - LATIN CAPITAL LETTER O WITH DIAERESIS */
	' ',			/* 215 - DIAMOND */
	'o',			/* 216 - LATIN CAPITAL LETTER O WITH STROKE */
	'y',			/* 217 - Y DIAERESIS */
	' ',			/* 218 - DIVISION SLASH */
	'u',			/* 219 - LATIN CAPITAL LETTER U WITH CIRCUMFLEX */
	' ',			/* 220 - LEFT-POINTING ANGLE */
	' ',			/* 221 - RIGHT-POINTING ANGLE */
	' ',			/* 222 - LATIN CAPITAL LETTER THORN */
	's',			/* 223 - LATIN SMALL LETTER SHARP S */
	'a',			/* 224 - LATIN SMALL LETTER A WITH GRAVE */
	' ',			/* 225 - PERIOD CENTERED */
	' ',			/* 226 - LOW-9 QUOTE */
	' ',			/* 227 - LOW-9 DOUBLE QUOTE */
	' ',			/* 228 - PER MILLE SIGN */
	'a',			/* 229 - LATIN SMALL LETTER A WITH RING ABOVE */
	' ',			/* 230 - LATIN SMALL LETTER AE */
	'c',			/* 231 - LATIN SMALL LETTER C WITH CEDILLA */
	'e',			/* 232 - LATIN SMALL LETTER E WITH GRAVE */
	'e',			/* 233 - LATIN SMALL LETTER E WITH ACUTE */
	'e',			/* 234 - LATIN SMALL LETTER E WITH CIRCUMFLEX */
	'e',			/* 235 - LATIN SMALL LETTER E WITH DIAERESIS */
	'i',			/* 236 - LATIN SMALL LETTER I WITH GRAVE */
	'i',			/* 237 - LATIN SMALL LETTER I WITH ACUTE */
	'i',			/* 238 - LATIN SMALL LETTER I WITH CIRCUMFLEX */
	'i',			/* 239 - LATIN SMALL LETTER I WITH DIAERESIS */
	' ',			/* 240 - APPLE LOGO */
	'n',			/* 241 - LATIN SMALL LETTER N WITH TILDE */
	'o',			/* 242 - LATIN SMALL LETTER O WITH GRAVE */
	'o',			/* 243 - LATIN SMALL LETTER O WITH ACUTE */
	'o',			/* 244 - LATIN SMALL LETTER O WITH CIRCUMFLEX */
	'i',			/* 245 - DOTLESS i */
	'o',			/* 246 - LATIN SMALL LETTER O WITH DIAERESIS */
	' ',			/* 247 - SMALL TILDE */
	'o',			/* 248 - LATIN SMALL LETTER O WITH STROKE */
	' ',			/* 249 - SEMI-CIRCULAR ACCENT */
	'u',			/* 250 - LATIN SMALL LETTER U WITH ACUTE */
	'u',			/* 251 - LATIN SMALL LETTER U WITH CIRCUMFLEX */
	'u',			/* 252 - LATIN SMALL LETTER U WITH DIAERESIS */
	' ',			/* 253 - DOUBLE BACKTICK */
	' ',			/* 254 - CEDILLA */
	'y',			/* 255 - LATIN SMALL LETTER Y WITH DIAERESIS */
};


static guint64 files_scanned = 0;
static guint64 kbytes_scanned = 0;
static guint64 bytes_scanned = 0;

GSList *extensions = NULL;
GSList *shared_dirs = NULL;
static GSList *shared_files = NULL;
static struct shared_file **file_table = NULL;
static search_table_t search_table;
static GHashTable *file_basenames = NULL;

static gchar stmp_1[4096];

/***
 *** Callbacks
 ***/

static listeners_t search_request_listeners = NULL;

void share_add_search_request_listener(search_request_listener_t l)
{
    LISTENER_ADD(search_request, (gpointer) l);
}

void share_remove_search_request_listener(search_request_listener_t l)
{
    LISTENER_REMOVE(search_request, (gpointer) l);
}

static void share_emit_search_request(
    query_type_t type, const gchar *query, guint32 ip, guint16 port)
{
    LISTENER_EMIT(search_request, type, query, ip, port);
}

/*
 * Buffer where query hit packet is built.
 *
 * There is only one such packet, never freed.  At the beginning, one founds
 * the gnutella header, followed by the query hit header: initial offsetting
 * set by FOUND_RESET().
 *
 * The bufffer is logically (and possibly physically) extended via FOUND_GROW()
 * FOUND_BUF and FOUND_SIZE are used within the building code to access the
 * beginning of the query hit packet and the logical size of the packet.
 *
 *		--RAM, 25/09/2001
 */

struct {
	guchar *d;					/* data */
	guint32 l;					/* data length */
	guint32 s;					/* size used by current search hit */
	guint files;				/* amount of file entries */
} found_data;

#define FOUND_CHUNK		1024	/* Minimal growing memory amount unit */

#define FOUND_GROW(len) do {						\
	gint missing;									\
	found_data.s += (len);							\
	missing = found_data.s - found_data.l;			\
	if (missing > 0) {								\
		missing = MAX(missing, FOUND_CHUNK);		\
		found_data.l += missing;					\
		found_data.d = (guchar *) g_realloc(found_data.d,	\
			found_data.l * sizeof(guchar));			\
	}												\
} while (0)

#define FOUND_RESET() do {							\
	found_data.s = sizeof(struct gnutella_header) +	\
		sizeof(struct gnutella_search_results_out);	\
	found_data.files = 0;							\
} while (0)

#define FOUND_BUF	found_data.d
#define FOUND_SIZE	found_data.s
#define FOUND_FILES	found_data.files

#define FOUND_LEFT(x)	(found_data.l - (x))

static gboolean use_ggep_h;			/* Can we use GGEP "H" for this query? */
static time_t release_date;

/* 
 * We don't want to include the same file several times in a reply (for
 * example, once because it matches an URN query and once because the file name
 * matches). So we keep track of what has been added in this hash table.
 * The file index is used as the key.
 */

static GHashTable *index_of_found_files = NULL;
static struct gnutella_node *issuing_node;

/* 
 * shared_file_already_in_found_set
 * 
 * Check if a given shared_file has been added to the QueryHit.
 * Return TRUE if the shared_file is in the QueryHit already, FALSE otherwise
 */
static gboolean shared_file_already_in_found_set(const struct shared_file *sf)
{
	return NULL != g_hash_table_lookup(index_of_found_files,
		GUINT_TO_POINTER(sf->file_index));
}

/*
 * put_shared_file_into_found_set
 * 
 * Add the shared_file to the set of files already added to the QueryHit.
 */

static void put_shared_file_into_found_set(const struct shared_file *sf)
{
	g_hash_table_insert(index_of_found_files, 
				  GUINT_TO_POINTER(sf->file_index), 
				  GUINT_TO_POINTER(!NULL));
}

/* 
 * found_reset
 * 
 * Reset the QueryHit, that is, the "data found" pointer is at the beginning of
 * the data found section in the query hit packet and the index_of_found_files
 * GTree is reset.
 */
static void found_reset(struct gnutella_node *n)
{
	FOUND_RESET();
	issuing_node = n;

	/*
	 * We only destroy and recreate a new hash table if we inserted something
	 * in the previous search.
	 */

	if (index_of_found_files && g_hash_table_size(index_of_found_files) > 0) {
		g_hash_table_destroy(index_of_found_files);
		index_of_found_files = NULL;
	}

	if (index_of_found_files == NULL)
		index_of_found_files = g_hash_table_new(NULL, NULL);
}

/*
 * Minimal trailer length is our code NAME, the open flags, and the GUID.
 */
#define QHIT_MIN_TRAILER_LEN	(4+3+16)	/* NAME + open flags + GUID */

#define FILENAME_CLASH 0xffffffff			/* Indicates basename clashes */



/* ----------------------------------------- */

static char_map_t query_map;
static gboolean b_latin = FALSE;

/*
 * setup_char_map
 *
 * Set up keymapping table for Gnutella.
 */
static void setup_char_map(char_map_t map)
{
	gint c;	
	gboolean b_ascii = FALSE;
	gboolean b_iso_8859_1 = FALSE;
	gboolean b_cp1252 = FALSE;
	gboolean b_macroman = FALSE;
	const gchar *charset = locale_get_charset();

	if (0 == strcmp(charset, "ASCII")) {
		b_ascii = TRUE;
		b_latin = TRUE;
	} else if (
		0 == strcmp(charset, "ISO-8859-1") ||
		0 == strcmp(charset, "ISO-8859-15")
	) {
		b_iso_8859_1 = TRUE;
		b_latin = TRUE;
	} else if (0 == strcmp(charset, "CP1252")) {
		b_cp1252 = TRUE;
		b_latin = TRUE;
	} else if (0 == strcmp(charset, "MacRoman")) {
		b_macroman = TRUE;
		b_latin = TRUE;
	} else if (
		0 == strcmp(charset, "CP437") ||
		0 == strcmp(charset, "CP775") ||
		0 == strcmp(charset, "CP850") ||
		0 == strcmp(charset, "CP852") ||
		0 == strcmp(charset, "CP865") ||
		0 == strcmp(charset, "HP-ROMAN8") ||
		0 == strcmp(charset, "ISO-8859-2") ||
		0 == strcmp(charset, "ISO-8859-4") ||
		0 == strcmp(charset, "ISO-8859-14")
	)
		b_latin = TRUE;

	for (c = 0; c < 256; c++)	{
		if (!isupper(c)) {  /* not same than islower, cf ssharp */
			map[c] = tolower(toupper(c)); /* not same than c, cf ssharp */
			map[toupper(c)] = c;
		}
		else if (isupper(c))
			; /* handled by previous case */
		else if (ispunct(c) || isspace(c))
			map[c] = ' ';
		else if (isdigit(c))
			map[c] = c;
		else if (isalnum(c))
			map[c] = c;
		else
			map[c] = ' ';			/* unknown in our locale */
	}

	if (b_latin) {
		if (b_iso_8859_1 || b_cp1252) {
			for (c = 160; c < 256; c++)
				map[c] = iso_8859_1[c - 160];
		}
		if (b_cp1252) {
			for (c = 130; c < 160; c++)
				map[c] = cp1252[c - 130];
		} else if (b_macroman) {
			for (c = 130; c < 256; c++)
				map[c] = macroman[c - 130];
		}
	}
}

/*
 * use_map_on_query
 *
 * Apply the proper charset mapping on the query, depending on their
 * locale, so that the query has no accent.
 */
void use_map_on_query(gchar *query, int len)
{
	query += len - 1;
	for (/* empty */; len > 0; len--) {
		*query = query_map[(guchar) *query];
		query--;
	}
}

/* ----------------------------------------- */

void share_init(void)
{
	setup_char_map(query_map);
	huge_init();
	st_initialize(&search_table, query_map);
	qrp_init(query_map);

	found_data.l = FOUND_CHUNK;		/* must be > size after found_reset */
	found_data.d = (guchar *) g_malloc(found_data.l * sizeof(guchar));

	release_date = date2time(GTA_RELEASE, NULL);

	/*
	 * We allocate an empty search_table, which will be de-allocated when we
	 * call share_scan().  Why do we do this?  Because it ensures the table
	 * is correctly setup empty, until we do call share_scan() for the first
	 * time (the call is delayed until the GUI is up).
	 *
	 * Since we will start processing network packets, we will have a race
	 * condition window if we get a Query message before having started
	 * the share_scan().  Creating the table right now prevents adding an
	 * extra test at the top of st_search().
	 *		--RAM, 15/08/2002.
	 */

	st_create(&search_table);
}

/*
 * shared_file
 *
 * Given a valid index, returns the `struct shared_file' entry describing
 * the shared file bearing that index if found, NULL if not found (invalid
 * index) and SHARE_REBUILDING when we're rebuilding the library.
 */
struct shared_file *shared_file(guint idx)
{
	/* Return shared file info for index `idx', or NULL if none */

	if (file_table == NULL)			/* Rebuilding the library! */
		return SHARE_REBUILDING;

	if (idx < 1 || idx > files_scanned)
		return NULL;

	return file_table[idx - 1];
}

/*
 * shared_file_by_name
 *
 * Given a file basename, returns the `struct shared_file' entry describing
 * the shared file bearing that basename, provided it is unique, NULL if
 * we either don't have a unique filename or SHARE_REBUILDING if the library
 * is being rebuilt.
 */
struct shared_file *shared_file_by_name(const gchar *basename)
{
	guint idx;

	if (file_table == NULL)
		return SHARE_REBUILDING;

	g_assert(file_basenames);

	idx = GPOINTER_TO_UINT(g_hash_table_lookup(file_basenames, basename));

	if (idx == 0 || idx == FILENAME_CLASH)
		return NULL;

	g_assert(idx >= 1 && idx <= files_scanned);

	return file_table[idx - 1];
}

/* ----------------------------------------- */

/* Free existing extensions */

static void free_extensions(void)
{
	GSList *sl = extensions;

	if (!sl)
		return;

	for ( /*empty */ ; sl; sl = g_slist_next(sl)) {
		struct extension *e = (struct extension *) sl->data;
		atom_str_free(e->str);
		g_free(e);
	}
	g_slist_free(extensions);
	extensions = NULL;
}

/* Get the file extensions to scan */

void parse_extensions(const gchar * str)
{
	gchar **exts = g_strsplit(str, ";", 0);
	gchar *x, *s;
	guint i, e;

	free_extensions();

	e = i = 0;

	while (exts[i]) {
		s = exts[i];
		while (*s == ' ' || *s == '\t' || *s == '.' || *s == '*'
			   || *s == '?')
			s++;
		if (*s) {
			x = s + strlen(s);
			while (--x > s
				   && (*x == ' ' || *x == '\t' || *x == '*' || *x == '?'))
				*x = 0;
			if (*s) {
				struct extension *e = (struct extension *) g_malloc(sizeof(*e));
				e->str = atom_str_get(s);
				e->len = strlen(s);
				extensions = g_slist_prepend(extensions, e);
			}
		}
		i++;
	}

	extensions = g_slist_reverse(extensions);
	g_strfreev(exts);
}

/* Shared dirs */

static void shared_dirs_free(void)
{
	GSList *sl;

	if (!shared_dirs)
		return;

	for (sl = shared_dirs; sl; sl = g_slist_next(sl)) {
		atom_str_free(sl->data);
	}
	g_slist_free(shared_dirs);
	shared_dirs = NULL;
}

void shared_dirs_update_prop(void)
{
    GSList *sl;
    GString *s;

    s = g_string_new("");

    for (sl = shared_dirs; sl != NULL; sl = g_slist_next(sl)) {
        g_string_append(s, sl->data);
        if (g_slist_next(sl) != NULL)
            g_string_append(s, ":");
    }

    gnet_prop_set_string(PROP_SHARED_DIRS_PATHS, s->str);

    g_string_free(s, TRUE);
}

/*
 * shared_dirs_parse:
 *
 * Parses the given string and updated the internal list of shared dirs.
 * The given string was completely parsed, it returns TRUE, otherwise
 * it returns FALSE.
 */
gboolean shared_dirs_parse(const gchar *str)
{
	gchar **dirs = g_strsplit(str, ":", 0);
	guint i = 0;
    gboolean ret = TRUE;

	shared_dirs_free();

	while (dirs[i]) {
		if (is_directory(dirs[i]))
			shared_dirs = g_slist_prepend(shared_dirs, atom_str_get(dirs[i]));
        else 
            ret = FALSE;
		i++;
	}

	shared_dirs = g_slist_reverse(shared_dirs);
	g_strfreev(dirs);

    return ret;
}

void shared_dir_add(const gchar * path)
{
	if (is_directory(path))
        shared_dirs = g_slist_append(shared_dirs, atom_str_get(path));

    shared_dirs_update_prop();
}

static inline gboolean too_big_for_gnutella(off_t size)
{
	g_return_val_if_fail(size >= 0, TRUE);
	if (sizeof(off_t) <= sizeof(guint32))
		return FALSE;
	return (guint64) size > (guint64) 0xffffffffUL;
}

/*
 * recurse_scan
 *
 * The directories that are given as shared will be completly transversed
 * including all files and directories. An entry of "/" would search the
 * the whole file system.
 */
static void recurse_scan(gchar *dir, const gchar *basedir)
{
	GSList *exts = NULL;
	DIR *directory;			/* Dir stream used by opendir, readdir etc.. */
	struct dirent *dir_entry;
	gchar *full = NULL;
	GSList *files = NULL;
	GSList *directories = NULL;
	gchar *dir_slash = NULL;
	GSList *sl;
	gint i;
	struct stat file_stat;
	const gchar *entry_end;

	if (*dir == '\0')
		return;

	if (!(directory = opendir(dir))) {
		g_warning("can't open directory %s: %s", dir, g_strerror(errno));
		return;
	}
	
	if (dir[strlen(dir) - 1] == G_DIR_SEPARATOR)
		dir_slash = dir;
	else
		dir_slash = g_strconcat(dir, G_DIR_SEPARATOR_S, NULL);

	while ((dir_entry = readdir(directory))) {

		if (dir_entry->d_name[0] == '.')	/* Hidden file, or "." or ".." */
			continue;

		full = g_strconcat(dir_slash, dir_entry->d_name, NULL);

		if (!is_directory(full)) {
			if (scan_ignore_symlink_regfiles && is_symlink(full)) {
				G_FREE_NULL(full);
				continue;
			}
			files = g_slist_prepend(files, full);
		} else {
			if (scan_ignore_symlink_dirs && is_symlink(full)) {
				G_FREE_NULL(full);
				continue;
			}
			directories = g_slist_prepend(directories, full);
		}
	}

	for (i = 0, sl = files; sl; i++, sl = g_slist_next(sl)) {
		const gchar *name;
		gint name_len;

		full = (gchar *) sl->data;

		/*
		 * In the "tmp" directory, don't share files that have a trailer.
		 * It's probably a file being downloaded, and which is not complete yet.
		 * This check is necessary in case they choose to share their
		 * downloading directory...
		 */

		name = strrchr(full, G_DIR_SEPARATOR);
		g_assert(name);
		name++;						/* Start of file name */

		name_len = strlen(name);
		entry_end = name + name_len;

		for (exts = extensions; exts; exts = exts->next) {
			struct extension *e = (struct extension *) exts->data;
			const gchar *start = entry_end - (e->len + 1);	/* +1 for "." */

			/*
			 * Look for the trailing chars (we're matching an extension).
			 * Matching is case-insensitive, and the extension opener is ".".
			 *
			 * An extension "--all--" matches all files, even if they
			 * don't have any extension. [Patch from Zygo Blaxell].
			 */

			if (
				0 == g_ascii_strcasecmp("--all--", e->str) ||	/* All files */
				(start >= name && *start == '.' &&
					0 == g_ascii_strcasecmp(start+1, e->str))
			) {
				struct shared_file *found = NULL;

				if (dbg > 5)
					g_message("%s: full=\"%s\"", __func__, full);

				if (stat(full, &file_stat) == -1) {
					g_warning("can't stat %s: %s", full, g_strerror(errno));
					break;
				}

				if (0 == file_stat.st_size) {
					if (dbg > 5)
						g_warning("Not sharing empty file: \"%s\"", full);
					break;
				}
					
				if (too_big_for_gnutella(file_stat.st_size)) {
					g_warning("File is too big to be shared: \"%s\"", full);
					break;
				}

				found = (struct shared_file *) walloc0(sizeof(*found));

				found->file_path = atom_str_get(full);
				found->file_name = found->file_path + (name - full);
				found->file_name_len = name_len;
				found->file_size = file_stat.st_size;
				found->file_index = ++files_scanned;
				found->mtime = file_stat.st_mtime;
				found->flags = 0;

				if (!sha1_is_cached(found) && file_info_has_trailer(full)) {
					/*
	 		 	 	 * It's probably a file being downloaded, and which is
					 * not complete yet. This check is necessary in case
					 * they choose to share their downloading directory...
	 		  	 	 */

					g_warning("will not share partial file \"%s\"", full);
					shared_file_free(found);
					found = NULL;
					break;
				}

				request_sha1(found);
				st_insert_item(&search_table, found->file_name, found);
				shared_files = g_slist_prepend(shared_files, found);

				bytes_scanned += file_stat.st_size;
				kbytes_scanned += bytes_scanned >> 10;
				bytes_scanned &= (1 << 10) - 1;
				break;			/* for loop */
			}
		}
		G_FREE_NULL(full);

		if (!(i & 0x3f)) {
			gcu_gui_update_files_scanned();	/* Interim view */
			gcu_gtk_main_flush();
		}
	}

	closedir(directory);
	directory = NULL;
	g_slist_free(files);
	files = NULL;

	/*
	 * Now that we handled files at this level and freed all their memory,
	 * recurse on directories.
	 */

	for (sl = directories; sl; sl = g_slist_next(sl)) {
		gchar *path = (gchar *) sl->data;
		recurse_scan(path, basedir);
		G_FREE_NULL(path);
	}
	g_slist_free(directories);

	if (dir_slash != dir)
		G_FREE_NULL(dir_slash);

	gcu_gui_update_files_scanned();		/* Interim view */
	gcu_gtk_main_flush();
}

/*
 * shared_file_free
 *
 * Dispose of a shared_file_t structure.
 */
void shared_file_free(shared_file_t *sf)
{
	g_assert(sf != NULL);

	atom_str_free(sf->file_path);
	wfree(sf, sizeof(*sf));
}

static void share_free(void)
{
	GSList *sl;

	st_destroy(&search_table);

	if (file_basenames) {
		g_hash_table_destroy(file_basenames);
		file_basenames = NULL;
	}

	if (file_table)
		G_FREE_NULL(file_table);

	for (sl = shared_files; sl; sl = g_slist_next(sl)) {
		struct shared_file *sf = sl->data;
		shared_file_free(sf);
	}

	g_slist_free(shared_files);
	shared_files = NULL;
}

static void reinit_sha1_table(void);

void share_scan(void)
{
	GSList *dirs;
	GSList *sl;
	gint i;
	static gboolean in_share_scan = FALSE;
	time_t now;
	guint32 elapsed;

	/*
	 * We normally disable the "Rescan" button, so we should not enter here
	 * twice.  Nonetheless, the events can be stacked, and since we call
	 * the main loop whilst scanning, we could re-enter here.
	 *
	 *		--RAM, 05/06/2002 (added after the above indeed happened)
	 */

	if (in_share_scan)
		return;
	else
		in_share_scan = TRUE;

	now = time(NULL);
	elapsed = delta_time(now, (time_t) 0);

	gnet_prop_set_boolean_val(PROP_LIBRARY_REBUILDING, TRUE);
	gnet_prop_set_guint32_val(PROP_LIBRARY_RESCAN_TIMESTAMP, elapsed);

	files_scanned = 0;
	bytes_scanned = 0;
	kbytes_scanned = 0;

	reinit_sha1_table();
	share_free();

	g_assert(file_basenames == NULL);

	st_create(&search_table);
	file_basenames = g_hash_table_new(g_str_hash, g_str_equal);

	/*
	 * Clone the `shared_dirs' list so that we don't behave strangely
	 * should they update the list of shared directories in the GUI
	 * whilst we're recursing!
	 *		--RAM, 30/01/2003
	 */

	for (dirs = NULL, sl = shared_dirs; sl; sl = g_slist_next(sl))
		dirs = g_slist_prepend(dirs, atom_str_get(sl->data));

	dirs = g_slist_reverse(dirs);

	/* Recurse on the cloned list... */
	for (sl = dirs; sl; sl = g_slist_next(sl))
		recurse_scan(sl->data, sl->data);	/* ...since this updates the GUI! */

	for (sl = dirs; sl; sl = g_slist_next(sl))
		atom_str_free(sl->data);

	g_slist_free(dirs);
	dirs = NULL;

	/*
	 * Done scanning all the files.
	 */

	st_compact(&search_table);

	/*
	 * In order to quickly locate files based on indicies, build a table
	 * of all shared files.  This table is only accessible via shared_file().
	 * NB: file indicies start at 1, but indexing in table start at 0.
	 *		--RAM, 08/10/2001
	 *
	 * We over-allocate the file_table by one entry so that even when they
	 * don't share anything, the `file_table' pointer is not NULL.
	 * This will prevent us giving back "rebuilding library" when we should
	 * actually return "not found" for user download requests.
	 *		--RAM, 23/10/2002
	 */

	file_table = g_malloc0((files_scanned + 1) * sizeof(*file_table[0]));

	for (i = 0, sl = shared_files; sl; i++, sl = g_slist_next(sl)) {
		struct shared_file *sf = sl->data;
		guint val;

		g_assert(sf->file_index > 0 && sf->file_index <= files_scanned);
		file_table[sf->file_index - 1] = sf;

		/*
		 * In order to transparently handle files requested with the wrong
		 * indices, for older servents that would not know how to handle a
		 * return code of "301 Moved" with a Location header, we keep track
		 * of individual basenames of files, recording the index of each file.
		 * As soon as there is a clash, we revoke the entry by storing
		 * FILENAME_CLASH instead, which cannot be a valid index.
		 *		--RAM, 06/06/2002
		 */

		val = GPOINTER_TO_UINT(
			g_hash_table_lookup(file_basenames, sf->file_name));

		/*
		 * The following works because 0 cannot be a valid file index.
		 */

		val = (val != 0) ? FILENAME_CLASH : sf->file_index;
		g_hash_table_insert(file_basenames, (gchar *) sf->file_name,
			GUINT_TO_POINTER(val));

		if (0 == (i & 0x7ff))
			gcu_gtk_main_flush();
	}

	gcu_gui_update_files_scanned();		/* Final view */

	now = time(NULL);
	elapsed = delta_time(now, (time_t) 0) - library_rescan_timestamp;
	gnet_prop_set_guint32_val(PROP_LIBRARY_RESCAN_TIME, MAX(elapsed, 1));

	/*
	 * Query routing table update.
	 */

	gnet_prop_set_guint32_val(PROP_QRP_INDEXING_TIMESTAMP, (guint32) now);

	qrp_prepare_computation();

	for (i = 0, sl = shared_files; sl; i++, sl = g_slist_next(sl)) {
		struct shared_file *sf = sl->data;
		qrp_add_file(sf);
		if (0 == (i & 0x7ff))
			gcu_gtk_main_flush();
	}

	qrp_finalize_computation();

	now = time(NULL);
	elapsed = delta_time(now, (time_t) 0) - qrp_indexing_timestamp;
	gnet_prop_set_guint32_val(PROP_QRP_INDEXING_TIME, elapsed);

	in_share_scan = FALSE;
	gnet_prop_set_boolean_val(PROP_LIBRARY_REBUILDING, FALSE);
}

void share_close(void)
{
	G_FREE_NULL(found_data.d);
	free_extensions();
	share_free();
	shared_dirs_free();
	huge_close();
	qrp_close();
}

/*
 * flush_match
 *
 * Flush pending search request to the network.
 */
static void flush_match(void)
{
	struct gnutella_node *n = issuing_node;		/* XXX -- global! */
	gchar trailer[10];
	guint32 pos, pl;
	struct gnutella_header *packet_head;
	struct gnutella_search_results_out *search_head;
	gchar version[24];
	gchar push_proxies[40];
	gchar hostname[256];
	gchar *last_ggep = NULL;
	gint version_size = 0;		/* Size of emitted GGEP version */
	gint proxies_size = 0;		/* Size of emitted GGEP proxies */
	gint hostname_size = 0;		/* Size of emitted GGEP hostname */
	guint32 connect_speed;		/* Connection speed, in kbits/s */

	if (dbg > 3)
		printf("flushing query hit (%d entr%s, %d bytes sofar)\n",
			FOUND_FILES, FOUND_FILES == 1 ? "y" : "ies", FOUND_SIZE);

	/*
	 * Build Gtk-gnutella trailer.
	 * It is compatible with BearShare's one in the "open data" section.
	 */

	memcpy(trailer, "GTKG", 4);	/* Vendor code */
	trailer[4] = 2;					/* Open data size */
	trailer[5] = 0x04 | 0x08 | 0x20;	/* Valid flags we set */
	trailer[6] = 0x01;				/* Our flags (valid firewall bit) */

	if (ul_running >= max_uploads)
		trailer[6] |= 0x04;			/* Busy flag */
	if (count_uploads > 0)
		trailer[6] |= 0x08;			/* One file uploaded, at least */
	if (is_firewalled)
		trailer[5] |= 0x01;			/* Firewall bit set in enabling byte */

	/*
	 * Build the "GTKGV1" GGEP extension.
	 */

	{
		guint8 major = GTA_VERSION;
		guint8 minor = GTA_SUBVERSION;
		gchar *revp = GTA_REVCHAR;
		guint8 revchar = (guint8) revp[0];
		guint8 patch;
		guint32 release;
		guint32 date = release_date;
		guint32 start;
		struct iovec iov[6];
		gint w;

#ifdef GTA_PATCHLEVEL
		patch = GTA_PATCHLEVEL;
#else
		patch = 0;
#endif

		WRITE_GUINT32_BE(date, &release);
		WRITE_GUINT32_BE(start_stamp, &start);

		iov[0].iov_base = (gpointer) &major;
		iov[0].iov_len = 1;

		iov[1].iov_base = (gpointer) &minor;
		iov[1].iov_len = 1;

		iov[2].iov_base = (gpointer) &patch;
		iov[2].iov_len = 1;

		iov[3].iov_base = (gpointer) &revchar;
		iov[3].iov_len = 1;

		iov[4].iov_base = (gpointer) &release;
		iov[4].iov_len = 4;

		iov[5].iov_base = (gpointer) &start;
		iov[5].iov_len = 4;

		w = ggep_ext_writev(version, sizeof(version),
				"GTKGV1", iov, G_N_ELEMENTS(iov),
				GGEP_W_FIRST);

		if (w == -1)
			g_warning("could not write GGEP \"GTKGV1\" extension in query hit");
		else {
			trailer[6] |= 0x20;			/* Has GGEP extensions in trailer */
			version_size = w;
			last_ggep = version + 1;	/* Skip leading magic byte */
		}
	}

	/*
	 * Look whether we'll need a "PUSH" GGEP extension to give out
	 * our current push proxies.  Prepare payload in `proxies'.
	 */

	if (is_firewalled) {
		GSList *nodes = node_push_proxies();

		if (nodes != NULL) {
			GSList *l;
			gint count;
			gchar *p;
			gchar proxies[6 * QHIT_MAX_PROXIES];
			gint proxies_len;	/* Length of the filled `proxies' buffer */
			struct iovec iov[1];
			gint w;

			for (
				l = nodes, count = 0, p = proxies;
				l && count < QHIT_MAX_PROXIES;
				l = g_slist_next(l), count++
			) {
				struct gnutella_node *n = (struct gnutella_node *) l->data;
				
				WRITE_GUINT32_BE(n->proxy_ip, p);
				p += 4;
				WRITE_GUINT16_LE(n->proxy_port, p);
				p += 2;
			}

			proxies_len = p - proxies;

			g_assert(proxies_len % 6 == 0);

			iov[0].iov_base = (gpointer) proxies;
			iov[0].iov_len = proxies_len;

			w = ggep_ext_writev(push_proxies, sizeof(push_proxies),
					"PUSH", iov, G_N_ELEMENTS(iov),
					last_ggep == NULL ? GGEP_W_FIRST : 0);

			if (w == -1)
				g_warning("could not write GGEP \"PUSH\" extension "
					"in query hit");
			else {
				trailer[6] |= 0x20;			/* Has GGEP extensions in trailer */
				proxies_size = w;
				last_ggep = push_proxies + ((last_ggep == NULL) ? 1 : 0);
			}
		}
	}

	/*
	 * Look whether we can include an HNAME extension advertising the
	 * server's hostname.
	 */

	if (!is_firewalled && give_server_hostname && 0 != *server_hostname) {
		struct iovec iov[1];
		gint w;

		iov[0].iov_base = (gpointer) server_hostname;
		iov[0].iov_len = strlen(server_hostname);

		w = ggep_ext_writev(hostname, sizeof(hostname),
				"HNAME", iov, G_N_ELEMENTS(iov),
				last_ggep == NULL ? GGEP_W_FIRST : 0);

		if (w == -1)
			g_warning("could not write GGEP \"HNAME\" extension "
				"in query hit");
		else {
			trailer[6] |= 0x20;			/* Has GGEP extensions in trailer */
			hostname_size = w;
			last_ggep = hostname + ((last_ggep == NULL) ? 1 : 0);
		}
	}

	if (last_ggep != NULL)
		ggep_ext_mark_last(last_ggep);

	pos = FOUND_SIZE;
	FOUND_GROW(16 + 7 + version_size + proxies_size + hostname_size);
	memcpy(&FOUND_BUF[pos], trailer, 7);	/* Store the open trailer */
	pos += 7;

	if (version_size) {
		memcpy(&FOUND_BUF[pos], version, version_size);	/* Store "GTKGV1" */
		pos += version_size;
	}

	if (proxies_size) {
		memcpy(&FOUND_BUF[pos], push_proxies, proxies_size); /* Store "PUSH" */
		pos += proxies_size;
	}

	if (hostname_size) {
		memcpy(&FOUND_BUF[pos], hostname, hostname_size); /* Store "HNAME" */
		pos += hostname_size;
	}

	memcpy(&FOUND_BUF[pos], guid, 16);	/* Store the GUID */

	/* Payload size including the search results header, actual results */
	pl = FOUND_SIZE - sizeof(struct gnutella_header);

	packet_head = (struct gnutella_header *) FOUND_BUF;
	memcpy(&packet_head->muid, &n->header.muid, 16);

	/*
	 * We limit the TTL to the minimal possible value, then add a margin
	 * of 5 to account for re-routing abilities some day.  We then trim
	 * at our configured hard TTL limit.  Replies are precious packets,
	 * it would be a pity if they did not make it back to their source.
	 *
	 *			 --RAM, 02/02/2001
	 */

	if (n->header.hops == 0) {
		g_warning
			("search_request(): hops=0, bug in route_message()?\n");
		n->header.hops++;	/* Can't send message with TTL=0 */
	}

	packet_head->function = GTA_MSG_SEARCH_RESULTS;
	packet_head->ttl = MIN((guint) n->header.hops + 5, hard_ttl_limit);
	packet_head->hops = 0;
	WRITE_GUINT32_LE(pl, packet_head->size);

	search_head = (struct gnutella_search_results_out *)
		&FOUND_BUF[sizeof(struct gnutella_header)];

	search_head->num_recs = FOUND_FILES;	/* One byte, little endian! */

	/*
	 * Compute connection speed dynamically if requested.
	 */

	connect_speed = connection_speed;
	if (compute_connection_speed) {
		connect_speed = max_uploads == 0 ?
			0 : (MAX(bsched_avg_bps(bws.out), bsched_bwps(bws.out)) * 8 / 1024);
		if (max_uploads > 0 && connect_speed == 0)
			connect_speed = 32;		/* No b/w limit set and no traffic yet */
	}
	connect_speed /= MAX(1, max_uploads);	/* Upload speed expected per slot */

	WRITE_GUINT16_LE(listen_port, search_head->host_port);
	WRITE_GUINT32_BE(listen_ip(), search_head->host_ip);
	WRITE_GUINT32_LE(connect_speed, search_head->host_speed);

	gmsg_sendto_one(n, (gchar *) FOUND_BUF, FOUND_SIZE);
}

/*
 * Callback from st_search(), for each matching file.	--RAM, 06/10/2001
 *
 * Returns TRUE if we inserted the record, FALSE if we refused it due to
 * lack of space.
 */
static gboolean got_match(struct shared_file *sf)
{
	guint32 pos = FOUND_SIZE;
	guint32 needed = 8 + 2 + sf->file_name_len;		/* size of hit entry */
	gboolean sha1_available;
	gnet_host_t hvec[QHIT_MAX_ALT];
	gint hcnt = 0;

	g_assert(sf->fi == NULL);	/* Cannot match partially downloaded files */

	sha1_available = SHARE_F_HAS_DIGEST ==
		(sf->flags & (SHARE_F_HAS_DIGEST | SHARE_F_RECOMPUTING));
	
	/*
	 * We don't stop adding records if we refused this one, hence the TRUE
	 * returned.
	 */

	if (shared_file_already_in_found_set(sf))
		return TRUE;

	put_shared_file_into_found_set(sf);

	/*
	 * In case we emit the SHA1 as a GGEP "H", we'll grow the buffer
	 * larger necessary, since the extension will take at most 26 bytes,
	 * and could take only 25.  This is NOT a problem, as we later adjust
	 * the real size to fit the data we really emitted.
	 *
	 * If some alternate locations are available, they'll be included as
	 * GGEP "ALT" afterwards.
	 */

	if (sha1_available) {
		needed += 9 + SHA1_BASE32_SIZE;
		hcnt = dmesh_fill_alternate(sf->sha1_digest, hvec, QHIT_MAX_ALT);
		needed += hcnt * 6 + 6;
	}

	/*
	 * Refuse entry if we don't have enough room.	-- RAM, 22/01/2002
	 */

	if (pos + needed + QHIT_MIN_TRAILER_LEN > search_answers_forward_size)
		return FALSE;

	/*
	 * Grow buffer by the size of the search results header 8 bytes,
	 * plus the string length - NULL, plus two NULL's
	 */

	FOUND_GROW(needed);

	WRITE_GUINT32_LE(sf->file_index, &FOUND_BUF[pos]); pos += 4;
	WRITE_GUINT32_LE(sf->file_size, &FOUND_BUF[pos]);  pos += 4;

	memcpy(&FOUND_BUF[pos], sf->file_name, sf->file_name_len);
	pos += sf->file_name_len;

	/* Position equals the next byte to be writen to */

	FOUND_BUF[pos++] = '\0';

	if (sha1_available) {
		gchar *ggep_h_addr = NULL;

		/*
		 * Emit the SHA1, either as GGEP "H" or as a plain ASCII URN.
		 */

		if (use_ggep_h) {
			/* Modern way: GGEP "H" for binary URN */
			guint8 type = GGEP_H_SHA1;
			struct iovec iov[2];
			gint w;
			guint32 flags = GGEP_W_FIRST | GGEP_W_COBS;

			iov[0].iov_base = (gpointer) &type;
			iov[0].iov_len = 1;

			iov[1].iov_base = sf->sha1_digest;
			iov[1].iov_len = SHA1_RAW_SIZE;

			if (hcnt == 0)
				flags |= GGEP_W_LAST;	/* Nothing will follow */

			w = ggep_ext_writev((gchar *) &FOUND_BUF[pos], FOUND_LEFT(pos),
					"H", iov, G_N_ELEMENTS(iov), flags);

			if (w == -1)
				g_warning("could not write GGEP \"H\" extension in query hit");
			else {
				pos += w;			/* Could be COBS-encoded, we don't know */
				ggep_h_addr = &FOUND_BUF[pos] + 1;	/* Skip leading magic */
			}
		} else {
			/* Good old way: ASCII URN */
			gchar *b32 = sha1_base32(sf->sha1_digest);
			memcpy(&FOUND_BUF[pos], "urn:sha1:", 9);
			pos += 9;
			memcpy(&FOUND_BUF[pos], b32, SHA1_BASE32_SIZE);
			pos += SHA1_BASE32_SIZE;
		}

		/*
		 * If we have known alternate locations, include a few of them for
		 * this file in the GGEP "ALT" extension.
		 */

		if (hcnt > 0) {
			gchar alts[6 * QHIT_MAX_ALT];
			gchar *p;
			gint i;
			gint alts_len;
			struct iovec iov[1];
			guint32 flags = GGEP_W_LAST | GGEP_W_COBS;
			gint w;

			g_assert(hcnt <= QHIT_MAX_ALT);

			for (i = 0, p = alts; i < hcnt; i++) {
				WRITE_GUINT32_BE(hvec[i].ip, p);
				p += 4;
				WRITE_GUINT16_LE(hvec[i].port, p);
				p += 2;
			}

			alts_len = p - alts;

			g_assert(alts_len % 6 == 0);

			iov[0].iov_base = (gpointer) alts;
			iov[0].iov_len = alts_len;

			if (ggep_h_addr == NULL)
				flags |= GGEP_W_FIRST;		/* Nothing before */

			w = ggep_ext_writev((gchar *) &FOUND_BUF[pos], FOUND_LEFT(pos),
					"ALT", iov, G_N_ELEMENTS(iov), flags);

			if (w == -1)
				g_warning(
					"could not write GGEP \"ALT\" extension in query hit");
			else
				pos += w;			/* Could be COBS-encoded, we don't know */
		}
	}

	FOUND_BUF[pos++] = '\0';
	FOUND_FILES++;

	/*
	 * Because we don't know exactly the size of the GGEP extension
	 * (could be COBS-encoded or not), we need to adjust the real
	 * extension size now that the entry is fully written.
	 */

	FOUND_SIZE = pos;

	/*
	 * If we have reached our size limit for query hits, flush what
	 * we have so far.
	 */

	if (FOUND_SIZE >= QHIT_SIZE_THRESHOLD || FOUND_FILES >= QHIT_MAX_RESULTS) {
		flush_match();
		FOUND_RESET();
	}

	return TRUE;		/* Hit entry accepted */
}

#define MIN_WORD_LENGTH 1		/* For compaction */

/*
 * compact_query:
 *
 * Remove unnecessary ballast from a query before processing it. Works in
 * place on the given string. Removed are all consecutive blocks of
 * whitespace and all word shorter then MIN_WORD_LENGTH.
 *
 * If `utf8_len' is non-zero, then we're facing an UTF-8 string.
 */
guint compact_query(gchar *search, gint utf8_len)
{
	gchar *s;
	gchar *w;
	gboolean skip_space = TRUE;
	gint word_length = 0;
	guint32 c;
	gint clen;
	gboolean is_utf8 = utf8_len != 0;

	if (dbg > 4)
		printf("original (%s): [%s]\n", is_utf8 ? "UTF-8" : "ASCII", search);

	w = s = search;
	while (
		(c = utf8_len ?
			utf8_decode_char(s, utf8_len, &clen, FALSE) :
			(guint32) *(guchar *) s)
	) {
		if (c == ' ') {
			/*
			 * Reduce consecutive spaces to a single space.
			 */
			if (!skip_space) {
				if (word_length < MIN_WORD_LENGTH) {
					/* 
					 * reached end of very short word in query. drop
					 * that word by rewinding write position
					 */
					if (dbg > 4)
						printf("w");
					w -= word_length;
				} else {
					/* copy space to final position, reset word length */
					*w++ = ' ';
				}
				skip_space = TRUE;
				word_length = 0; /* count this space to the next word */
			} else if (dbg > 4)
				printf("s");
		} else {
			/*
			 * Within a word now, copy character.
			 */
			skip_space = FALSE;
			if (utf8_len) {
				gint i;
				for (i = 0; i < clen; i++)
					*w++ = s[i];
				word_length += clen;	/* Yes, count 3-wide char as 3 */
			} else {
				*w++ = c;
				word_length++;
			}
		}
	
		/* count the length of the original search string */
		if (utf8_len) {
			s += clen;
			utf8_len -= clen;
			g_assert(utf8_len >= 0);
		} else
			s++;
	}

	/* maybe very short word at end of query, then drop */
	if ((word_length > 0) && (word_length < MIN_WORD_LENGTH)) {
		if (dbg > 4)
			printf("e");
		w -= word_length;
		skip_space = TRUE;
	}
	
	/* space left at end of query but query not empty, drop */
	if (skip_space && (w != search)) {
		if (dbg > 4)
			printf("t");
		w--;
	}

	*w = '\0'; /* terminate mangled query */

	if (dbg > 4 && w != s)
		printf("\nmangled (%s): [%s]\n", is_utf8 ? "UTF-8" : "ASCII", search);

	/* search does no longer contain unnecessary whitespace */
	return w - search;
}

/*
 * query_utf8_decode
 *
 * Given a query `text' of `len' bytes:
 *
 * If query is UTF8, compute its length and store it in `retlen'.
 * If query starts with a BOM mark, skip it and set `retoff' accordingly.
 *
 * Returns FALSE on bad UTF-8, TRUE otherwise.
 */
static gboolean query_utf8_decode(
	const gchar *text, gint len, gint *retlen, gint *retoff)
{
	gint offset = 0;
	gint utf8_len = -1;

	/*
	 * Look whether we're facing an UTF-8 query.
	 *
	 * If it starts with the sequence EF BB BF (BOM in UTF-8), then
	 * it is clearly UTF-8.  If we can't decode it, it is bad UTF-8.
	 */

	if (len >= 3) {
		const guchar *p = (guchar *) text;
		if (p[0] == 0xef && p[1] == 0xbb && p[2] == 0xbf) {
			offset = 3;				/* Is UTF-8, skip BOM */
			if (
				len == offset ||
				!(utf8_len = utf8_is_valid_string(text + offset, len - offset))
			)
				return FALSE;		/* Bad UTF-8 encoding */
		}
	}

	if (utf8_len == -1) {
		utf8_len = utf8_is_valid_string(text, len);
		if (utf8_len && utf8_len == len)			/* Is pure ASCII */
			utf8_len = 0;							/* Not fully UTF-8 */
	}

	*retlen = utf8_len;
	*retoff = offset;

	return TRUE;
}

/*
 * Searches requests (from others nodes) 
 * Basic matching. The search request is made lowercase and
 * is matched to the filenames in the LL.
 *
 * If `qhv' is not NULL, it is filled with hashes of URN or query words,
 * so that we may later properly route the query among the leaf nodes.
 *
 * Returns TRUE if the message should be dropped and not propagated further.
 */
gboolean search_request(struct gnutella_node *n, query_hashvec_t *qhv)
{
	guchar found_files = 0;
	guint16 req_speed;
	gchar *search;
	guint32 search_len;
	guint32 max_replies;
	gboolean skip_file_search = FALSE;
	extvec_t exv[MAX_EXTVEC];
	gint exvcnt = 0;
	struct {
		gchar sha1_digest[SHA1_RAW_SIZE];
		gboolean matched;
	} exv_sha1[MAX_EXTVEC];
	gchar *last_sha1_digest = NULL;
	gint exv_sha1cnt = 0;
	gint utf8_len = -1;
	gint offset = 0;			/* Query string start offset */
	gboolean drop_it = FALSE;
	gboolean oob = FALSE;		/* Wants out-of-band query hit delivery? */

	/*
	 * Make sure search request is NUL terminated... --RAM, 06/10/2001
	 *
	 * We can't simply check the last byte, because there can be extensions
	 * at the end of the query after the first NUL.  So we need to scan the
	 * string.  Note that we use this scanning opportunity to also compute
	 * the search string length.
	 *		--RAN, 21/12/2001
	 */

	search = n->data + 2;
	search_len = 0;

	/* open a block, since C doesn't allow variables to be declared anywhere */
	{
		gchar *s = search;
		guint32 max_len = n->size - 3;		/* Payload size - Speed - NUL */

        while (search_len <= max_len && *s++)
            search_len ++;

		if (search_len > max_len) {
			g_assert(n->data[n->size - 1] != '\0');
			if (dbg)
				g_warning("query (hops=%d, ttl=%d) had no NUL (%d byte%s)",
					n->header.hops, n->header.ttl, n->size - 2,
					n->size == 3 ? "" : "s");
			if (dbg > 4)
				dump_hex(stderr, "Query Text", search, MIN(n->size - 2, 256));

            gnet_stats_count_dropped(n, MSG_DROP_QUERY_NO_NUL);
			return TRUE;		/* Drop the message! */
		}
		/* We can now use `search' safely as a C string: it embeds a NUL */

		/*
		 * Drop the "QTRAX2_CONNECTION" queries as being "overhead".
		 */

#define QTRAX_STRLEN	(sizeof("QTRAX2_CONNECTION")-1)

		if (
			search_len >= QTRAX_STRLEN &&
			search[0] == 'Q' &&
			search[1] == 'T' &&
			0 == strncmp(search, "QTRAX2_CONNECTION", QTRAX_STRLEN)
		) {
            gnet_stats_count_dropped(n, MSG_DROP_QUERY_OVERHEAD);
			return TRUE;		/* Drop the message! */
		}

#undef QTRAX_STRLEN

    }

	/*
	 * Compact query, if requested and we're going to relay that message.
	 */

	if (
		gnet_compact_query &&
		n->header.ttl &&
		current_peermode != NODE_P_LEAF
	) {
		guint32 mangled_search_len;

		/*
		 * Look whether we're facing an UTF-8 query.
		 */

		if (!query_utf8_decode(search, search_len, &utf8_len, &offset)) {
			gnet_stats_count_dropped(n, MSG_DROP_MALFORMED_UTF_8);
			return TRUE;					/* Drop message! */
		} else if (utf8_len)
			gnet_stats_count_general(n, GNR_QUERY_UTF8, 1);

		/*
		 * Compact the query, offsetting from the start as needed in case
		 * there is a leading BOM (our UTF-8 decoder does not allow BOM
		 * within the UTF-8 string, and rightly I think: that would be pure
		 * gratuitous bloat).
		 */

		mangled_search_len = compact_query(search + offset, utf8_len);

		g_assert(mangled_search_len <= search_len - offset);
	
		if (mangled_search_len != search_len - offset) {
			gnet_stats_count_general(n, GNR_QUERY_COMPACT_COUNT, 1);
			gnet_stats_count_general(n, GNR_QUERY_COMPACT_SIZE,
				search_len - offset - mangled_search_len);
		}

		/*
		 * Need to move the trailing data forward and adjust the
		 * size of the packet.
		 */

		g_memmove(
			search+offset+mangled_search_len, /* new end of query string */
			search+search_len,                /* old end of query string */
			n->size - (search - n->data) - search_len); /* trailer len */

		n->size -= search_len - offset - mangled_search_len;
		WRITE_GUINT32_LE(n->size, n->header.size);
		search_len = mangled_search_len + offset;

		g_assert(search[search_len] == '\0');
	} 

	/*
	 * If there are extra data after the first NUL, fill the extension vector.
	 */

	if (search_len + 3 != n->size) {
		gint extra = n->size - 3 - search_len;		/* Amount of extra data */
		gint i;

		exvcnt = ext_parse(search + search_len + 1, extra, exv, MAX_EXTVEC);

		if (exvcnt == MAX_EXTVEC) {
			g_warning("%s has %d extensions!",
				gmsg_infostr(&n->header), exvcnt);
			if (dbg)
				ext_dump(stderr, exv, exvcnt, "> ", "\n", TRUE);
			if (dbg > 1)
				dump_hex(stderr, "Query", search, n->size - 2);
		}

		if (exvcnt && dbg > 3) {
			printf("Query with extensions: %s\n", search);
			ext_dump(stdout, exv, exvcnt, "> ", "\n", dbg > 4);
		}

		/*
		 * If there is a SHA1 URN, validate it and extract the binary digest
		 * into sha1_digest[], and set `sha1_query' to the base32 value.
		 */

		for (i = 0; i < exvcnt; i++) {
			extvec_t *e = &exv[i];

			if (e->ext_token == EXT_T_OVERHEAD) {
				if (dbg > 6)
					dump_hex(stderr, "Query Packet (BAD: has overhead)",
						search, MIN(n->size - 2, 256));
				gnet_stats_count_dropped(n, MSG_DROP_QUERY_OVERHEAD);
				return TRUE;			/* Drop message! */
			}

			if (e->ext_token == EXT_T_URN_SHA1) {
				gchar *sha1_digest = exv_sha1[exv_sha1cnt].sha1_digest;

				if (e->ext_paylen == 0)
					continue;				/* A simple "urn:sha1:" */

				if (
					!huge_sha1_extract32(e->ext_payload, e->ext_paylen,
						sha1_digest, &n->header, FALSE)
                ) {
                    gnet_stats_count_dropped(n, MSG_DROP_MALFORMED_SHA1);
					return TRUE;			/* Drop message! */
                }

				exv_sha1[exv_sha1cnt].matched = FALSE;
				exv_sha1cnt++;

				if (dbg > 4)
					printf("Valid SHA1 #%d in query: %32s\n",
						exv_sha1cnt, e->ext_payload);

				/*
				 * Add valid URN query to the list of query hashes, if we
				 * are to fill any for query routing.
				 */

				if (qhv != NULL) {
					gm_snprintf(stmp_1, sizeof(stmp_1),
						"urn:sha1:%s", sha1_base32(sha1_digest));
					qhvec_add(qhv, stmp_1, QUERY_H_URN);
				}

				last_sha1_digest = sha1_digest;
			}
		}

		if (exv_sha1cnt)
			gnet_stats_count_general(n, GNR_QUERY_SHA1, 1);
	}

    /*
     * Reorderd the checks: if we drop the packet, we won't notify any
     * listeners. We first check whether we want to drop the packet and
     * later decide whether we are eligible for answering the query:
     * 1) try top drop
     * 2) notify listeners
     * 3) bail out if not eligible for a local search
     * 4) local search
     *      --Richard, 11/09/2002
     */

	/*
	 * If the query comes from a node farther than our TTL (i.e. the TTL we'll
	 * use to send our reply), don't bother processing it: the reply won't
	 * be able to reach the issuing node.
	 *
	 * However, note that for replies, we use our maximum configured TTL, so
	 * we compare to that, and not to my_ttl, which is the TTL used for
	 * "standard" packets.
	 *
	 *				--RAM, 12/09/2001
	 */

    if (n->header.hops > max_ttl) {
        gnet_stats_count_dropped(n, MSG_DROP_MAX_TTL_EXCEEDED);
		return TRUE;					/* Drop this long-lived search */
    }

	/*
	 * When an URN search is present, there can be an empty search string.
	 *
	 * If requester if farther than 3 hops. save bandwidth when returning
	 * lots of hits from short queries, which are not specific enough.
	 * The idea here is to give some response, but not too many.
	 *
	 * Notes from RAM, 09/09/2001:
	 * 1) The hop amount must be made configurable.
	 * 2) We can add a config option to forbid forwarding of such queries.
	 */

	if (
		search_len <= 1 ||
		(search_len < 5 && n->header.hops > 3)
	)
		skip_file_search = TRUE;

    if (0 == exv_sha1cnt && skip_file_search) {
        gnet_stats_count_dropped(n, MSG_DROP_QUERY_TOO_SHORT);
		return TRUE;					/* Drop this search message */
    }

	/*
	 * When we are not a leaf node, we do two sanity checks here:
	 * 
	 * 1. We keep track of all the queries sent by the node (hops = 1)
	 *    and the time by which we saw them.  If they are sent too often,
	 *    just drop the duplicates.  Since an Ultranode will send queries
	 *    from its leaves with an adjusted hop, we only do that for leaf
	 *    nodes.
	 *
	 * 2. We keep track of all queries relayed by the node (hops >= 1)
	 *    by hops and by search text for a limited period of time.
	 *    The purpose is to sanitize the traffic if the node did not do
	 *    point #1 above for its own neighbours.  Naturally, we expire
	 *    this data more quickly.
	 *
	 * When there is a SHA1 in the query, it is the SHA1 itself that is
	 * being remembered.
	 *
	 *		--RAM, 09/12/2003
	 */

	if (n->header.hops == 1 && n->qseen != NULL) {
		time_t now = time(NULL);
		time_t seen = 0;
		gboolean found;
		gpointer atom;
		gpointer seenp;
		gchar *query = search;

		g_assert(NODE_IS_LEAF(n));

		if (last_sha1_digest != NULL) {
			gm_snprintf(stmp_1, sizeof(stmp_1),
				"urn:sha1:%s", sha1_base32(last_sha1_digest));
			query = stmp_1;
		}

		found = g_hash_table_lookup_extended(n->qseen, query, &atom, &seenp);
		if (found)
			seen = (time_t) GPOINTER_TO_INT(seenp);

		if (delta_time(now, (time_t) 0) - seen < node_requery_threshold) {
			if (dbg) g_warning("node %s (%s) re-queried \"%s\" after %d secs",
				node_ip(n), node_vendor(n), query, (gint) (now - seen));
			gnet_stats_count_dropped(n, MSG_DROP_THROTTLE);
			return TRUE;		/* Drop the message! */
		}

		if (!found)
			atom = atom_str_get(query);

		g_hash_table_insert(n->qseen, atom,
			GINT_TO_POINTER((gint) delta_time(now, (time_t) 0)));
	}

	/*
	 * For point #2, there are two tables to consider: `qrelayed_old' and
	 * `qrelayed'.  Presence in any of the tables is sufficient, but we
	 * only insert in the "new" table `qrelayed'.
	 */

	if (n->qrelayed != NULL) {					/* Check #2 */
		gpointer found = NULL;

		g_assert(!NODE_IS_LEAF(n));

		/*
		 * Consider both hops and TTL for dynamic querying, whereby the
		 * same query can be repeated with an increased TTL.
		 */

		if (last_sha1_digest == NULL)
			gm_snprintf(stmp_1, sizeof(stmp_1),
				"%d/%d%s", n->header.hops, n->header.ttl, search);
		else
			gm_snprintf(stmp_1, sizeof(stmp_1),
				"%d/%durn:sha1:%s", n->header.hops, n->header.ttl,
				sha1_base32(last_sha1_digest));

		if (n->qrelayed_old != NULL)
			found = g_hash_table_lookup(n->qrelayed_old, stmp_1);

		if (found == NULL)
			found = g_hash_table_lookup(n->qrelayed, stmp_1);

		if (found != NULL) {
			if (dbg) g_warning("dropping query \"%s%s\" (hops=%d, TTL=%d) "
				"already seen recently from %s (%s)",
				last_sha1_digest == NULL ? "" : "urn:sha1:",
				last_sha1_digest == NULL ?
					search : sha1_base32(last_sha1_digest),
				n->header.hops, n->header.ttl,
				node_ip(n), node_vendor(n));
			gnet_stats_count_dropped(n, MSG_DROP_THROTTLE);
			return TRUE;		/* Drop the message! */
		}

		g_hash_table_insert(n->qrelayed,
			atom_str_get(stmp_1), GINT_TO_POINTER(1));
	}

    /*
     * Push the query string to interested ones.
     */
    if (
		(search[0] == '\0' || (search[0] == '\\' && search[1] == '\0'))
		&& exv_sha1cnt
    ) {
		gint i;
		for (i = 0; i < exv_sha1cnt; i++)
			share_emit_search_request(QUERY_SHA1,
				sha1_base32(exv_sha1[i].sha1_digest), n->ip, n->port);
	} else
		share_emit_search_request(QUERY_STRING, search, n->ip, n->port);

	READ_GUINT16_LE(n->data, req_speed);

	/*
	 * Special processing for the "connection speed" field of queries.
	 *
	 * Unless bit 15 is set, process as a speed.
	 * Otherwise if bit 15 is set:
	 *
	 * 1. If the firewall bit (bit 14) is set, the remote servent is firewalled.
	 *    Therefore, if we are also firewalled, don't reply.
	 *
	 * 2. If the XML bit (bit 13) is cleared and we support XML meta data, don't
	 *    include them in the result set [GTKG does not support XML meta data]
	 *
	 *		--RAM, 19/01/2003, updated 06/07/2003 (bit 14-13 instead of 8-9)
	 *
	 * 3. If the GGEP "H" bit (bit 11) is set, the issuer of the query will
	 *    understand the "H" extension in query hits.
	 *		--RAM, 16/07/2003
	 *
	 * Starting today (06/07/2003), we ignore the connection speed overall
	 * if it's not marked with the QUERY_SPEED_MARK flag to indicate new
	 * interpretation. --RAM
	 */

	use_ggep_h = FALSE;

	if (req_speed & QUERY_SPEED_MARK) {
		if ((req_speed & QUERY_SPEED_FIREWALLED) && is_firewalled)
			return FALSE;			/* Both servents are firewalled */

		if (req_speed & QUERY_SPEED_GGEP_H)
			use_ggep_h = TRUE;
	}

	oob = (req_speed & QUERY_SPEED_OOB_REPLY) != 0;

	/*
	 * If we aren't going to let the searcher download anything, then
	 * don't waste bandwidth and his time by giving him search results.
	 *		--Mark Schreiber, 11/01/2002
     *
     * Also don't waste any time if we don't share a file.
     *      -- Richard, 9/9/2002
	 */

	if (files_scanned == 0 || !upload_is_enabled())
		return FALSE;

	/*
	 * If query comes from GTKG 0.91 or later, it understands GGEP "H".
	 * Otherwise, it's an old servent or one unwilling to support this new
	 * extension, so it will get its SHA1 URNs in ASCII form.
	 *		--RAM, 17/11/2002
	 */

	{
		guint8 major, minor;
		gboolean release;

		if (
			guid_query_muid_is_gtkg(
				n->header.muid, oob, &major, &minor, &release)
		) {
			/* Only supersede `use_ggep_h' if not indicated in "min speed" */
			if (!use_ggep_h)
				use_ggep_h =
					major >= 1 || minor > 91 || (minor == 91 && release);

			if (dbg > 3)
				printf("GTKG %squery from %d.%d%s\n",
					guid_is_requery(n->header.muid) ? "re-" : "",
					major, minor, release ? "" : "u");
		}
	}

	/*
	 * If OOB reply is wanted, we have the IP/port of the querier.
	 * Verify against the hotile IP addresses...
	 */

	if (oob) {
		guint32 ip;

		guid_oob_get_ip_port(n->header.muid, &ip, NULL);

		if (hostiles_check(ip)) {
			gnet_stats_count_dropped(n, MSG_DROP_HOSTILE_IP);
			return TRUE;		/* Drop the message! */
		}
	}

	/*
	 * Perform search...
	 */

    gnet_stats_count_general(n, GNR_LOCAL_SEARCHES, 1);
	if (current_peermode == NODE_P_LEAF && node_ultra_received_qrp(n))
		node_inc_qrp_query(n);
	found_reset(n);

	max_replies = (search_max_items == (guint32) -1) ? 255 : search_max_items;

	/*
	 * Search each SHA1.
	 */

	if (exv_sha1cnt) {
		gint i;

		for (i = 0; i < exv_sha1cnt && max_replies > 0; i++) {
			struct shared_file *sf;

			sf = shared_file_by_sha1(exv_sha1[i].sha1_digest);
			if (sf && sf != SHARE_REBUILDING && sf->fi == NULL) {
				got_match(sf);
				max_replies--;
				found_files++;
			}
		}
	}

	if (!skip_file_search) {
		gboolean is_utf8 = FALSE;
		gboolean ignore = FALSE;

		/*
		 * Keep only UTF8 encoded queries (This includes ASCII)
		 */

		g_assert(search[search_len] == '\0');

		if (
			utf8_len == -1 &&
			!query_utf8_decode(search, search_len, &utf8_len, &offset)
		) {
			gnet_stats_count_dropped(n, MSG_DROP_MALFORMED_UTF_8);
			drop_it = TRUE;					/* Drop message! */
			goto finish;					/* Flush any SHA1 result we have */
		} else if (utf8_len)
			gnet_stats_count_general(n, GNR_QUERY_UTF8, 1);

		is_utf8 = utf8_len > 0;

		/*
		 * Because st_search() will apply a character map over the string,
		 * we always need to copy the query string to avoid changing the
		 * data inplace.
		 *
		 * `stmp_1' is a static buffer.  Note that we copy the trailing NUL
		 * into the buffer, hence the "+1" below.
		 */

		search_len -= offset;
		memcpy(stmp_1, search + offset, search_len + 1);

#ifdef USE_ICU
		if (!is_utf8) {
			char* stmp_2;

			stmp_2 = iso_8859_1_to_utf8(stmp_1);
			if (!stmp_2 || (strlen(stmp_2) < search_len)) {
				/* COUNT MARK : Not an utf8 and not an iso-8859-1 query */
				ignore = TRUE;
			} else {
				/* COUNT MARK : We received an ISO-8859-1 query */
				use_map_on_query(stmp_1, search_len);
			}
		}

		/*
		 * Here we suppose that the peer has the same NFKD/NFC keyword algo
		 * than us, see unicode_canonize() in utf8.c.
		 * (It must anyway, for compatibility with the QRP)
		 */
#else
		if (is_utf8) {
			gint isochars;

			isochars = utf8_to_iso8859(stmp_1, search_len, TRUE);

			if (isochars != utf8_len)		/* Not fully ISO-8859-1 */
				ignore = TRUE;

			if (dbg > 4)
				printf("UTF-8 query, len=%d, utf8-len=%d, iso-len=%d: \"%s\"\n",
					search_len, utf8_len, isochars, stmp_1);
		}
#endif

		if (!ignore)
			found_files +=
				st_search(&search_table, stmp_1, got_match, max_replies, qhv);
	}

finish:
	if (found_files > 0) {
        gnet_stats_count_general(n, GNR_LOCAL_HITS, found_files);
		if (current_peermode == NODE_P_LEAF && node_ultra_received_qrp(n))
			node_inc_qrp_match(n);

		if (FOUND_FILES)			/* Still some unflushed results */
			flush_match();			/* Send last packet */

		if (dbg > 3) {
			printf("Share HIT %u files '%s'%s ", (gint) found_files,
				search + offset,
				skip_file_search ? " (skipped)" : "");
			if (exv_sha1cnt) {
				gint i;
				for (i = 0; i < exv_sha1cnt; i++)
					printf("\n\t%c(%32s)",
						exv_sha1[i].matched ? '+' : '-',
						sha1_base32(exv_sha1[i].sha1_digest));
				printf("\n\t");
			}
			printf("req_speed=%u ttl=%d hops=%d\n", (guint) req_speed,
				(gint) n->header.ttl, (gint) n->header.hops);
			fflush(stdout);
		}
	}

	return drop_it;
}

/*
 * SHA1 digest processing
 */

/* 
 * This tree maps a SHA1 hash (base-32 encoded) onto the corresponding
 * shared_file if we have one.
 */

static GTree *sha1_to_share = NULL;

/* 
 * compare_share_sha1
 * 
 * Compare binary SHA1 hashes.
 * Return 0 if they're the same, a negative or positive number if s1 if greater
 * than s2 or s1 greater than s2, respectively.
 * Used to search the sha1_to_share tree.
 */

static int compare_share_sha1(const gchar *s1, const gchar *s2)
{
	return memcmp(s1, s2, SHA1_RAW_SIZE);
}

/* 
 * reinit_sha1_table
 * 
 * Reset sha1_to_share
 */

static void reinit_sha1_table(void)
{
	if (sha1_to_share)
		g_tree_destroy(sha1_to_share);

	sha1_to_share = g_tree_new((GCompareFunc) compare_share_sha1);
}

/* 
 * set_sha1
 * 
 * Set the SHA1 hash of a given shared_file. Take care of updating the
 * sha1_to_share structure. This function is called from inside the bowels of
 * sha1_server.c when it knows what the hash associated to a file is.
 *
 * FIXME: sha1_server.c?? There's no such file. Maybe it's about huge.c?
 */

void set_sha1(struct shared_file *f, const char *sha1)
{
	g_assert(f->fi == NULL);		/* Cannot be a partial file */

	/*
	 * If we were recomputing the SHA1, remove the old version.
	 */

	if (f->flags & SHARE_F_RECOMPUTING) {
		f->flags &= ~SHARE_F_RECOMPUTING;
		g_tree_remove(sha1_to_share, f->sha1_digest);
	}

	memcpy(f->sha1_digest, sha1, SHA1_RAW_SIZE);
	f->flags |= SHARE_F_HAS_DIGEST;
	g_tree_insert(sha1_to_share, f->sha1_digest, f);
}

/*
 * sha1_hash_available
 * 
 * Predicate returning TRUE if the SHA1 hash is available for a given
 * shared_file, FALSE otherwise.
 *
 * Use sha1_hash_is_uptodate() to check for availability and accurateness.
 */

gboolean sha1_hash_available(const struct shared_file *sf)
{
	return SHARE_F_HAS_DIGEST ==
		(sf->flags & (SHARE_F_HAS_DIGEST | SHARE_F_RECOMPUTING));
}

/*
 * sha1_hash_is_uptodate
 *
 * Predicate returning TRUE if the SHA1 hash is available AND is up to date
 * for the shared file.
 *
 * NB: if the file is found to have changed, the background computation of
 * the SHA1 is requested.
 */
gboolean sha1_hash_is_uptodate(struct shared_file *sf)
{
	struct stat buf;

	if (!(sf->flags & SHARE_F_HAS_DIGEST))
		return FALSE;

	if (sf->flags & SHARE_F_RECOMPUTING)
		return FALSE;

	/*
	 * If there is a non-NULL `fi' entry, then this is a partially
	 * downloaded file that we are sharing.  Don't try to update its
	 * SHA1 by recomputing it!
	 *
	 * If it's a partial file, don't bother checking whether it exists.
	 * (if gone, we won't be able to serve it, that's all).  But partial
	 * files we serve MUST have known SHA1.
	 */

	if (sf->fi != NULL) {
		g_assert(sf->fi->sha1 != NULL);
		return TRUE;
	}

	if (-1 == stat(sf->file_path, &buf)) {
		g_warning("can't stat shared file #%d \"%s\": %s",
			sf->file_index, sf->file_path, g_strerror(errno));
		g_tree_remove(sha1_to_share, sf->sha1_digest);
		sf->flags &= ~SHARE_F_HAS_DIGEST;
		return FALSE;
	}

	/*
	 * If file was modified since the last time we computed the SHA1,
	 * recompute it and tell them that the SHA1 we have might not be
	 * accurate.
	 */

	if (sf->mtime != buf.st_mtime) {
		g_warning("shared file #%d \"%s\" changed, recomputing SHA1",
			sf->file_index, sf->file_path);
		sf->flags |= SHARE_F_RECOMPUTING;
		sf->mtime = buf.st_mtime;
		request_sha1(sf);
		return FALSE;
	}

	return TRUE;
}

/* 
 * shared_file_complete_by_sha1
 * 
 * Returns the shared_file if we share a complete file bearing the given SHA1.
 * Returns NULL if we don't share a complete file, or SHARE_REBUILDING if the
 * set of shared file is being rebuilt.
 */
static struct shared_file *shared_file_complete_by_sha1(gchar *sha1_digest)
{
	struct shared_file *f;

	if (sha1_to_share == NULL)			/* Not even begun share_scan() yet */
		return SHARE_REBUILDING;

	f = g_tree_lookup(sha1_to_share, sha1_digest);

	if (!f || !sha1_hash_available(f)) {
		/*
		 * If we're rebuilding the library, we might not have parsed the
		 * file yet, so it's possible we have this URN but we don't know
		 * it yet.	--RAM, 12/10/2002.
		 */

		if (file_table == NULL)			/* Rebuilding the library! */
			return SHARE_REBUILDING;

		return NULL;
	}

	return f;
}

/* 
 * shared_file_by_sha1
 * 
 * Take a given binary SHA1 digest, and return the corresponding
 * shared_file if we have it.
 *
 * NOTA BENE: if the returned "shared_file" structure holds a non-NULL `fi',
 * then it means it is a partially shared file.
 */
struct shared_file *shared_file_by_sha1(gchar *sha1_digest)
{
	struct shared_file *f;

	f = shared_file_complete_by_sha1(sha1_digest);

	/*
	 * If we don't share this file, or if we're rebuilding, and provided
	 * PFSP-server is enabled, look whether we don't have a partially
	 * downloaded file with this SHA1.
	 */

	if ((f == NULL || f == SHARE_REBUILDING) && pfsp_server) {
		struct shared_file *pf = file_info_shared_sha1(sha1_digest);
		if (pf != NULL)
			f = pf;
	}

	return f;
}

/*
 * is_latin_locale
 *
 * Is the locale using the latin alphabet?
 */
gboolean is_latin_locale(void)
{
	return b_latin;
}

/*
 * shared_kbytes_scanned
 *
 * Get accessor for ``kbytes_scanned''
 */
guint64 shared_kbytes_scanned(void)
{
	return kbytes_scanned;
}

/*
 * shared_files_scanned
 *
 * Get accessor for ``files_scanned''
 */
guint64 shared_files_scanned(void)
{
	return files_scanned;
}

/* vi: set ts=4 sw=4 cindent: */
