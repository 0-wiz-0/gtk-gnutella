/*
 * $Id$
 *
 * Copyright (c) 2002-2003, Ch. Tronche & Raphael Manfredi
 *
 * HUGE support (Hash/URN Gnutella Extension).
 *
 * Started by Ch. Tronche (http://tronche.com/) 28/04/2002
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
#include <fcntl.h>
#include <glib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "huge.h"
#include "share.h"
#include "gmsg.h"
#include "header.h"
#include "dmesh.h"
#include "version.h"
#include "file.h"

#include "settings.h"
#include "override.h"		/* Must be the last header included */

RCSID("$Id$");

/***
 *** Server side: computation of SHA1 hash digests and replies.
 *** SHA1 is defined in RFC 3174.
 ***/

/*
 * There's an in-core cache (the GHashTable sha1_cache), and a
 * persistent copy (normally in ~/.gtk-gnutella/sha1_cache). The
 * in-core cache is filled with the persistent one at launch. When the
 * "shared_file" (the records describing the shared files, see
 * share.h) are created, a call is made to sha1_set_digest to fill the
 * SHA1 digest part of the shared_file. If the digest isn't found in
 * the in-core cache, it's computed, stored in the in-core cache and
 * appended at the end of the persistent cache. If the digest is found
 * in the cache, a check is made based on the file size and last
 * modification time. If they're identical to the ones in the cache,
 * the digest is considered to be accurate, and is used. If the file
 * size or last modification time don't match, the digest is computed
 * again and stored in the in-core cache, but it isn't stored in the
 * persistent one. Instead, the cache is marked as dirty, and will be
 * entirely overwritten by dump_cache, called when everything has been
 * computed.
 */

struct sha1_cache_entry {
    gchar *file_name;                     /* Full path name                 */
    off_t  size;                          /* File size                      */
    time_t mtime;                         /* Last modification time         */
    gchar digest[SHA1_RAW_SIZE];          /* SHA1 digest as a binary string */
    gboolean shared;                      /* There's a known entry for this
                                           * file in the share library
                                           */
};

static GHashTable *sha1_cache = NULL;

/* 
 * cache_dirty means that in-core cache is different from the one on disk when
 * TRUE.
 */
static gboolean cache_dirty = FALSE;

/**
 ** Elementary operations on SHA1 values
 **/

/*
 * copy_sha1
 *
 * Copy the ASCII representation of a SHA1 digest from source to dest. 
 */
static void copy_sha1(char *dest, const char *source)
{
	memcpy(dest, source, SHA1_RAW_SIZE);
}

/**
 ** Handling of persistent buffer
 **/

/* In-memory cache */

/* 
 * update_volatile_cache
 *
 * Takes an in-memory cached entry, and update its content.
 */
static void update_volatile_cache(
	struct sha1_cache_entry *sha1_cached_entry,
	off_t size,
	time_t mtime, 
	const char *digest)
{
	sha1_cached_entry->size = size;
	sha1_cached_entry->mtime = mtime;
	copy_sha1(sha1_cached_entry->digest, digest);
	sha1_cached_entry->shared = TRUE;
}

/* add_volatile_cache_entry
 * 
 * Add a new entry to the in-memory cache.
 */
static void add_volatile_cache_entry(
	const char *file_name, 
	off_t size,
	time_t mtime,
	const char *digest, 
	gboolean known_to_be_shared)
{
	struct sha1_cache_entry *new_entry = g_new(struct sha1_cache_entry, 1);
	new_entry->file_name = atom_str_get(file_name);
	new_entry->size = size;
	new_entry->mtime = mtime;
	copy_sha1(new_entry->digest, digest);
	new_entry->shared = known_to_be_shared;
	g_hash_table_insert(sha1_cache, new_entry->file_name, new_entry);
}

/* Disk cache */

static const char sha1_persistent_cache_file_header[] = 
"#\n"
"# gtk-gnutella SHA1 cache file.\n"
"# This file is automatically generated.\n"
"# Format is: SHA1 digest<TAB>file_size<TAB>file_mtime<TAB>file_name\n"
"# Comment lines start with a sharp (#)\n"
"#\n"
"\n";

static char *persistent_cache_file_name = NULL;

/* 
 * add_persistent_cache_entry
 * 
 * Add an entry to the persistent cache.
 */
