/*
 * $Id$
 *
 * Copyright (c) 2003, Raphael Manfredi
 *
 * Token management.
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

#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>	/* For ntohl(), htonl() */

#include "common.h"
#include "token.h"
#include "misc.h"
#include "sha1.h"
#include "base64.h"
#include "crc.h"
#include "clock.h"

#include "version.h"
#include "override.h"		/* Must be the last header included */

RCSID("$Id$");

#define TOKEN_CLOCK_SKEW	3600		/* +/- 1 hour */
#define TOKEN_LIFE			60			/* lifetime of our tokens */
#define TOKEN_BASE64_SIZE	(TOKEN_VERSION_SIZE * 4 / 3)	/* base64 size */
#define LEVEL_SIZE			(2 * G_N_ELEMENTS(token_keys))	/* at most */
#define LEVEL_BASE64_SIZE	(LEVEL_SIZE * 4 / 3 + 3)		/* +2 for == tail */

/*
 * Keys are generated through "od -x /dev/random".
 * There can be up to 2^5 = 32 keys per version.
 */

static const gchar *keys_092c[] = {
	"0d69 54ec e06a 47c4 ec25 cb35 4f3a ec74",
	"c80f 10cd fbd6 85a9 69ef e724 c519 2997",
	"05e4 401f fd79 0e8e def5 12d6 80a9 53b7",
	"f7f5 ae0b 2649 1441 eab4 562f 9509 c4b7",
	"811e 301f 23d0 7e71 017e d449 6c8c 232f",
	"44f1 2a2b d2da 2313 17df 1a21 635f dea2",
	"200e 7cfe 35fa 5a6a 47fc f79e 81c6 e11c",
	"1f7d 541d 1193 4d44 bd84 fdd6 7659 2573",
	"5db1 b96a 2961 7c83 c254 b19d 75dd 1844",
	"72ff 61c8 8553 ddd1 9a32 24cc 88bb 51fd",
	"664d 87d3 1e30 3778 31a2 da87 2e9d f832",
	"c3d9 6801 e69f cf8d d7c7 4f62 9b80 3438",
	"d2fc 0fad 1340 e47a 3f3e b012 18fe 3ad0",
	"2258 65cf 591c dc58 b68a ac2b d174 fe1d",
	"b6a1 7686 c7f7 9e57 d9e8 6c47 e128 d5c0",
	"c545 7424 1b25 e586 1f94 e119 25af 2862",
	"4fb8 1f55 4a5b 2e21 dc48 9fba 7b5c e381",
	"dfe0 c023 06b6 d236 82f6 5732 40d4 492e",
	"93d6 d989 aa52 3ca0 8a69 a79a 424d b7a3",
	"7257 7cff ac09 668f 3b0e 7d6b fe8a 7e7d",
};

static const gchar *keys_092_2[] = {
	"b6c1 2fd1 ae02 409f 7f04 f34a 8076 4283",
	"9232 aa04 652b fa9d 6f48 f3dc b616 ffc1",
	"211d c9d9 1fa4 6e78 335b c094 dd22 9ac7",
	"9f5f df94 730b b366 0d6b f4f9 caed 11a9",
	"3458 9e10 8b23 5977 f5d1 66d7 8bb0 238f",
	"ec9b f865 fade fa12 1930 6f3e 7dde 47a6",
	"8e02 5453 4a37 c4c1 f7ab a611 f454 69aa",
	"e450 8506 06aa 3de9 a7c8 4b09 8427 65d2",
	"5d19 a84f a371 70f4 464d 0ba7 ff51 a93b",
	"fe1c fa92 0cc9 46f1 128a 810c 434c 1568",
	"779d 8c74 99aa 1d50 659e b4cf 47c7 3325",
	"06bb d901 f3e8 0d06 f77a a20c 31fe 0bc8",
	"cbff 3cf3 325b 8fc9 bdfc 7acf 15c1 25a2",
	"f167 81cc be83 60e4 6535 092f ea9d 8ef6",
	"c2ec 27f2 0b30 5155 3cd4 dc8c 5928 2e63",
	"d365 afac 948e ffdc abc1 7687 2850 9d58",
};

