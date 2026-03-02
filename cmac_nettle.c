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

  Support for AES128 and AES256 CMAC in Nettle.

  */

#include "config.h"

#include "sysincl.h"

#include <nettle/cmac.h>
#include <nettle/version.h>

#include "cmac.h"
#include "hash.h"
#include "memory.h"

struct CMC_Instance_Record {
  int key_length;
  union {
    struct cmac_aes128_ctx aes128;
    struct cmac_aes256_ctx aes256;
  } context;
};

/* ================================================== */

int
CMC_GetKeyLength(CMC_Algorithm algorithm)
{
  if (algorithm == CMC_AES128)
    return AES128_KEY_SIZE;
  else if (algorithm == CMC_AES256)
    return AES256_KEY_SIZE;
  return 0;
}

/* ================================================== */

CMC_Instance
CMC_CreateInstance(CMC_Algorithm algorithm, const unsigned char *key, int length)
{
  CMC_Instance inst;

  if (length <= 0 || length != CMC_GetKeyLength(algorithm))
    return NULL;

  inst = MallocNew(struct CMC_Instance_Record);
  inst->key_length = length;

  switch (length) {
    case AES128_KEY_SIZE:
      cmac_aes128_set_key(&inst->context.aes128, key);
      break;
    case AES256_KEY_SIZE:
      cmac_aes256_set_key(&inst->context.aes256, key);
      break;
    default:
      assert(0);
  }

  return inst;
}

/* ================================================== */

int
CMC_Hash(CMC_Instance inst, const void *in, int in_len, unsigned char *out, int out_len)
{
  unsigned char buf[MAX_HASH_LENGTH];

  if (in_len < 0 || out_len < 0)
    return 0;

  if (out_len > CMAC128_DIGEST_SIZE)
    out_len = CMAC128_DIGEST_SIZE;

  assert(CMAC128_DIGEST_SIZE <= sizeof (buf));

  switch (inst->key_length) {
    case AES128_KEY_SIZE:
      cmac_aes128_update(&inst->context.aes128, in_len, in);
      cmac_aes128_digest(&inst->context.aes128,
#if NETTLE_VERSION_MAJOR < 4
                         CMAC128_DIGEST_SIZE,
#endif
                         buf);
      break;
    case AES256_KEY_SIZE:
      cmac_aes256_update(&inst->context.aes256, in_len, in);
      cmac_aes256_digest(&inst->context.aes256,
#if NETTLE_VERSION_MAJOR < 4
                         CMAC128_DIGEST_SIZE,
#endif
                         buf);
      break;
    default:
      assert(0);
  }

  memcpy(out, buf, out_len);

  return out_len;
}

/* ================================================== */

void
CMC_DestroyInstance(CMC_Instance inst)
{
  Free(inst);
}