static void add_persistent_cache_entry(
	const char *file_name,
	off_t size,
	time_t mtime,
	const char *digest)
{
	FILE *persistent_cache;

	if (!persistent_cache_file_name)
		return;

	persistent_cache = file_fopen(persistent_cache_file_name, "a");

	if (persistent_cache == NULL)
		return;

	/*
	 * If we're adding the very first entry (file empty), then emit header.
	 */

	if (0 == ftell(persistent_cache))
		fputs(sha1_persistent_cache_file_header, persistent_cache);

	fprintf(persistent_cache, "%s\t%lu\t%ld\t%s\n",
		sha1_base32(digest), (gulong) size, (glong) mtime, file_name);
	fclose(persistent_cache);
}

/*
 * dump_cache_one_entry
 * 
 * Dump one (in-memory) cache into the persistent cache. This is a callback
 * called by dump_cache to dump the whole in-memory cache onto disk.
 */
static void dump_cache_one_entry(
	const char *file_name,
	struct sha1_cache_entry *e,
	FILE *persistent_cache)
{
	if (!e->shared)
		return;

	fprintf(persistent_cache, "%s\t%lu\t%ld\t%s\n",
		sha1_base32(e->digest), (gulong) e->size, (glong) e->mtime, 
			e->file_name);
}

/*
 * dump_cache
 *
 * Dump the whole in-memory cache onto disk.
 */
static void dump_cache(void)
{
	FILE *persistent_cache;

	persistent_cache = file_fopen(persistent_cache_file_name, "w");

	if (persistent_cache == NULL)
		return;

	fputs(sha1_persistent_cache_file_header, persistent_cache);
	g_hash_table_foreach(sha1_cache,
		(GHFunc)dump_cache_one_entry, persistent_cache);
	fclose(persistent_cache);

	cache_dirty = FALSE;
}

/*
 * parse_and_append_cache_entry
 * 
 * This function is used to read the disk cache into memory. It must be passed
 * one line from the cache (ending with '\n'). It performs all the
 * syntactic processing to extract the fields from the line and calls
 * add_volatile_cache_entry to append the record to the in-memory cache.
 */
static void parse_and_append_cache_entry(char *line)
{
	const char *sha1_digest_ascii;
	const char *file_name;
	char *file_name_end;
	char *p, *end; /* pointers to scan the line */
	off_t size;
	time_t mtime;
	char digest[SHA1_RAW_SIZE];

	/* Skip comments and blank lines */
	if (*line == '#' || *line == '\n') return;

	sha1_digest_ascii = line; /* SHA1 digest is the first field. */

	/* Scan until file size */

	p = line;
	while(*p != '\t' && *p != '\n') p++;

	if (
		*p != '\t' ||
		(p - sha1_digest_ascii) != SHA1_BASE32_SIZE ||
		!base32_decode_into(sha1_digest_ascii, SHA1_BASE32_SIZE,
			digest, sizeof(digest))
	) {
		g_warning("Malformed line in SHA1 cache file %s[SHA1]: %s",
			persistent_cache_file_name, line);
		return;
	}

	/* p is now supposed to point to the beginning of the file size */

	size = strtoul(p, &end, 10);
	if (end == p || *end != '\t') {
		g_warning("Malformed line in SHA1 cache file %s[size]: %s",
			persistent_cache_file_name, line);
		return;
	}

	p = end + 1;

	/*
	 * p is now supposed to point to the beginning of the file last
	 * modification time.
	 */

	mtime = strtoul(p, &end, 10);
	if (end == p || *end != '\t') {
		g_warning("Malformed line in SHA1 cache file %s[mtime]: %s", 
			persistent_cache_file_name, line);
		return;
	}

	p = end + 1;

	/* p is now supposed to point to the file name */

	file_name = p;
	file_name_end = strchr(file_name, '\n');

	if (!file_name_end) {
		g_warning("Malformed line in SHA1 cache file %s[file_name]: %s",
			persistent_cache_file_name, line);
		return;
	}

	/* Set string end markers */
	*file_name_end = '\0';

	add_volatile_cache_entry(file_name, size, mtime, digest, FALSE);
}

/*
 * sha1_read_cache
 * 
 * Read the whole persistent cache into memory.
 */