static const gchar *keys_093_1[] = {
	"8bd8 5c21 1f38 b433 f6bb 8b9c d3ed cbdb",
	"550c 0a1e d6af ba66 11cb 2e38 348a 2cba",
	"793c 2d05 3eae c7fb 75af 8cc8 5952 cf7b",
	"3af4 5190 0c8c efde acdf e12d 3687 4fc4",
	"515d 09ef a9b4 e53e f60f 4a72 6eaa 371a",
	"f947 8d4b ead0 abae 972a 8d73 e521 f914",
	"72c0 809a 66ec 4979 345b a28f ad46 4179",
	"3b43 49d4 5517 38ea 5ab6 b088 1b79 b603",
	"5cd2 69d4 f187 907e 096c c648 adea c40a",
	"9ce0 f178 3238 905d b831 8f9b 031e adb2",
	"6125 2bce 1b0e c97a d5b8 81ac d808 2369",
	"790f 0ca8 91b9 3d94 86f8 6f1e d3d2 198a",
	"e01a 668f 9749 9037 fdf4 a78c 1db8 4381",
	"a019 5ad1 595e 5b72 7fc9 5aea 1799 89ed",
	"db94 b4c2 6c3d a31e d7e4 8731 0784 1fb8",
	"ee48 01f0 40d7 e57b fd0d d3be 84f8 fbe8",
};

static const gchar *keys_095u[] = {
	"2f46 2dd9 4806 cf8a 9b5c 8aff bcdb 1bcd",
	"5a70 0e24 4924 15b8 6f99 de62 15b6 ea58",
	"2cbe fede 70fb bdf6 1e24 19f0 f656 db55",
	"2b5a 1130 f91f 9c13 9ec3 0d56 6e09 a111",
	"ff90 7a78 9b24 cb34 71d3 32e5 3541 d5af",
	"bc2d cb6c 4bd2 c3c6 a3f8 7b33 32cf 2d46",
	"4234 cc41 ca94 cf18 e8f0 6f7a 0379 13a9",
	"102a 6c09 a835 454d 2fda f279 a3a2 5d10",
	"54dd 2ff7 52f2 6bfc 4cc9 1b97 ef05 10e3",
	"372f 124b 40ef 8812 b418 4dfc 4643 0007",
	"5cc9 cd6a 5e64 736c 0a13 c900 3508 5136",
	"0008 6978 d45b 81ae 8b69 dd51 d2ff 8743",
	"1060 eba1 2ec2 82af 3128 716f 73d1 46d8",
	"f034 605b 1f54 68c4 5adc 32e3 ff67 358d",
	"903e 5405 ab52 3b5e 672f 7d89 b4d1 595c",
	"83f9 b561 2070 9caf 1b7f 0548 4630 36f6",
	"4680 7381 a8c4 7994 5f22 d8f4 6db5 c89a",
	"292d 4921 f7bb e0c0 5c13 721f 62af 5670",
	"144f 1e7c 0249 3217 936e 24b9 c630 3ee1",
	"969a 39ec 1650 971b 17d2 294b e75c 1872",
};

/* 
 * Describes the keys to use depending on the version.
 */
struct tokkey {
	version_t ver;		/* Version number */
	const gchar **keys;	/* Keys to use */
	guint count;		/* Amount of keys defined */
} token_keys[] = {
	/* Keep this array sorted by increasing timestamp */
	{
		{ 0, 92, 0, 'c', 0, 1053813600 },			/* 25/05/2003 */
		keys_092c, G_N_ELEMENTS(keys_092c),
	},
	{
		{ 0, 92, 2, '\0', 0, 1067209200 },			/* 27/10/2003 */
		keys_092_2, G_N_ELEMENTS(keys_092_2),
	},
	{
		{ 0, 93, 1, '\0', 0, 1072566000 },			/* 28/12/2003 */
		keys_093_1, G_N_ELEMENTS(keys_093_1),
	},
// XXX Keep this out because a bug in 0.94 and 0.93 prevents them from
// XXX validating our level if there are more entries in the level we
// XXX generate compared to the level they can validate.
#if 0	// Until we decide to release 0.95
	{
		{ 0, 95, 0, 'u', 0, 1089756000 },			/* 14/07/2004 */
		keys_095u, G_N_ELEMENTS(keys_095u),
	},
#endif
};

