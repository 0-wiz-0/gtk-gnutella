/*
 * $Id$
 *
 * Copyright (c) 2001-2003, Raphael Manfredi
 * Copyright (c) 2000 Daniel Walker (dwalker@cats.ucsc.edu)
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

#ifndef _core_share_h_
#define _core_share_h_

#include "common.h"
#include "if/core/share.h"

struct extension {
	gchar *str;			/* Extension string (e.g. "html") */
	gint len;			/* Extension length (e.g. 4) */
};

typedef struct shared_file {
	gchar *file_path;		/* The full path of the file */
	const gchar *file_name;	/* Pointer within file_path at start of filename */
	guint32 file_index;		/* the files index within our local DB */
	guint32 file_size;		/* File size in Bytes */
	guint32 flags;			/* See below for definition */
	gint file_name_len;
	time_t mtime;			/* Last modification time, for SHA1 computation */
	gchar sha1_digest[SHA1_RAW_SIZE];	/* SHA1 digest, binary form */
	struct dl_file_info *fi;			/* PFSP-server: the holding fileinfo */
} shared_file_t;

/*
 * shared_file flags
 */

#define SHARE_F_HAS_DIGEST	0x00000001		/* Digest is set */
#define SHARE_F_RECOMPUTING	0x00000002		/* Digest being recomputed */

struct gnutella_search_results_out {
	guchar num_recs;
	guchar host_port[2];
	guchar host_ip[4];
	guchar host_speed[4];

	/* Last 16 bytes = client_id */
};

#define SHARE_REBUILDING	((struct shared_file *) 0x1)

struct gnutella_node;
struct query_hashvec;

/*
 * Global Data
 */

extern GSList *extensions, *shared_dirs;

/*
 * Special return value from shared_file() during library rebuild time.
 * This is needed because we no longer block the GUI whilst scanning.
 */

/*
 * Global Functions
 */

void share_init(void);
struct shared_file *shared_file(guint idx);
struct shared_file *shared_file_by_name(const gchar *basename);
void share_close(void);
gboolean search_request(struct gnutella_node *n, struct query_hashvec *qhv);
void parse_extensions(const gchar *);
gchar *get_file_path(gint);
void shared_dirs_update_prop(void);
gboolean shared_dirs_parse(const gchar *);
gint get_file_size(gint);

guint compact_query(gchar *search);

void shared_file_free(shared_file_t *sf);
void set_sha1(struct shared_file *, const gchar *sha1_digest);
struct shared_file *shared_file_by_sha1(gchar *sha1_digest);
gboolean sha1_hash_available(const struct shared_file *);
gboolean sha1_hash_is_uptodate(struct shared_file *sf);

gboolean is_latin_locale(void);
void use_map_on_query(gchar *query, int len);

#endif /* _core_share_h_ */

/* vi: set ts=4: */
