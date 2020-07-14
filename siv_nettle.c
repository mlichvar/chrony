/*
  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Miroslav Lichvar  2019
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 * 
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 * 
 **********************************************************************

  =======================================================================

  SIV ciphers using the Nettle library
  */

#include "config.h"

#include "sysincl.h"

#ifdef HAVE_NETTLE_SIV_CMAC
#include <nettle/siv-cmac.h>
#else
#include "siv_nettle_int.c"
#endif

#include "memory.h"
#include "siv.h"

struct SIV_Instance_Record {
  struct siv_cmac_aes128_ctx siv;
};

/* ================================================== */

SIV_Instance
SIV_CreateInstance(SIV_Algorithm algorithm)
{
  SIV_Instance instance;

  if (algorithm != AEAD_AES_SIV_CMAC_256)
    return NULL;

  instance = MallocNew(struct SIV_Instance_Record);

  return instance;
}

/* ================================================== */

void
SIV_DestroyInstance(SIV_Instance instance)
{
  Free(instance);
}

/* ================================================== */

int
SIV_GetKeyLength(SIV_Algorithm algorithm)
{
  assert(32 <= SIV_MAX_KEY_LENGTH);

  if (algorithm == AEAD_AES_SIV_CMAC_256)
    return 32;
  return 0;
}

/* ================================================== */

int
SIV_SetKey(SIV_Instance instance, const unsigned char *key, int length)
{
  if (length != 32)
    return 0;

  siv_cmac_aes128_set_key(&instance->siv, key);

  return 1;
}

/* ================================================== */

int
SIV_GetTagLength(SIV_Instance instance)
{
  assert(SIV_DIGEST_SIZE <= SIV_MAX_TAG_LENGTH);

  return SIV_DIGEST_SIZE;
}

/* ================================================== */

int
SIV_Encrypt(SIV_Instance instance,
            const unsigned char *nonce, int nonce_length,
            const void *assoc, int assoc_length,
            const void *plaintext, int plaintext_length,
            unsigned char *ciphertext, int ciphertext_length)
{
  if (nonce_length < SIV_MIN_NONCE_SIZE || assoc_length < 0 ||
      plaintext_length < 0 || plaintext_length > ciphertext_length ||
      plaintext_length + SIV_DIGEST_SIZE != ciphertext_length)
    return 0;

  assert(assoc && plaintext);

  siv_cmac_aes128_encrypt_message(&instance->siv, nonce_length, nonce,
                                  assoc_length, assoc,
                                  ciphertext_length, ciphertext, plaintext);
  return 1;
}

/* ================================================== */

int
SIV_Decrypt(SIV_Instance instance,
            const unsigned char *nonce, int nonce_length,
            const void *assoc, int assoc_length,
            const unsigned char *ciphertext, int ciphertext_length,
            void *plaintext, int plaintext_length)
{
  if (nonce_length < SIV_MIN_NONCE_SIZE || assoc_length < 0 ||
      plaintext_length < 0 || plaintext_length > ciphertext_length ||
      plaintext_length + SIV_DIGEST_SIZE != ciphertext_length)
    return 0;

  assert(assoc && plaintext);

  if (!siv_cmac_aes128_decrypt_message(&instance->siv, nonce_length, nonce,
                                       assoc_length, assoc,
                                       plaintext_length, plaintext, ciphertext))
    return 0;

  return 1;
}