/*
 * Token validation errors.
 */

static const gchar *tok_errstr[] = {
	"OK",							/* TOK_OK */
	"Bad length",					/* TOK_BAD_LENGTH */
	"Bad timestamp",				/* TOK_BAD_STAMP */
	"Bad key index",				/* TOK_BAD_INDEX */
	"Failed checking",				/* TOK_INVALID */
	"Not base64-encoded",			/* TOK_BAD_ENCODING */
	"Keys not found",				/* TOK_BAD_KEYS */
	"Bad version string",			/* TOK_BAD_VERSION */
	"Version older than expected",	/* TOK_OLD_VERSION */
	"Level not base64-encoded",		/* TOK_BAD_LEVEL_ENCODING */
	"Bad level length",				/* TOK_BAD_LEVEL_LENGTH */
	"Level too short",				/* TOK_SHORT_LEVEL */
	"Level mismatch",				/* TOK_INVALID_LEVEL */
	"Missing level",				/* TOK_MISSING_LEVEL */
};

/*
 * tok_strerror
 *
 * Return human-readable error string corresponding to error code `errnum'.
 */
const gchar *tok_strerror(tok_error_t errnum)
{
	if ((gint) errnum < 0 || errnum >= G_N_ELEMENTS(tok_errstr))
		return "Invalid error code";

	return tok_errstr[errnum];
}

/*
 * find_tokkey
 *
 * Based on the timestamp, determine the proper token keys to use.
 * Returns NULL if we cannot locate any suitable keys.
 */
static const struct tokkey *find_tokkey(time_t now)
{
	time_t adjusted = now - VERSION_ANCIENT_BAN;
	const struct tokkey *tk;
	guint i;

	for (i = 0; i < G_N_ELEMENTS(token_keys); i++) {
		tk = &token_keys[i];
		if (tk->ver.timestamp > adjusted)
			return tk;
	}

	return NULL;
}

/*
 * random_key
 *
 * Pickup a key randomly.
 * Returns the key string and the index within the key array into `idx'
 * and the token key structure used in `tkused'.
 */
static const gchar *random_key(
	time_t now, guint *idx, const struct tokkey **tkused)
{
	static gboolean warned = FALSE;
	guint random_idx;
	const struct tokkey *tk;

	tk = find_tokkey(now);

	if (tk == NULL) {
		if (!warned) {
			g_warning("did not find any token key, version too ancient");
			warned = TRUE;
		}

		tk = &token_keys[0];	/* They'll have problems with their token */
	}

	random_idx = random_value(tk->count - 1);
	*idx = random_idx;
	*tkused = tk;

	return tk->keys[random_idx];
}

/*
 * tok_generate
 *
 * Generate new token for given version string.
 */
static gchar *tok_generate(time_t now, const gchar *version)
{
	gchar token[TOKEN_BASE64_SIZE + 1];
	gchar digest[TOKEN_VERSION_SIZE];
	gchar lvldigest[LEVEL_SIZE];
	gchar lvlbase64[LEVEL_BASE64_SIZE + 1];
	const struct tokkey *tk;
	guint idx;
	const gchar *key;
	SHA1Context ctx;
	guint8 seed[3];
	guint32 now32;
	gint lvlsize;
	gint klen;
	gint i;

	/*
	 * Compute token.
	 */

	key = random_key(now, &idx, &tk);
	seed[0] = random_value(0xff);
	seed[1] = random_value(0xff);
	seed[2] = random_value(0xff) & 0xe0;	/* Upper 3 bits only */
	seed[2] |= idx;							/* Has 5 bits for the index */

	now = clock_loc2gmt(now);				/* As close to GMT as possible */

	now32 = (guint32) htonl((guint32) now);
	memcpy(digest, &now32, 4);
	memcpy(digest + 4, &seed, 3);

	SHA1Reset(&ctx);
	SHA1Input(&ctx, (guint8 *) key, strlen(key));
	SHA1Input(&ctx, (guint8 *) digest, 7);
	SHA1Input(&ctx, (guint8 *) version, strlen(version));
	SHA1Result(&ctx, (guint8 *) digest + 7);

	/*
	 * Compute level.
	 */

	lvlsize = G_N_ELEMENTS(token_keys) - (tk - token_keys);
	now32 = crc32_update_crc(0, digest, TOKEN_VERSION_SIZE);
	klen = strlen(tk->keys[0]);

	for (i = 0; i < lvlsize; i++, tk++) {
		guint j;
		guint32 crc = now32;
		const guchar *c = (const guchar *) &crc;

		for (j = 0; j < tk->count; j++)
			crc = crc32_update_crc(crc, tk->keys[j], klen);

		crc = htonl(crc);
		lvldigest[i*2] = c[0] ^ c[1];
		lvldigest[i*2+1] = c[2] ^ c[3];
	}

	/*
	 * Encode into base64.
	 */

	base64_encode_into(digest, TOKEN_VERSION_SIZE, token, TOKEN_BASE64_SIZE);
	token[TOKEN_BASE64_SIZE] = '\0';

	memset(lvlbase64, 0, sizeof(lvlbase64));
	base64_encode_into(lvldigest, 2 * lvlsize, lvlbase64, LEVEL_BASE64_SIZE);

	return g_strconcat(token, "; ", lvlbase64, NULL);
}

