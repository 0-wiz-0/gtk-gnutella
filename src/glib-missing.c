/*
 * $Id$
 *
 * Copyright (c) 2003, Raphael Manfredi
 *
 * Functions that should be in glib-1.2 but are not.
 * They are all prefixed with "gm_" as in "Glib Missing".
 *
 * We also include FIXED versions of glib-1.2 routines that are broken
 * and make sure those glib versions are never called directly.
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
#include "glib-missing.h"
#include "gnet_property.h"
#include "gnet_property_priv.h"

#include "override.h"		/* Must be the last header included */

RCSID("$Id$");

#ifndef TRACK_MALLOC

/*
 * gm_slist_insert_after
 *
 * Insert `item' after `lnk' in list `list'.
 * If `lnk' is NULL, insertion happens at the head.
 *
 * Returns new list head.
 */
GSList *gm_slist_insert_after(GSList *list, GSList *lnk, gpointer data)
{
	GSList *new;

	g_assert(list != NULL || lnk == NULL);	/* (list = NULL) => (lnk = NULL) */

	if (lnk == NULL)
		return g_slist_prepend(list, data);

	new = g_slist_alloc();
	new->data = data;

	new->next = lnk->next;
	lnk->next = new;

	return list;
}

/*
 * gm_list_insert_after
 *
 * Insert `item' after `lnk' in list `list'.
 * If `lnk' is NULL, insertion happens at the head.
 *
 * Returns new list head.
 */
GList *gm_list_insert_after(GList *list, GList *lnk, gpointer data)
{
	GList *new;

	g_assert(list != NULL || lnk == NULL);	/* (list = NULL) => (lnk = NULL) */

	if (lnk == NULL)
		return g_list_prepend(list, data);

	new = g_list_alloc();
	new->data = data;

	new->prev = lnk;
	new->next = lnk->next;

	if (lnk->next)
		lnk->next->prev = new;

	lnk->next = new;

	return list;
}

#ifndef USE_GTK2
GList *g_list_delete_link(GList *l, GList *lnk)
{
	GList *new;

	new = g_list_remove_link(l, lnk);
	g_list_free_1(lnk);
	return new;
}
#endif /* !USE_GTK2 */

#endif /* !TRACK_MALLOC */

/*
 * DO_VSNPRINTF
 *
 * Perform the vsnprintf() operation for the gm_vsnprintf() and gm_snprintf()
 * routines.
 *
 * We don't use macro arguments on purpose: instead, we hardwire the following
 * that must be provided by the context, to ensure this macro is not reused
 * out of its original intended context.
 *
 * `retval' is the returned value.
 * `str' is the string where printing is done.
 * `n' is the maximum amount of chars that can be held in `str'.
 * `fmt' is the format string.
 * `args' is the arguments to be printed.
 */

#ifdef	HAS_VSNPRINTF
#define DO_VSNPRINTF() do {					\
	str[0] = '\0';							\
	retval = vsnprintf(str, n, fmt, args);	\
	if (retval < 0) {						\
		/* Old versions of vsnprintf() */ 	\
		str[n - 1] = '\0';					\
		retval = strlen(str);				\
	} else if ((size_t) retval >= n) {		\
	/* New versions (compliant with C99) */ \
		str[n - 1] = '\0';					\
		retval = n - 1;						\
	}										\
} while (0)
#else	/* !HAVE_VSNPRINTF */
#define DO_VSNPRINTF() do {							\
	gchar *printed = g_strdup_vprintf(fmt, args);	\
	size_t l = g_strlcpy(str, printed, n);			\
	retval = MIN((n - 1), l);						\
	G_FREE_NULL(printed);							\
} while (0)
#endif	/* HAVE_VSNPRINTF */


/*
 * gm_vsnprintf
 *
 * This version implements the correct FIXED semantics of the 1.2.10 glib:
 */
size_t gm_vsnprintf(gchar *str, size_t n, gchar const *fmt, va_list args)
{
	ssize_t retval;

	g_return_val_if_fail (str != NULL, 0);
	g_return_val_if_fail (fmt != NULL, 0);
	g_return_val_if_fail ((ssize_t) n > 0, 0);
	g_return_val_if_fail (n <= (size_t) INT_MAX, 0);

	DO_VSNPRINTF();

	g_assert((size_t) retval < n);

	return retval;
}

/*
 * gm_snprintf
 *
 * This version implements the correct FIXED semantics of the 1.2.10 glib:
 * It returns the length of the output string, and it is GUARANTEED to
 * be one less than `n' (last position occupied by the trailing NUL).
 */
size_t gm_snprintf(gchar *str, size_t n, gchar const *fmt, ...)
{
	va_list args;
	ssize_t retval;

	g_return_val_if_fail (str != NULL, 0);
	g_return_val_if_fail (fmt != NULL, 0);
	g_return_val_if_fail ((ssize_t) n > 0, 0);
	g_return_val_if_fail (n <= (size_t) INT_MAX, 0);

	va_start (args, fmt);
	DO_VSNPRINTF();
	va_end (args);

	g_assert((size_t) retval < n);

	return retval;
}

/*
 * gm_strconcatv
 *
 * The vectorized version of g_strconcat().
 */