static void sha1_read_cache(void)
{
	char buffer[4096];
	FILE *persistent_cache_file;
	char *fgets_return;

	if (!settings_config_dir()) {
		g_warning("sha1_read_cache: No config dir");
		return;
	}

	gm_snprintf(buffer, sizeof(buffer), "%s/sha1_cache",
		settings_config_dir());
	persistent_cache_file_name = g_strdup(buffer);
	  
	persistent_cache_file = file_fopen(persistent_cache_file_name, "r");
	if (persistent_cache_file == NULL) {
		cache_dirty = TRUE;
		return;
	}
	  
	for (;;) {
		fgets_return = fgets(buffer, sizeof(buffer), persistent_cache_file);
		if (!fgets_return)
			break;
		parse_and_append_cache_entry(buffer);
	}

	fclose(persistent_cache_file);
}

/**
 ** Asynchronous computation of hash value
 **/

#define HASH_BLOCK_SHIFT	12			/* Power of two of hash unit credit */
#define HASH_BUF_SIZE		65536		/* Size of a the reading buffer */

static gpointer sha1_task = NULL;

/* This is a file waiting either for the digest to be computer, or
 * when computed to be retrofit into the share record. 
 */

struct file_sha1 {
	const char *file_name;
	guint32 file_index;

	/*
	 * This is used only if this record is
	 * in the waiting_for_library_build_complete list.
	 */

	gchar sha1_digest[SHA1_RAW_SIZE];
	struct file_sha1 *next;
};

/* Two useful lists */

/*
 * When a hash is requested for a file and is unknown, it is first stored onto
 * this stack, waiting to be processed.
 */

static struct file_sha1 *waiting_for_sha1_computation = NULL;

/* 
 * When the hash for a file has been computed but cannot be set into the struct
 * shared_file because the function shared_file returned SHARE_REBUILDING (for
 * example), the corresponding struct file_hash is stored into this stack, until
 * such time it's possible to get struct shared_file from index with
 * shared_file. 
 */

static struct file_sha1 *waiting_for_library_build_complete = NULL;

/* 
 * push
 * 
 * Push a record onto a stack (either waiting_for_sha1_computation or
 * waiting_for_library_build_complete).
 */

static void push(struct file_sha1 **stack,struct file_sha1 *record)
{
	record->next = *stack;
	*stack = record;
}

/*
 * free_cell
 *
 * Free a working cell.
 */
static void free_cell(struct file_sha1 *cell)
{
	atom_str_free(cell->file_name);
	G_FREE_NULL(cell);
}

/* The context of the SHA1 computation being performed */

#define SHA1_MAGIC	0xa1a1a1a1

struct sha1_computation_context {
	gint magic;
	SHA1Context context;
	struct file_sha1 *file;
	gchar *buffer;				/* Large buffer where data is read */
	gint fd;
	time_t start;				/* Debugging, show computation rate */
};

static void sha1_computation_context_free(gpointer u)
{
	struct sha1_computation_context *ctx =
		(struct sha1_computation_context *) u;

	g_assert(ctx->magic == SHA1_MAGIC);

	if (ctx->fd != -1)
		close(ctx->fd);

	if (ctx->file)
		free_cell(ctx->file);

	G_FREE_NULL(ctx->buffer);

	wfree(ctx, sizeof(*ctx));
}

/* 
 * put_sha1_back_into_share_library
 * 
 * When SHA1 is computed, and we know what struct share it's related
 * to, we call this function to update set the share SHA1 value.
 */
static void put_sha1_back_into_share_library(
	struct shared_file *sf,
	const char *file_name,
	const char *digest)
{
	struct sha1_cache_entry *cached_sha1;
	struct stat buf;

	g_assert(sf != SHARE_REBUILDING);

	if (!sf) {
		g_warning("got SHA1 for unknown file: %s", file_name);
		return;
	}

	if (0 != strcmp(sf->file_path, file_name)) {

		/*
		 * File name changed since last time
		 * (that is, "rescan dir" was called)
		 */

		g_warning("name of file #%d changed from \"%s\" to \"%s\" (rescan?): "
				"discarding SHA1", sf->file_index, file_name, sf->file_path);
		return;
	}

	/*
	 * Make sure the file's timestamp is still accurate.
	 */

	if (-1 == stat(sf->file_path, &buf)) {
		g_warning("discarding SHA1 for file #%d \"%s\": can't stat(): %s",
			sf->file_index, sf->file_path, g_strerror(errno));
		return;
	}

	if (buf.st_mtime != sf->mtime) {
		g_warning("file #%d \"%s\" was modified whilst SHA1 was computed",
			sf->file_index, sf->file_path);
		sf->mtime = buf.st_mtime;
		request_sha1(sf);					/* Retry! */
		return;
	}

	set_sha1(sf, digest);

	/* Update cache */

	cached_sha1 = (struct sha1_cache_entry *)
		g_hash_table_lookup(sha1_cache, (gconstpointer) sf->file_path);

	if (cached_sha1) {
		update_volatile_cache(cached_sha1, sf->file_size, sf->mtime, digest);
		cache_dirty = TRUE;
	} else {
		add_volatile_cache_entry(sf->file_path,
			sf->file_size, sf->mtime, digest, TRUE);
		add_persistent_cache_entry(sf->file_path,
			sf->file_size, sf->mtime, digest);
	}
}