/*
 * tok_version
 *
 * Get a version token, base64-encoded.
 * Returns a pointer to static data.
 *
 * NOTE: token versions are only used to identify GTKG servents as such with
 * a higher level of confidence than just reading the version string alone.
 * It is not meant to be used for strict authentication management, since
 * the algorithm and the keys are exposed publicly.
 */
gchar *tok_version(void)
{
	static time_t last_generated = 0;
	static gchar *toklevel = NULL;
	time_t now = time(NULL);

	/*
	 * We don't generate a new token each time, but only every TOKEN_LIFE
	 * seconds.  The clock skew threshold must be greater than twice that
	 * amount, of course.
	 */

	g_assert(TOKEN_CLOCK_SKEW > 2 * TOKEN_LIFE);

	if (now - last_generated < TOKEN_LIFE)
		return toklevel;

	last_generated = now;

	if (toklevel != NULL)
		g_free(toklevel);

	toklevel = tok_generate(now, version_string);

	return toklevel;
}

/*
 * tok_short_version
 *
 * Get a version token for the short version string, base64-encoded.
 * Returns a pointer to static data.
 */
gchar *tok_short_version(void)
{
	static time_t last_generated = 0;
	static gchar *toklevel = NULL;
	time_t now = time(NULL);

	/*
	 * We don't generate a new token each time, but only every TOKEN_LIFE
	 * seconds.  The clock skew threshold must be greater than twice that
	 * amount, of course.
	 */

	g_assert(TOKEN_CLOCK_SKEW > 2 * TOKEN_LIFE);

	if (now - last_generated < TOKEN_LIFE)
		return toklevel;

	last_generated = now;

	if (toklevel != NULL)
		g_free(toklevel);

	toklevel = tok_generate(now, version_short_string);

	return toklevel;
}

/*
 * tok_version_valid
 *
 * Validate a base64-encoded version token `tokenb64' of `len' bytes.
 * The `ip' is given only for clock update operations.
 *
 * Returns error code, or TOK_OK if token is valid.
 */
