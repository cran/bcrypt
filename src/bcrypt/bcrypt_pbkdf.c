/* $OpenBSD: bcrypt_pbkdf.c,v 1.3 2013/06/04 15:55:50 tedu Exp $ */
/*
 * Copyright (c) 2013 Ted Unangst <tedu@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <string.h>
#include <stdlib.h>

#include "pybc_blf.h"
#include "pybc_sha2.h"

/*
 * pkcs #5 pbkdf2 implementation using the "bcrypt" hash
 *
 * The bcrypt hash function is derived from the bcrypt password hashing
 * function with the following modifications:
 * 1. The input password and salt are preprocessed with SHA512.
 * 2. The output length is expanded to 256 bits.
 * 3. Subsequently the magic string to be encrypted is lengthened and modifed
 *    to "OxychromaticBlowfishSwatDynamite"
 * 4. The hash function is defined to perform 64 rounds of initial state
 *    expansion. (More rounds are performed by iterating the hash.)
 *
 * Note that this implementation pulls the SHA512 operations into the caller
 * as a performance optimization.
 *
 * One modification from official pbkdf2. Instead of outputting key material
 * linearly, we mix it. pbkdf2 has a known weakness where if one uses it to
 * generate (i.e.) 512 bits of key material for use as two 256 bit keys, an
 * attacker can merely run once through the outer loop below, but the user
 * always runs it twice. Shuffling output bytes requires computing the
 * entirety of the key material to assemble any subkey. This is something a
 * wise caller could do; we just do it for you.
 */

#define BCRYPT_BLOCKS 8
#define BCRYPT_HASHSIZE (BCRYPT_BLOCKS * 4)

static void
bcrypt_hash(u_int8_t *sha2pass, u_int8_t *sha2salt, u_int8_t *out)
{
	pybc_blf_ctx state;
	u_int8_t ciphertext[BCRYPT_HASHSIZE] =
	    "OxychromaticBlowfishSwatDynamite";
	u_int32_t cdata[BCRYPT_BLOCKS];
	int i;
	u_int16_t j;
	size_t shalen = PYBC_SHA512_DIGEST_LENGTH;

	/* key expansion */
	pybc_Blowfish_initstate(&state);
	pybc_Blowfish_expandstate(&state, sha2salt, shalen, sha2pass, shalen);
	for (i = 0; i < 64; i++) {
		pybc_Blowfish_expand0state(&state, sha2salt, shalen);
		pybc_Blowfish_expand0state(&state, sha2pass, shalen);
	}

	/* encryption */
	j = 0;
	for (i = 0; i < BCRYPT_BLOCKS; i++)
		cdata[i] = pybc_Blowfish_stream2word(ciphertext, sizeof(ciphertext),
		    &j);
	for (i = 0; i < 64; i++)
		pybc_blf_enc(&state, cdata, sizeof(cdata) / sizeof(u_int32_t));

	/* copy out */
	for (i = 0; i < BCRYPT_BLOCKS; i++) {
		out[4 * i + 3] = (cdata[i] >> 24) & 0xff;
		out[4 * i + 2] = (cdata[i] >> 16) & 0xff;
		out[4 * i + 1] = (cdata[i] >> 8) & 0xff;
		out[4 * i + 0] = cdata[i] & 0xff;
	}

	/* zap */
	memset(ciphertext, 0, sizeof(ciphertext));
	memset(cdata, 0, sizeof(cdata));
	memset(&state, 0, sizeof(state));
}

int
bcrypt_pbkdf(const u_int8_t *pass, size_t passlen, const u_int8_t *salt, size_t saltlen,
    u_int8_t *key, size_t keylen, unsigned int rounds)
{
	PYBC_SHA2_CTX ctx;
	u_int8_t sha2pass[PYBC_SHA512_DIGEST_LENGTH];
	u_int8_t sha2salt[PYBC_SHA512_DIGEST_LENGTH];
	u_int8_t out[BCRYPT_HASHSIZE];
	u_int8_t tmpout[BCRYPT_HASHSIZE];
	u_int8_t countsalt[4];
	size_t i, j, amt, stride;
	u_int32_t count;

	/* nothing crazy */
	if (rounds < 1)
		return -1;
	if (passlen == 0 || saltlen == 0 || keylen == 0 ||
	    keylen > sizeof(out) * sizeof(out))
		return -1;
	stride = (keylen + sizeof(out) - 1) / sizeof(out);
	amt = (keylen + stride - 1) / stride;

	/* collapse password */
	PYBC_SHA512Init(&ctx);
	PYBC_SHA512Update(&ctx, pass, passlen);
	PYBC_SHA512Final(sha2pass, &ctx);

	/* generate key, sizeof(out) at a time */
	for (count = 1; keylen > 0; count++) {
		countsalt[0] = (count >> 24) & 0xff;
		countsalt[1] = (count >> 16) & 0xff;
		countsalt[2] = (count >> 8) & 0xff;
		countsalt[3] = count & 0xff;

		/* first round, salt is salt */
		PYBC_SHA512Init(&ctx);
		PYBC_SHA512Update(&ctx, salt, saltlen);
		PYBC_SHA512Update(&ctx, countsalt, sizeof(countsalt));
		PYBC_SHA512Final(sha2salt, &ctx);
		bcrypt_hash(sha2pass, sha2salt, tmpout);
		memcpy(out, tmpout, sizeof(out));

		for (i = 1; i < rounds; i++) {
			/* subsequent rounds, salt is previous output */
			PYBC_SHA512Init(&ctx);
			PYBC_SHA512Update(&ctx, tmpout, sizeof(tmpout));
			PYBC_SHA512Final(sha2salt, &ctx);
			bcrypt_hash(sha2pass, sha2salt, tmpout);
			for (j = 0; j < sizeof(out); j++)
				out[j] ^= tmpout[j];
		}

		/*
		 * pbkdf2 deviation: ouput the key material non-linearly.
		 */
		if (keylen < amt)
			amt = keylen;
		for (i = 0; i < amt; i++)
			key[i * stride + (count - 1)] = out[i];
		keylen -= amt;
	}

	/* zap */
	memset(&ctx, 0, sizeof(ctx));
	memset(out, 0, sizeof(out));

	return 0;
}
