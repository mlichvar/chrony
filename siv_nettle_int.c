/* This is a single-file implementation of AES-SIV-CMAC-256 based on
   a patch for GNU Nettle by Nikos Mavrogiannopoulos */

/*
   AES-CMAC-128 (rfc 4493)
   Copyright (C) Stefan Metzmacher 2012
   Copyright (C) Jeremy Allison 2012
   Copyright (C) Michael Adam 2012
   Copyright (C) 2017, Red Hat Inc.

   This file is part of GNU Nettle.

   GNU Nettle is free software: you can redistribute it and/or
   modify it under the terms of either:

     * the GNU Lesser General Public License as published by the Free
       Software Foundation; either version 3 of the License, or (at your
       option) any later version.

   or

     * the GNU General Public License as published by the Free
       Software Foundation; either version 2 of the License, or (at your
       option) any later version.

   or both in parallel, as here.

   GNU Nettle is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received copies of the GNU General Public License and
   the GNU Lesser General Public License along with this program.  If
   not, see http://www.gnu.org/licenses/.
*/
/* siv-aes128.c, siv-cmac.c, siv.h

   AES-SIV, RFC5297
   SIV-CMAC, RFC5297

   Copyright (C) 2017 Nikos Mavrogiannopoulos

   This file is part of GNU Nettle.

   GNU Nettle is free software: you can redistribute it and/or
   modify it under the terms of either:

     * the GNU Lesser General Public License as published by the Free
       Software Foundation; either version 3 of the License, or (at your
       option) any later version.

   or

     * the GNU General Public License as published by the Free
       Software Foundation; either version 2 of the License, or (at your
       option) any later version.

   or both in parallel, as here.

   GNU Nettle is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received copies of the GNU General Public License and
   the GNU Lesser General Public License along with this program.  If
   not, see http://www.gnu.org/licenses/.
*/
/* cmac.h, siv-cmac.h, cmac-aes128.c

   CMAC mode, as specified in RFC4493
   SIV-CMAC mode, as specified in RFC5297
   CMAC using AES128 as the underlying cipher.

   Copyright (C) 2017 Red Hat, Inc.

   Contributed by Nikos Mavrogiannopoulos

   This file is part of GNU Nettle.

   GNU Nettle is free software: you can redistribute it and/or
   modify it under the terms of either:

     * the GNU Lesser General Public License as published by the Free
       Software Foundation; either version 3 of the License, or (at your
       option) any later version.

   or

     * the GNU General Public License as published by the Free
       Software Foundation; either version 2 of the License, or (at your
       option) any later version.

   or both in parallel, as here.

   GNU Nettle is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received copies of the GNU General Public License and
   the GNU Lesser General Public License along with this program.  If
   not, see http://www.gnu.org/licenses/.
*/

# include "config.h"

#include <assert.h>
#include <string.h>

#include "nettle/aes.h"
#include "nettle/ctr.h"
#include "nettle/macros.h"
#include "nettle/memxor.h"
#include "nettle/memops.h"

#include "nettle/nettle-types.h"

/* For SIV, the block size of the block cipher shall be 128 bits. */
#define SIV_BLOCK_SIZE  16
#define SIV_DIGEST_SIZE 16
#define SIV_MIN_NONCE_SIZE 1

/*
 * SIV mode requires the aad and plaintext when building the IV, which
 * prevents streaming processing and it incompatible with the AEAD API.
 */

/* AES_SIV_CMAC_256 */
struct siv_cmac_aes128_ctx {
    struct aes128_ctx         cipher;
    uint8_t s2vk[AES128_KEY_SIZE];
};

struct cmac128_ctx
{
  /* Key */
  union nettle_block16 K1;
  union nettle_block16 K2;

  /* MAC state */
  union nettle_block16 X;

  /* Block buffer */
  union nettle_block16 block;
  size_t index;
};

/* shift one and XOR with 0x87. */
static void
_cmac128_block_mulx(union nettle_block16 *dst,
	   const union nettle_block16 *src)
{
  uint64_t b1 = READ_UINT64(src->b);
  uint64_t b2 = READ_UINT64(src->b+8);

  b1 = (b1 << 1) | (b2 >> 63);
  b2 <<= 1;

  if (src->b[0] & 0x80)
    b2 ^= 0x87;

  WRITE_UINT64(dst->b, b1);
  WRITE_UINT64(dst->b+8, b2);
}

static void
cmac128_set_key(struct cmac128_ctx *ctx, const void *cipher,
		nettle_cipher_func *encrypt)
{
  static const uint8_t const_zero[] = {
    0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00
  };
  union nettle_block16 *L = &ctx->block;
  memset(ctx, 0, sizeof(*ctx));

  /* step 1 - generate subkeys k1 and k2 */
  encrypt(cipher, 16, L->b, const_zero);

  _cmac128_block_mulx(&ctx->K1, L);
  _cmac128_block_mulx(&ctx->K2, &ctx->K1);
}