/* 
 * try_to_put_sha1_back_into_share_library
 *
 * We have some SHA1 we couldn't put the values into the share library
 * because it wasn't available. We try again. This function is called from the
 * sha1_timer. 
 */
static void try_to_put_sha1_back_into_share_library(void)
{
	struct shared_file *sf;

	if (!waiting_for_library_build_complete)
		return;

	/*
	 * Check to see if we'll be able to get the share from the indexes.
	 */

	sf = shared_file(1);
	if (sf == SHARE_REBUILDING)
		return;						/* Nope. Try later. */

	if (dbg > 1)
		printf("try_to_put_sha1_back_into_share_library: flushing...\n");

	while (waiting_for_library_build_complete) {
		struct file_sha1 *f = waiting_for_library_build_complete;
		struct shared_file *sf = shared_file(f->file_index);

		if (dbg > 4)
			printf("flushing file \"%s\" (idx=%u), %sfound in lib\n",
				f->file_name, f->file_index, sf ? "" : "NOT ");

		waiting_for_library_build_complete = f->next;
		put_sha1_back_into_share_library(sf, f->file_name, f->sha1_digest);

		free_cell(f);
	}
}

/* 
 * close_current_file
 * 
 * Close the file whose hash we're computing (after calculation completed) and
 * free the associated structure.
 */
static void close_current_file(struct sha1_computation_context *ctx)
{
	if (ctx->file) {
		free_cell(ctx->file);
		ctx->file = NULL;
	}

	if (ctx->fd != -1) {
		if (dbg > 1) {
			struct stat buf;
			time_t delta = time((time_t *) NULL) - ctx->start;

			if (delta && -1 != fstat(ctx->fd, &buf))
				printf("SHA1 computation rate: %lu bytes/sec\n",
					(gulong) buf.st_size / delta);
		}
		close(ctx->fd);
		ctx->fd = -1;
	}
}

/* 
 * get_next_file_from_list
 * 
 * Get the next file waiting for its hash to be computed from the queue
 * (actually a stack).
 * 
 * Returns this file.
 */
static struct file_sha1 *get_next_file_from_list(void)
{
	struct file_sha1 *l;

	/*
	 * XXX HACK ALERT
	 *
	 * We need to be careful here, because each time the library is rescanned,
	 * we add file to the list of SHA1 to recompute if we don't have them
	 * yet.  This means that when we rescan the library during a computation,
	 * we'll add duplicates to our working queue.
	 *
	 * Fortunately, we can probe our in-core cache to see if what we have
	 * is already up-to-date.
	 *
	 * XXX It would be best to maintain a hash table of all the filenames
	 * XXX in our workqueue and not enqueue the work in the first place.
	 * XXX		--RAM, 21/05/2002
	 */

	for (;;) {
		struct sha1_cache_entry *cached;

		l = waiting_for_sha1_computation;

		if (!l)
			return NULL;

		waiting_for_sha1_computation = waiting_for_sha1_computation->next;

		cached = (struct sha1_cache_entry *)
			g_hash_table_lookup(sha1_cache, (gconstpointer) l->file_name);

		if (cached) {
			struct stat buf;

			if (-1 == stat(l->file_name, &buf)) {
				g_warning("ignoring SHA1 recomputation request for \"%s\": %s",
					l->file_name, g_strerror(errno));
				free_cell(l);
				continue;
			}

			if (cached->size == buf.st_size && cached->mtime == buf.st_mtime) {
				if (dbg > 1)
					printf("ignoring duplicate SHA1 work for \"%s\"\n",
						l->file_name);
				free_cell(l);
				continue;
			}
		}

		return l;
	}
}

