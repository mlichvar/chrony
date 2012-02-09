/*
  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Richard P. Curnow  1997-2003
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

  Module for managing keys used for authenticating NTP packets and commands

  */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "keys.h"
#include "conf.h"
#include "memory.h"
#include "util.h"
#include "local.h"
#include "logging.h"


typedef struct {
  unsigned long id;
  char *val;
  int len;
  int hash_id;
  int auth_delay;
} Key;

#define MAX_KEYS 256

static int n_keys;
static Key keys[MAX_KEYS];

static int command_key_valid;
static int command_key_id;
static int cache_valid;
static unsigned long cache_key_id;
static int cache_key_pos;

/* ================================================== */

void
KEY_Initialise(void)
{
  n_keys = 0;
  command_key_valid = 0;
  cache_valid = 0;
  KEY_Reload();
  return;
}

/* ================================================== */

void
KEY_Finalise(void)
{
  /* Nothing to do */
  return;
}

/* ================================================== */

static int
determine_hash_delay(int key_id)
{
  NTP_Packet pkt;
  struct timeval before, after;
  unsigned long usecs, min_usecs=0;
  int i;

  for (i = 0; i < 10; i++) {
    LCL_ReadRawTime(&before);
    KEY_GenerateAuth(key_id, (unsigned char *)&pkt, NTP_NORMAL_PACKET_SIZE,
        (unsigned char *)&pkt.auth_data, sizeof (pkt.auth_data));
    LCL_ReadRawTime(&after);

    usecs = (after.tv_sec - before.tv_sec) * 1000000 + (after.tv_usec - before.tv_usec);

    if (i == 0 || usecs < min_usecs) {
      min_usecs = usecs;
    }
  }

#if 0
  LOG(LOGS_INFO, LOGF_Keys, "authentication delay for key %d: %d useconds", key_id, min_usecs);
#endif

  /* Add on a bit extra to allow for copying, conversions etc */
  return min_usecs + (min_usecs >> 4);
}

/* ================================================== */

/* Compare two keys */

static int
compare_keys_by_id(const void *a, const void *b)
{
  const Key *c = (const Key *) a;
  const Key *d = (const Key *) b;

  if (c->id < d->id) {
    return -1;
  } else if (c->id > d->id) {
    return +1;
  } else {
    return 0;
  }

}

/* ================================================== */


#define KEYLEN 2047
#define SKEYLEN "2047"

void
KEY_Reload(void)
{
  int i, len1, fields;
  char *key_file;
  FILE *in;
  unsigned long key_id;
  char line[KEYLEN+1], buf1[KEYLEN+1], buf2[KEYLEN+1];
  char *keyval, *hashname;

  for (i=0; i<n_keys; i++) {
    Free(keys[i].val);
  }
  n_keys = 0;

  key_file = CNF_GetKeysFile();

  if (key_file) {
    in = fopen(key_file, "r");
    if (in) {
      while (fgets(line, sizeof(line), in)) {
        len1 = strlen(line) - 1;

        /* Guard against removing last character of the line
         * if the last line of the file is missing an end-of-line */
        if (line[len1] == '\n') {
          line[len1] = '\0';
        }
        fields = sscanf(line, "%lu%" SKEYLEN "s%" SKEYLEN "s", &key_id, buf1, buf2);
        if (fields >= 2 && fields <= 3) {
          if (fields == 3) {
            hashname = buf1;
            keyval = buf2;
          } else {
            hashname = "MD5";
            keyval = buf1;
          }
          keys[n_keys].hash_id = HSH_GetHashId(hashname);
          if (keys[n_keys].hash_id < 0) {
            LOG(LOGS_WARN, LOGF_Keys, "Unknown hash function in key %d", key_id);
            continue;
          }

          keys[n_keys].len = UTI_DecodePasswordFromText(keyval);
          if (!keys[n_keys].len) {
            LOG(LOGS_WARN, LOGF_Keys, "Could not decode password in key %d", key_id);
            continue;
          }

          keys[n_keys].id = key_id;
          keys[n_keys].val = MallocArray(char, keys[n_keys].len);
          memcpy(keys[n_keys].val, keyval, keys[n_keys].len);
          n_keys++;
        }
      }
      fclose(in);
      
      /* Sort keys into order.  Note, if there's a duplicate, it is
         arbitrary which one we use later - the user should have been
         more careful! */
      qsort((void *) keys, n_keys, sizeof(Key), compare_keys_by_id);

      /* Erase the passwords from stack */
      memset(line, 0, sizeof (line));
      memset(buf1, 0, sizeof (buf1));
      memset(buf2, 0, sizeof (buf2));
    }
  }

  command_key_valid = 0;
  cache_valid = 0;

  for (i=0; i<n_keys; i++) {
    keys[i].auth_delay = determine_hash_delay(keys[i].id);
  }

  return;
}

/* ================================================== */

static int
lookup_key(unsigned long id)
{
  Key specimen, *where;
  int pos;

  specimen.id = id;
  where = (Key *) bsearch((void *)&specimen, (void *)keys, n_keys, sizeof(Key), compare_keys_by_id);
  if (!where) {
    return -1;
  } else {
    pos = where - keys;
    return pos;
  }
}

/* ================================================== */

static int
get_key_pos(unsigned long key_id)
{
  if (!cache_valid || key_id != cache_key_id) {
    cache_valid = 1;
    cache_key_pos = lookup_key(key_id);
    cache_key_id = key_id;
  }

  return cache_key_pos;
}

/* ================================================== */

unsigned long
KEY_GetCommandKey(void)
{
  if (!command_key_valid) {
    command_key_id = CNF_GetCommandKey();
  }

  return command_key_id;
}

/* ================================================== */

int
KEY_KeyKnown(unsigned long key_id)
{
  int position;

  if (cache_valid && (key_id == cache_key_id)) {
    return 1;
  } else {

    position = lookup_key(key_id);

    if (position >= 0) {
      /* Store key in cache, we will probably be using it in a
         minute... */
      cache_valid = 1;
      cache_key_pos = position;
      cache_key_id = key_id;
      return 1;
    } else {
      return 0;
    }
  }
}

/* ================================================== */

int
KEY_GetAuthDelay(unsigned long key_id)
{
  int key_pos;

  key_pos = get_key_pos(key_id);

  if (key_pos < 0) {
    return 0;
  }

  return keys[key_pos].auth_delay;
}

/* ================================================== */

int
KEY_GenerateAuth(unsigned long key_id, const unsigned char *data, int data_len,
    unsigned char *auth, int auth_len)
{
  int key_pos;

  key_pos = get_key_pos(key_id);

  if (key_pos < 0) {
    return 0;
  }

  return UTI_GenerateNTPAuth(keys[key_pos].hash_id,
      (unsigned char *)keys[key_pos].val, keys[key_pos].len,
      data, data_len, auth, auth_len);
}

/* ================================================== */

int
KEY_CheckAuth(unsigned long key_id, const unsigned char *data, int data_len,
    const unsigned char *auth, int auth_len)
{
  int key_pos;

  key_pos = get_key_pos(key_id);

  if (key_pos < 0) {
    return 0;
  }

  return UTI_CheckNTPAuth(keys[key_pos].hash_id,
      (unsigned char *)keys[key_pos].val, keys[key_pos].len,
      data, data_len, auth, auth_len);
}