tok_error_t tok_version_valid(
	const gchar *version, const gchar *tokenb64, gint len, guint32 ip)
{
	time_t now = time(NULL);
	time_t stamp;
	guint32 stamp32;
	const struct tokkey *tk;
	const struct tokkey *rtk;
	guint idx;
	const gchar *key;
	SHA1Context ctx;
	gchar lvldigest[1024];
	gchar token[TOKEN_VERSION_SIZE]; 
	gchar digest[SHA1HashSize];
	version_t rver;
	gchar *end;
	gint toklen;
	gint lvllen;
	gint lvlsize;
	gint klen;
	guint i;
	gchar *c = (gchar *) &stamp32;

	end = strchr(tokenb64, ';');		/* After 25/02/2003 */
	toklen = end ? (end - tokenb64) : len;

	/*
	 * Verify token.
	 */

	if (toklen != TOKEN_BASE64_SIZE)
		return TOK_BAD_LENGTH;

	if (!base64_decode_into(tokenb64, toklen, token, TOKEN_VERSION_SIZE))
		return TOK_BAD_ENCODING;

	memcpy(&stamp32, token, 4);
	stamp = (time_t) ntohl(stamp32);

	/*
	 * Use that stamp, whose precision is TOKEN_LIFE, to update our
	 * clock skew if necessary.
	 */

	clock_update(stamp, TOKEN_LIFE, ip);

	if (ABS(stamp - clock_loc2gmt(now)) > TOKEN_CLOCK_SKEW)
		return TOK_BAD_STAMP;

	tk = find_tokkey(stamp);				/* The keys they used */
	if (tk == NULL)
		return TOK_BAD_KEYS;

	idx = (guchar) token[6] & 0x1f;					/* 5 bits for the index */
	if (idx >= tk->count)
		return TOK_BAD_INDEX;

	key = tk->keys[idx];

	SHA1Reset(&ctx);
	SHA1Input(&ctx, (guint8 *) key, strlen(key));
	SHA1Input(&ctx, (guint8 *) token, 7);
	SHA1Input(&ctx, (guint8 *) version, strlen(version));
	SHA1Result(&ctx, (guint8 *) digest);

	if (0 != memcmp(token + 7, digest, SHA1HashSize))
		return TOK_INVALID;

	if (!version_fill(version, &rver))		/* Remote version */
		return TOK_BAD_VERSION;

	if (version_cmp(&rver, &tk->ver) < 0)
		return TOK_OLD_VERSION;

	/*
	 * Verify level.
	 */

	if (end == NULL) {						/* No level */
		if (rver.timestamp >= 1046127600)	/* 25/02/2003 */
			return TOK_MISSING_LEVEL;
		return TOK_OK;
	}

	lvllen = len - toklen - 2;				/* Forget about "; " */
	end += 2;								/* Skip "; " */

	if (lvllen >= (gint) sizeof(lvldigest) || lvllen <= 0)
		return TOK_BAD_LEVEL_LENGTH;

	if (lvllen & 0x3)
		return TOK_BAD_LEVEL_LENGTH;

	lvllen = base64_decode_into(end, lvllen, lvldigest, sizeof(lvldigest));

	if (lvllen == 0 || (lvllen & 0x1))
		return TOK_BAD_LEVEL_ENCODING;
	
	g_assert(lvllen >= 2);
	g_assert((lvllen & 0x1) == 0);

	/*
	 * Only check the highest keys we can check.
	 */

	lvllen /= 2;							/* # of keys held remotely */
	lvlsize = G_N_ELEMENTS(token_keys) - (tk - token_keys);
	lvlsize = MIN(lvllen, lvlsize);

	g_assert(lvlsize >= 1);

	rtk = tk + (lvlsize - 1);				/* Keys at that level */

	stamp32 = crc32_update_crc(0, token, TOKEN_VERSION_SIZE);
	klen = strlen(rtk->keys[0]);

	for (i = 0; i < rtk->count; i++)
		stamp32 = crc32_update_crc(stamp32, rtk->keys[i], klen);

	stamp32 = htonl(stamp32);

	lvlsize--;								/* Move to 0-based offset */

	if (lvldigest[2*lvlsize] != (c[0] ^ c[1]))
		return TOK_INVALID_LEVEL;

	if (lvldigest[2*lvlsize+1] != (c[2] ^ c[3]))
		return TOK_INVALID_LEVEL;

	for (i = 0; i < G_N_ELEMENTS(token_keys); i++) {
		rtk = &token_keys[i];
		if (rtk->ver.timestamp > rver.timestamp) {
			rtk--;							/* `rtk' could not exist remotely */
			break;
		}
	}

	if (lvlsize < rtk - tk)
		return TOK_SHORT_LEVEL;

	return TOK_OK;
}

/*
 * tok_is_ancient
 *
 * Check whether the version is too ancient to be able to generate a proper
 * token string identifiable by remote parties.
 */
gboolean tok_is_ancient(time_t now)
{
	return find_tokkey(now) == NULL;
}

/* vi: set ts=4: */