/* 
 * open_next_file
 * 
 * Open the next file waiting for its hash to be computed.
 * 
 * Returns TRUE if open succeeded, FALSE otherwise.
 */
static gboolean open_next_file(struct sha1_computation_context *ctx)
{
	ctx->file = get_next_file_from_list();

	if (!ctx->file)
		return FALSE;			/* No more file to process */

	if (dbg > 1) {
		printf("Computing SHA1 digest for %s\n", ctx->file->file_name);
		ctx->start = time((time_t *) NULL);
	}

	ctx->fd = file_open(ctx->file->file_name, O_RDONLY);

	if (ctx->fd < 0) {
		g_warning("Unable to open \"%s\" for computing SHA1 hash\n",
			ctx->file->file_name);
		close_current_file(ctx);
		return FALSE;
	}

	SHA1Reset(&ctx->context);

	return TRUE;
}

/*
 * got_sha1_result
 * 
 * Callback to be called when a computation has completed.
 */
static void got_sha1_result(struct sha1_computation_context *ctx, char *digest)
{
	struct shared_file *sf;

	g_assert(ctx->magic == SHA1_MAGIC);
	g_assert(ctx->file != NULL);

	sf = shared_file(ctx->file->file_index);

	if (sf == SHARE_REBUILDING) {
		/*
		 * We can't retrofit SHA1 hash into shared_file now, because we can't
		 * get the shared_file yet.
		 */

		copy_sha1(ctx->file->sha1_digest, digest);

		/* Re-use the record to save some time and heap fragmentation */

		push(&waiting_for_library_build_complete, ctx->file);
		ctx->file = NULL;
	} else
		put_sha1_back_into_share_library(sf, ctx->file->file_name, digest);
}

/*
 * sha1_timer_one_step
 * 
 * The timer calls repeatedly this function, consuming one unit of
 * credit every call.
 */
static void sha1_timer_one_step(
	struct sha1_computation_context *ctx, gint ticks, gint *used)
{
	ssize_t r;
	int res;
	gint amount;

	if (!ctx->file && !open_next_file(ctx)) {
		*used = 1;
		return;
	}

	/*
	 * Each tick we have can buy us 2^HASH_BLOCK_SHIFT bytes.
	 * We read into a HASH_BUF_SIZE bytes buffer.
	 */

	amount = ticks << HASH_BLOCK_SHIFT;
	amount = MIN(amount, HASH_BUF_SIZE);

	r = read(ctx->fd, ctx->buffer, amount);

	if (r < 0) {
		g_warning("Error while reading %s for computing SHA1 hash: %s\n",
			ctx->file->file_name, g_strerror(errno));
		close_current_file(ctx);
		*used = 1;
		return;
	}

	/*
	 * Any partially read block counts as one block, hence the second term.
	 */

	*used = (r >> HASH_BLOCK_SHIFT) +
		((r & ((1 << HASH_BLOCK_SHIFT) - 1)) ? 1 : 0);

	if (r > 0) {
		res = SHA1Input(&ctx->context, (guint8 *) ctx->buffer, r);
		if (res != shaSuccess) {
			g_warning("SHA1 error while computing hash for %s\n",
				ctx->file->file_name);
			close_current_file(ctx);
			return;
		}
	}

	if (r < amount) {					/* EOF reached */
		guint8 digest[SHA1HashSize];
		SHA1Result(&ctx->context, digest);
		got_sha1_result(ctx, (gchar *) digest);
		close_current_file(ctx);
	}
}

/* 
 * sha1_step_compute
 * 
 * The routine doing all the work.
 */