#define MIN(x,y) ((x)<(y)?(x):(y))

static void
cmac128_update(struct cmac128_ctx *ctx, const void *cipher,
	       nettle_cipher_func *encrypt,
	       size_t msg_len, const uint8_t *msg)
{
  union nettle_block16 Y;
  /*
   * check if we expand the block
   */
  if (ctx->index < 16)
    {
      size_t len = MIN(16 - ctx->index, msg_len);
      memcpy(&ctx->block.b[ctx->index], msg, len);
      msg += len;
      msg_len -= len;
      ctx->index += len;
    }

  if (msg_len == 0) {
    /* if it is still the last block, we are done */
    return;
  }

  /*
   * now checksum everything but the last block
   */
  memxor3(Y.b, ctx->X.b, ctx->block.b, 16);
  encrypt(cipher, 16, ctx->X.b, Y.b);

  while (msg_len > 16)
    {
      memxor3(Y.b, ctx->X.b, msg, 16);
      encrypt(cipher, 16, ctx->X.b, Y.b);
      msg += 16;
      msg_len -= 16;
    }

  /*
   * copy the last block, it will be processed in
   * cmac128_digest().
   */
  memcpy(ctx->block.b, msg, msg_len);
  ctx->index = msg_len;
}

static void
cmac128_digest(struct cmac128_ctx *ctx, const void *cipher,
	       nettle_cipher_func *encrypt,
	       unsigned length,
	       uint8_t *dst)
{
  union nettle_block16 Y;

  memset(ctx->block.b+ctx->index, 0, sizeof(ctx->block.b)-ctx->index);

  /* re-use ctx->block for memxor output */
  if (ctx->index < 16)
    {
      ctx->block.b[ctx->index] = 0x80;
      memxor(ctx->block.b, ctx->K2.b, 16);
    }
  else
    {
      memxor(ctx->block.b, ctx->K1.b, 16);
    }

  memxor3(Y.b, ctx->block.b, ctx->X.b, 16);

  assert(length <= 16);
  if (length == 16)
    {
      encrypt(cipher, 16, dst, Y.b);
    }
  else
    {
      encrypt(cipher, 16, ctx->block.b, Y.b);
      memcpy(dst, ctx->block.b, length);
    }

  /* reset state for re-use */
  memset(&ctx->X, 0, sizeof(ctx->X));
  ctx->index = 0;
}


#define CMAC128_CTX(type) \
  { struct cmac128_ctx ctx; type cipher; }

/* NOTE: Avoid using NULL, as we don't include anything defining it. */
#define CMAC128_SET_KEY(self, set_key, encrypt, cmac_key)	\
  do {								\
    (set_key)(&(self)->cipher, (cmac_key));			\
    if (0) (encrypt)(&(self)->cipher, ~(size_t) 0,		\
		     (uint8_t *) 0, (const uint8_t *) 0);	\
    cmac128_set_key(&(self)->ctx, &(self)->cipher,		\
		(nettle_cipher_func *) (encrypt));		\
  } while (0)

#define CMAC128_UPDATE(self, encrypt, length, src)		\
  cmac128_update(&(self)->ctx, &(self)->cipher,			\
	      (nettle_cipher_func *)encrypt, (length), (src))

#define CMAC128_DIGEST(self, encrypt, length, digest)		\
  (0 ? (encrypt)(&(self)->cipher, ~(size_t) 0,			\
		 (uint8_t *) 0, (const uint8_t *) 0)		\
     : cmac128_digest(&(self)->ctx, &(self)->cipher,		\
		  (nettle_cipher_func *) (encrypt),		\
		  (length), (digest)))

struct cmac_aes128_ctx CMAC128_CTX(struct aes128_ctx);

static void
cmac_aes128_set_key(struct cmac_aes128_ctx *ctx, const uint8_t *key)
{
  CMAC128_SET_KEY(ctx, aes128_set_encrypt_key, aes128_encrypt, key);
}

static void
cmac_aes128_update (struct cmac_aes128_ctx *ctx,
		   size_t length, const uint8_t *data)
{
  CMAC128_UPDATE (ctx, aes128_encrypt, length, data);
}

static void
cmac_aes128_digest(struct cmac_aes128_ctx *ctx,
		  size_t length, uint8_t *digest)
{
  CMAC128_DIGEST(ctx, aes128_encrypt, length, digest);
}

static const uint8_t const_one[] = {
	0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x01
};

static const uint8_t const_zero[] = {
	0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00
};