gchar *gm_strconcatv(const gchar *s, va_list args)
{
	gchar *res;
	gchar *add;
	gint size;

	size = strlen(s) + 1;
	res = g_malloc(size);
	memcpy(res, s, size);

	while ((add = va_arg(args, gchar *))) {
		gint len = strlen(add);
		res = g_realloc(res, size + len);
		memcpy(res + size - 1, add, len + 1);	/* Includes trailing NULL */
		size += len;
	}

	return res;
}

static gint orig_argc;
static gchar **orig_argv;
static gchar **orig_env;

/*
 * gm_savemain
 *
 * Save the original main() arguments.
 */
void gm_savemain(gint argc, gchar **argv, gchar **env)
{
	orig_argc = argc;
	orig_argv = argv;
	orig_env = env;
}

/*
 * gm_setproctitle
 *
 * Change the process title as seen by "ps".
 */
void gm_setproctitle(gchar *title)
{
	static gint sysarglen = 0;		/* Length of the exec() arguments */
	gint tlen;
	gint i;

	/*
	 * Compute the length of the exec() arguments that were given to us.
	 */

	if (sysarglen == 0) {
		gchar *s = orig_argv[0];

		s += strlen(s) + 1;			/* Go past trailing NUL */

		/*
		 * Let's see whether all the argv[] arguments were contiguous.
		 */

		for (i = 1; i < orig_argc; i++) {
			if (orig_argv[i] != s)
				break;
			s += strlen(s) + 1;		/* Yes, still contiguous */
		}

		/*
		 * Maybe the environment is contiguous as well...
		 */

		for (i = 0; orig_env[i] != NULL; i++) {
			if (orig_env[i] != s)
				break;
			s += strlen(s) + 1;		/* Yes, still contiguous */
		}

		sysarglen = s - orig_argv[0] - 1;	/* -1: leave room for NUL */

		if (lib_debug)
			g_warning("exec() args used %d contiguous bytes", sysarglen + 1);
	}

	tlen = strlen(title);
	
	if (tlen >= sysarglen) {		/* If too large, needs truncation */
		memcpy(orig_argv[0], title, sysarglen);
		(orig_argv[0])[sysarglen] = '\0';
	} else {
		memcpy(orig_argv[0], title, tlen + 1);	/* Copy trailing NUL */
		if (tlen + 1 < sysarglen)
			memset(orig_argv[0] + tlen + 1, ' ', sysarglen - tlen - 1);
	}

	/*
	 * Scrap references to the arguments.
	 */

	for (i = 1; i < orig_argc; i++)
		orig_argv[i] = NULL;
}

/*
 * gm_atoul
 *
 * Returns the nul-terminated string `str' converted to an unsigned long.
 * If successful `errorcode' will be set to 0 (zero), otherwise it will
 * contain an errno(2) code and the function returns 0 (zero).
 * If endptr is not NULL it will point to the first invalid character.
 * See strtoul(3) for more details about valid and invalid inputs. 
 */
unsigned long gm_atoul(const char *str, char **endptr, int *errorcode)
{
	char *ep;
	unsigned long ret;
	int old_errno = errno;

	g_assert(NULL != str);
	g_assert(NULL != errorcode);

	errno = 0;
	ret = strtoul(str, &ep, 10);
	if (str == ep) {
		*errorcode = EINVAL;
		ret = 0;
	} else {
		if (0 != errno) {
			*errorcode = ERANGE;
			ret = 0;
		} else
			*errorcode = 0;
	}

	if (NULL != endptr)
		*endptr = ep;
	errno = old_errno;
	return ret;
}

/**
 * Creates a valid and sanitized filename from the supplied string. This is
 * necessary for platforms which have certain requirements for filenames. For
 * most Unix-like platforms anything goes but for security reasons, shell
 * meta characters are replaced by harmless characters.
 *
 * @param filename the suggested filename.
 *
 * @returns a newly allocated string or ``filename'' if it was a valid filename
 *		    already.
 */
gchar *
gm_sanitize_filename(const gchar *filename)
{
	gint c;
	gchar *q = NULL, *r;
	const gchar *p, *s = filename;
	gboolean no_spaces;
	
	g_assert(filename != NULL);

	gnet_prop_get_boolean_val(PROP_CONVERT_SPACES, &no_spaces);
	
#if defined(__APPLE__) && defined(__MACH__) /* Mac OS X*/
	s = locale_to_utf8_nfd(filename, len);
	q = s;
#endif /* Mac OS X */

	/* Replace shell meta characters and likely problematic characters */
	for (p = s; (c = *(guchar *) p) != '\0'; ++p) {
		static const gchar evil[] = "$&*/\\`:;<>?|~\177";
		
		if (c < 32
				|| is_ascii_cntrl(c)
				|| c == G_DIR_SEPARATOR
				|| (c == ' ' && no_spaces)
				|| (p == s && c == '.')
				|| NULL != strchr(evil, c)
		) {
			if (!q) {
				q = g_strdup(s);
			}
			q[p - s] = '_';
		}
	}

	return q ? q : (gchar *) s; /* Override const */
}

/* vi: set ts=4 sw=4 cindent: */