static bgret_t sha1_step_compute(gpointer h, gpointer u, gint ticks)
{
	gboolean call_again;
	struct sha1_computation_context *ctx =
		(struct sha1_computation_context *) u;
	gint credit = ticks;

	g_assert(ctx->magic == SHA1_MAGIC);

	if (dbg > 4)
		printf("sha1_step_compute: ticks = %d\n", ticks);

	while (credit > 0) {
		gint used;
		if (!ctx->file && !waiting_for_sha1_computation)
			break;
		sha1_timer_one_step(ctx, credit, &used);
		credit -= used;
	}

	/*
	 * If we didn't use all our credit, tell the background task scheduler.
	 */

	if (credit > 0)
		bg_task_ticks_used(h, ticks - credit);

	if (dbg > 4)
		printf("sha1_step_compute: file=0x%lx [#%d], wait_comp=0x%lx [#%d], "
			"wait_lib=0x%lx [#%d]\n",
			(gulong) ctx->file, ctx->file ? ctx->file->file_index : 0,
			(gulong) waiting_for_sha1_computation,
			waiting_for_sha1_computation ?
				waiting_for_sha1_computation->file_index : 0,
			(gulong) waiting_for_library_build_complete,
			waiting_for_library_build_complete ?
				waiting_for_library_build_complete->file_index : 0);

	if (waiting_for_library_build_complete) 
		try_to_put_sha1_back_into_share_library();

	call_again = ctx->file
		|| waiting_for_sha1_computation
		|| waiting_for_library_build_complete;

	if (!call_again) {
		if (dbg > 1)
			printf("sha1_step_compute: was last call for now\n");
		sha1_task = NULL;
		gnet_prop_set_boolean_val(PROP_SHA1_REBUILDING, FALSE);
		return BGR_NEXT;
	}

	return BGR_MORE;
}

/*
 * sha1_step_dump
 *
 * Dump SHA1 cache if it is dirty.
 */
static bgret_t sha1_step_dump(gpointer h, gpointer u, gint ticks)
{
	if (cache_dirty)
		dump_cache();

	return BGR_DONE;			/* Finished */
}

/**
 ** External interface
 **/

/* This is the external interface. During the share library building,
 * computation of SHA1 values for shared_file is repeatedly requested
 * through sha1_set_digest. If the value is found in the cache (and
 * the cache is up to date), it's set immediately. Otherwise, the file
 * is put in a queue for it's SHA1 digest to be computed.
 */

/* 
 * queue_shared_file_for_sha1_computation
 * 
 * Put the file with a given file_index and file_name on the stack of the things
 * to do. Activate the timer if this wasn't done already.
 */
static void queue_shared_file_for_sha1_computation(
	guint32 file_index,
	const char *file_name)
{
	struct file_sha1 *new_cell = g_malloc(sizeof(struct file_sha1));

	new_cell->file_name = atom_str_get(file_name);
	new_cell->file_index = file_index;
	push(&waiting_for_sha1_computation, new_cell);

	if (sha1_task == NULL) {
		struct sha1_computation_context *ctx;
		bgstep_cb_t steps[] = {
			sha1_step_compute,
			sha1_step_dump,
		};

		ctx = walloc0(sizeof(*ctx));
		ctx->magic = SHA1_MAGIC;
		ctx->fd = -1;
		ctx->buffer = g_malloc(HASH_BUF_SIZE);

		sha1_task = bg_task_create("SHA1 computation",
			steps, 2,  ctx, sha1_computation_context_free, NULL, NULL);

		gnet_prop_set_boolean_val(PROP_SHA1_REBUILDING, TRUE);
	}
}

/* 
 * cached_entry_up_to_date
 * 
 * Check to see if an (in-memory) entry cache is up to date.
 * Returns true (in the C sense) if it is, or false otherwise.
 */
static gboolean cached_entry_up_to_date(
	const struct sha1_cache_entry *cache_entry,
	const struct shared_file *sf)
{
	return cache_entry->size == sf->file_size
		&& cache_entry->mtime == sf->mtime;
}

/* 
 * requested_sha1
 * 
 * External interface to call for getting the hash for a shared_file.
 */
void request_sha1(struct shared_file *sf)
{
	struct sha1_cache_entry *cached_sha1;

	cached_sha1 = (struct sha1_cache_entry *)
		g_hash_table_lookup(sha1_cache, (gconstpointer) sf->file_path);

	if (cached_sha1 && cached_entry_up_to_date(cached_sha1, sf)) {
		set_sha1(sf, cached_sha1->digest);
		cached_sha1->shared = TRUE;
		return;
	}

	queue_shared_file_for_sha1_computation(sf->file_index, sf->file_path);
}

/**
 ** Init
 **/

/*
 * huge_init
 *
 * Initialize SHA1 module
 */
void huge_init(void)
{
	sha1_cache = g_hash_table_new(g_str_hash, g_str_equal);
	sha1_read_cache();
}