static
void _siv_s2v(nettle_set_key_func *cmac_set_key,
	      nettle_hash_update_func *cmac_update,
	      nettle_hash_digest_func *cmac_digest,
	      size_t cmac_ctx_size,
	      const uint8_t *s2vk, size_t alength, const uint8_t *adata,
              size_t nlength, const uint8_t *nonce,
              size_t plength, const uint8_t *pdata,
              uint8_t *v)
{
  uint8_t ctx[sizeof(struct cmac128_ctx)+sizeof(struct aes_ctx)];
  union nettle_block16 D, S, T;

  assert(cmac_ctx_size <= sizeof (ctx));

  cmac_set_key(ctx, s2vk);

  if (nlength == 0 && alength == 0) {
    cmac_update(ctx, 16, const_one);
    cmac_digest(ctx, 16, v);
    return;
  }

  cmac_update(ctx, 16, const_zero);
  cmac_digest(ctx, 16, D.b);

  if (1) {
    _cmac128_block_mulx(&D, &D);
    cmac_update(ctx, alength, adata);
    cmac_digest(ctx, 16, S.b);

    memxor(D.b, S.b, 16);
  }

  if (nlength > 0) {
    _cmac128_block_mulx(&D, &D);
    cmac_update(ctx, nlength, nonce);
    cmac_digest(ctx, 16, S.b);

    memxor(D.b, S.b, 16);
  }

  /* Sn */
  if (plength >= 16) {
    cmac_update(ctx, plength-16, pdata);

    pdata += plength-16;

    memxor3(T.b, pdata, D.b, 16);
  } else {
    union nettle_block16 pad;

    _cmac128_block_mulx(&T, &D);
    memcpy(pad.b, pdata, plength);
    pad.b[plength] = 0x80;
    if (plength+1 < 16)
      memset(&pad.b[plength+1], 0, 16-plength-1);

    memxor(T.b, pad.b, 16);
  }

  cmac_update(ctx, 16, T.b);
  cmac_digest(ctx, 16, v);
}

static void
siv_cmac_aes128_set_key(struct siv_cmac_aes128_ctx *ctx, const uint8_t *key)
{
  memcpy(ctx->s2vk, key, 16);
  aes128_set_encrypt_key(&ctx->cipher, key+16);
}

static void
siv_cmac_aes128_encrypt_message(struct siv_cmac_aes128_ctx *ctx,
				size_t nlength, const uint8_t *nonce,
				size_t alength, const uint8_t *adata,
				size_t clength, uint8_t *dst, const uint8_t *src)
{
  union nettle_block16 siv;
  size_t slength;

  assert (clength >= SIV_DIGEST_SIZE);
  slength = clength - SIV_DIGEST_SIZE;

  /* create CTR nonce */
  _siv_s2v((nettle_set_key_func*)cmac_aes128_set_key,
	   (nettle_hash_update_func*)cmac_aes128_update,
	   (nettle_hash_digest_func*)cmac_aes128_digest,
	   sizeof(struct cmac_aes128_ctx), ctx->s2vk, alength, adata,
	   nlength, nonce, slength, src, siv.b);
  memcpy(dst, siv.b, SIV_DIGEST_SIZE);
  siv.b[8] &= ~0x80;
  siv.b[12] &= ~0x80;

  ctr_crypt(&ctx->cipher, (nettle_cipher_func *)aes128_encrypt, AES_BLOCK_SIZE,
            siv.b, slength, dst+SIV_DIGEST_SIZE, src);
}

static int
siv_cmac_aes128_decrypt_message(struct siv_cmac_aes128_ctx *ctx,
				size_t nlength, const uint8_t *nonce,
				size_t alength, const uint8_t *adata,
				size_t mlength, uint8_t *dst, const uint8_t *src)
{
  union nettle_block16 siv;
  union nettle_block16 ctr;

  memcpy(ctr.b, src, SIV_DIGEST_SIZE);
  ctr.b[8] &= ~0x80;
  ctr.b[12] &= ~0x80;

  ctr_crypt(&ctx->cipher, (nettle_cipher_func *)aes128_encrypt, AES_BLOCK_SIZE,
            ctr.b, mlength, dst, src+SIV_DIGEST_SIZE);

  /* create CTR nonce */
  _siv_s2v((nettle_set_key_func*)cmac_aes128_set_key,
	   (nettle_hash_update_func*)cmac_aes128_update,
	   (nettle_hash_digest_func*)cmac_aes128_digest,
	   sizeof(struct cmac_aes128_ctx), ctx->s2vk, alength, adata,
	   nlength, nonce, mlength, dst, siv.b);

  return memeql_sec(siv.b, src, SIV_DIGEST_SIZE);
}

