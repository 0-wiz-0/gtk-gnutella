/*
 * $Id$
 *
 * Copyright (c) 2008-2011, Raphael Manfredi
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
 * @ingroup lib
 * @file
 *
 * Security tokens.
 *
 * @author Raphael Manfredi
 * @date 2008-2011
 */

#include "common.h"

RCSID("$Id$")

#include "sectoken.h"
#include "atoms.h"			/* For binary_hash() */
#include "cq.h"
#include "endian.h"
#include "host_addr.h"
#include "random.h"
#include "tea.h"
#include "unsigned.h"
#include "walloc.h"
#include "override.h"		/* Must be the last header included */

enum sectoken_gen_magic { SECTOKEN_GEN_MAGIC = 0x2a7f3219 };

/**
 * Security token generator.
 */
struct sectoken_gen {
	enum sectoken_gen_magic magic;
	tea_key_t *keys;			/**< Rotating set of keys */
	size_t keycnt;				/**< Amount of keys in the keys[] array */
	cevent_t *rotate_ev;		/**< Rotate event */
	time_delta_t refresh;		/**< Refresh period in seconds */
};

static inline void
sectoken_gen_check(const sectoken_gen_t * const stg)
{
	g_assert(stg != NULL);
	g_assert(SECTOKEN_GEN_MAGIC == stg->magic);
	g_assert(stg->keys != NULL);
	g_assert(size_is_positive(stg->keycnt));
}

/**
 * @return lifetime in seconds of the security tokens we generate.
 */
time_delta_t
sectoken_lifetime(const sectoken_gen_t *stg)
{
	sectoken_gen_check(stg);

	return stg->refresh * stg->keycnt;
}

/**
 * Create a security token from host address and port using specified key.
 *
 * @param stg		the security token generator
 * @param n			key index to use
 * @param tok		where security token is written
 * @param addr		address of the host for which we're generating a token
 * @param port		port of the host for which we're generating a token
 */
static void
sectoken_generate_n(sectoken_gen_t *stg, size_t n,
	sectoken_t *tok, host_addr_t addr, guint16 port)
{
	char block[8];
	char enc[8];
	char *p = block;

	sectoken_gen_check(stg);
	g_assert(tok != NULL);
	g_assert(size_is_non_negative(n));
	g_assert(n < stg->keycnt);

	switch (host_addr_net(addr)) {
	case NET_TYPE_IPV4:
		p = poke_be32(p, host_addr_ipv4(addr));
		break;
	case NET_TYPE_IPV6:
		{
			guint val;

			val = binary_hash(host_addr_ipv6(&addr), 16);
			p = poke_be32(p, val);
		}
		break;
	case NET_TYPE_LOCAL:
	case NET_TYPE_NONE:
		g_error("unexpected address for security token generation: %s",
			host_addr_to_string(addr));
	}

	p = poke_be16(p, port);
	p = poke_be16(p, 0);		/* Filler */

	g_assert(p == &block[8]);

	STATIC_ASSERT(sizeof(tok->v) == sizeof(guint32));
	STATIC_ASSERT(sizeof(block) == sizeof(enc));

	tea_encrypt(&stg->keys[n], enc, block, sizeof block);
	poke_be32(tok->v, tea_squeeze(enc, sizeof enc));
}

/**
 * Create a security token from host address and port.
 *
 * @param stg		the security token generator
 * @param tok		where security token is written
 * @param addr		address of the host for which we're generating a token
 * @param port		port of the host for which we're generating a token
 */
void
sectoken_generate(sectoken_gen_t *stg,
	sectoken_t *tok, host_addr_t addr, guint16 port)
{
	sectoken_generate_n(stg, 0, tok, addr, port);
}

/*
 * Is specified token still valid for this address/port tuple?
 */
gboolean
sectoken_is_valid(sectoken_gen_t *stg,
	const sectoken_t *tok, host_addr_t addr, guint16 port)
{
	size_t i;

	sectoken_gen_check(stg);
	g_assert(tok != NULL);

	/*
	 * We can't decrypt, we just generate a new token with the set of
	 * keys and say the token is valid if it matches with the one we're
	 * generating.
	 *
	 * We try the most recent key first as it is the most likely to succeed.
	 */

	for (i = 0; i < stg->keycnt; i++) {
		sectoken_t gen;

		sectoken_generate_n(stg, i, &gen, addr, port);
		if (0 == memcmp(gen.v, tok->v, sizeof(tok->v)))
			return TRUE;
	}

	return FALSE;
}

/**
 * Token key rotating event.
 */
static void
sectoken_rotate(cqueue_t *unused_cq, gpointer obj)
{
	size_t i;
	sectoken_gen_t *stg = obj;

	sectoken_gen_check(stg);

	(void) unused_cq;

	stg->rotate_ev = cq_main_insert(stg->refresh * 1000, sectoken_rotate, stg);

	for (i = 0; i < stg->keycnt - 1; i++)
		stg->keys[i + 1] = stg->keys[i];

	/* 0 is most recent key */
	random_bytes(&stg->keys[0], sizeof(stg->keys[0]));
}

/**
 * Allocate a remote security token.
 */
sectoken_remote_t *
sectoken_remote_alloc(guint8 length)
{
	sectoken_remote_t *token;

	WALLOC(token);
	token->length = length;
	token->v = length ? walloc(length) : NULL;

	return token;
}

/**
 * Free remote security token.
 */
void
sectoken_remote_free(sectoken_remote_t *token, gboolean freedata)
{
	if (token->v && freedata)
		wfree(token->v, token->length);

	WFREE(token);
}

/**
 * Create a new security token generator.
 */
sectoken_gen_t *
sectoken_gen_new(size_t keys, time_delta_t refresh)
{
	sectoken_gen_t *stg;
	size_t i;

	g_assert(size_is_positive(keys));

	WALLOC0(stg);
	stg->magic = SECTOKEN_GEN_MAGIC;
	stg->keys = walloc(keys * sizeof stg->keys[0]);
	stg->keycnt = keys;
	stg->refresh = refresh;

	for (i = 0; i < stg->keycnt; i++)
		random_bytes(&stg->keys[i], sizeof(stg->keys[0]));

	stg->rotate_ev = cq_main_insert(refresh * 1000, sectoken_rotate, stg);

	return stg;
}

/**
 * Destroy the security token generator and nullify its pointer.
 */
void
sectoken_gen_free_null(sectoken_gen_t **stg_ptr)
{
	sectoken_gen_t *stg = *stg_ptr;

	if (stg != NULL) {
		sectoken_gen_check(stg);

		cq_cancel(&stg->rotate_ev);
		WFREE_NULL(stg->keys, stg->keycnt * sizeof stg->keys[0]);
		stg->magic = 0;
		WFREE(stg);
		*stg_ptr = NULL;
	}
}

/* vi: set ts=4 sw=4 cindent: */