/*
 * cache_free_entry
 *
 * Free SHA1 cache entry.
 */
static gboolean cache_free_entry(gpointer k, gpointer v, gpointer udata)
{
	struct sha1_cache_entry *e = (struct sha1_cache_entry *) v;

	atom_str_free(e->file_name);
	G_FREE_NULL(e);

	return TRUE;
}

/*
 * huge_close
 *
 * Called when servent is shutdown.
 */
void huge_close(void)
{
	if (sha1_task)
		bg_task_cancel(sha1_task);

	if (cache_dirty)
		dump_cache();

	if (persistent_cache_file_name)
		G_FREE_NULL(persistent_cache_file_name);

	g_hash_table_foreach_remove(sha1_cache, cache_free_entry, NULL);
	g_hash_table_destroy(sha1_cache);

	while (waiting_for_sha1_computation) {
		struct file_sha1 *l = waiting_for_sha1_computation;

		waiting_for_sha1_computation = waiting_for_sha1_computation->next;
		free_cell(l);
	}
}

/*
 * huge_http_sha1_extract32
 *
 * Validate SHA1 starting in NUL-terminated `buf' as a proper base32 encoding
 * of a SHA1 hash, and write decoded value in `retval'.
 *
 * The SHA1 typically comes from HTTP, in a X-Gnutella-Content-URN header.
 * Therefore, we unconditionally accept both old and new encodings.
 *
 * Returns TRUE if the SHA1 was valid and properly decoded, FALSE on error.
 */
gboolean huge_http_sha1_extract32(gchar *buf, gchar *retval)
{
	gint i;
	const gchar *p;

	/*
	 * Make sure we have at least SHA1_BASE32_SIZE characters before the
	 * end of the string.
	 */

	for (p = buf, i = 0; *p && i < SHA1_BASE32_SIZE; p++, i++)
		/* empty */;

	if (i < SHA1_BASE32_SIZE)
		goto invalid;

	if (base32_decode_into(buf, SHA1_BASE32_SIZE, retval, SHA1_RAW_SIZE))
		return TRUE;

	/*
	 * When extracting SHA1 from HTTP headers, we want to get the proper
	 * hash value: some servents were deployed with the old base32 encoding
	 * (using the digits 8 and 9 in the alphabet instead of letters L and O
	 * in the new one).
	 *
	 * Among the 32 groups of 5 bits, equi-probable, there is a 2/32 chance
	 * of having a 8 or a 9 encoded in the old alphabet.  Therefore, the
	 * probability of not having a 8 or a 9 in the first letter is 30/32.
	 * The probability of having no 8 or 9 in the 32 letters is (30/32)^32.
	 * So the probability of having at least an 8 or a 9 is 1-(30/32)^32,
	 * which is 87.32%.
	 */
	
	if (base32_decode_old_into(buf, SHA1_BASE32_SIZE, retval, SHA1_RAW_SIZE))
		return TRUE;

invalid:
	g_warning("ignoring invalid SHA1 base32 encoding: %s", buf);

	return FALSE;
}

/*
 * huge_sha1_extract32
 *
 * Validate `len' bytes starting from `buf' as a proper base32 encoding
 * of a SHA1 hash, and write decoded value in `retval'.
 *
 * `header' is the header of the packet where we found the SHA1, so that we
 * may trace errors if needed.
 *
 * When `check_old' is true, check the encoding against an earlier version
 * of the base32 alphabet.
 *
 * Returns TRUE if the SHA1 was valid and properly decoded, FALSE on error.
 */
gboolean huge_sha1_extract32(gchar *buf, gint len, gchar *retval,
	gpointer header, gboolean check_old)
{
	if (len != SHA1_BASE32_SIZE) {
		if (dbg) {
			g_warning("%s has bad SHA1 (len=%d)", gmsg_infostr(header), len);
			if (len)
				dump_hex(stderr, "Base32 SHA1", buf, len);
		}
		return FALSE;
	}

	if (base32_decode_into(buf, len, retval, SHA1_RAW_SIZE))
		return TRUE;

	if (!check_old) {
		if (dbg) {
			if (base32_decode_old_into(buf, len, retval, SHA1_RAW_SIZE))
				g_warning("%s old SHA1: %32s", gmsg_infostr(header), buf);
			else
				g_warning("%s bad SHA1: %32s", gmsg_infostr(header), buf);
		}
		return FALSE;
	}

	if (base32_decode_old_into(buf, len, retval, SHA1_RAW_SIZE))
		return TRUE;

	if (dbg) {
		g_warning("%s bad SHA1: %32s", gmsg_infostr(header), buf);
		dump_hex(stderr, "Base32 SHA1", buf, len);
	}

	return FALSE;
}

/*
 * huge_extract_sha1
 *
 * Locate the start of "urn:sha1:" or "urn:bitprint:" indications and extract
 * the SHA1 out of it, placing it in the supplied `digest' buffer.
 *
 * Returns whether we successfully extracted the SHA1.
 */
gboolean huge_extract_sha1(gchar *buf, gchar *digest)
{
	gchar *sha1;

	/*
	 * We handle both "urn:sha1:" and "urn:bitprint:".  In the latter case,
	 * the first 32 bytes of the bitprint is the SHA1.
	 */

	sha1 = strcasestr(buf, "urn:sha1:");		/* Case-insensitive */
	
	if (sha1) {
		sha1 += 9;		/* Skip "urn:sha1:" */
		if (huge_http_sha1_extract32(sha1, digest))
			return TRUE;
	}

	sha1 = strcasestr(buf, "urn:bitprint:");	/* Case-insensitive */

	if (sha1) {
		sha1 += 13;		/* Skip "urn:bitprint:" */
		if (huge_http_sha1_extract32(sha1, digest))
			return TRUE;
	}

	return FALSE;
}

/*
 * huge_extract_sha1_no_urn
 *
 * This is the same as huge_extract_sha1(), only the leading "urn:" part
 * is missing (typically a URN embedded in a GGEP "u").
 *
 * `buf' MUST start with "sha1:" or "bitprint:" indications.  Since the
 * leading "urn:" part is missing, we cannot be lenient.
 *
 * Extract the SHA1 out of it, placing it in the supplied `digest' buffer.
 *
 * Returns whether we successfully extracted the SHA1.
 */
gboolean huge_extract_sha1_no_urn(gchar *buf, gchar *digest)
{
	gchar *sha1;

	/*
	 * We handle both "sha1:" and "bitprint:".  In the latter case,
	 * the first 32 bytes of the bitprint is the SHA1.
	 */

	sha1 = strcasestr(buf, "sha1:");			/* Case-insensitive */
	
	if (sha1 && sha1 == buf) {
		sha1 += 5;		/* Skip "sha1:" */
		if (huge_http_sha1_extract32(sha1, digest))
			return TRUE;
	}

	sha1 = strcasestr(buf, "bitprint:");		/* Case-insensitive */

	if (sha1 && sha1 == buf) {
		sha1 += 9;		/* Skip "bitprint:" */
		if (huge_http_sha1_extract32(sha1, digest))
			return TRUE;
	}

	return FALSE;
}

/*
 * huge_collect_locations
 *
 * Parse the "X-Gnutella-Alternate-Location" header if present to learn
 * about other sources for this file.
 *
 * Since 05/10/2003, we also parse the new compact "X-Alt" header which
 * is a more compact representation of alternate locations.  We also remember
 * which vendor gave us X-Alt locations, so that we can emit back X-Alt to
 * them the next time instead of the longer X-Gnutella-Alternate-Location.
 */
void huge_collect_locations(gchar *sha1, header_t *header, const gchar *vendor)
{
	gchar *alt = header_get(header, "X-Gnutella-Alternate-Location");

	/*
	 * Unfortunately, clueless people broke the HUGE specs and made up their
	 * own headers.  They should learn about header continuations, and
	 * that "X-Gnutella-Alternate-Location" does not need to be repeated.
	 */

	if (alt == NULL)
		alt = header_get(header, "Alternate-Location");
	if (alt == NULL)
		alt = header_get(header, "Alt-Location");

	if (alt) {
		dmesh_collect_locations(sha1, alt, TRUE);
		return;
	}

	alt = header_get(header, "X-Alt");

	if (alt) {
		dmesh_collect_compact_locations(sha1, alt);
    }
}

/* 
 * Emacs stuff:
 * Local Variables: ***
 * c-indentation-style: "bsd" ***
 * fill-column: 80 ***
 * tab-width: 4 ***
 * indent-tabs-mode: nil ***
 * End: ***
 */
