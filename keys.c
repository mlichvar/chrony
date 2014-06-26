/*
  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Richard P. Curnow  1997-2003
 * Copyright (C) Miroslav Lichvar  2012-2014
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

#include "sysincl.h"

#include "keys.h"
#include "cmdparse.h"
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

static int
generate_key(unsigned long key_id)
{
#ifdef GENERATE_SHA1_KEY
  unsigned char key[20];
  const char *hashname = "SHA1";
#else
  unsigned char key[16];
  const char *hashname = "MD5";
#endif
  const char *key_file, *rand_dev = "/dev/urandom";
  FILE *f;
  struct stat st;
  int i;

  key_file = CNF_GetKeysFile();

  if (!key_file)
    return 0;

  f = fopen(rand_dev, "r");
  if (!f || fread(key, sizeof (key), 1, f) != 1) {
    if (f)
      fclose(f);
    LOG_FATAL(LOGF_Keys, "Could not read %s", rand_dev);
    return 0;
  }
  fclose(f);

  f = fopen(key_file, "a");
  if (!f) {
    LOG_FATAL(LOGF_Keys, "Could not open keyfile %s for writing", key_file);
    return 0;
  }

  /* Make sure the keyfile is not world-readable */
  if (stat(key_file, &st) || chmod(key_file, st.st_mode & 0770)) {
    fclose(f);
    LOG_FATAL(LOGF_Keys, "Could not change permissions of keyfile %s", key_file);
    return 0;
  }

  fprintf(f, "\n%lu %s HEX:", key_id, hashname);
  for (i = 0; i < sizeof (key); i++)
    fprintf(f, "%02hhX", key[i]);
  fprintf(f, "\n");
  fclose(f);

  /* Erase the key from stack */
  memset(key, 0, sizeof (key));

  LOG(LOGS_INFO, LOGF_Keys, "Generated key %lu", key_id);

  return 1;
}

/* ================================================== */

void
KEY_Initialise(void)
{
  n_keys = 0;
  command_key_valid = 0;
  cache_valid = 0;
  KEY_Reload();

  if (CNF_GetGenerateCommandKey() && !KEY_KeyKnown(KEY_GetCommandKey())) {
    if (generate_key(KEY_GetCommandKey()))
      KEY_Reload();
  }
}

/* ================================================== */

void
KEY_Finalise(void)
{
}

/* ================================================== */

static int
determine_hash_delay(unsigned long key_id)
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
  LOG(LOGS_INFO, LOGF_Keys, "authentication delay for key %lu: %d useconds", key_id, min_usecs);
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

void
KEY_Reload(void)
{
  int i, line_number;
  FILE *in;
  unsigned long key_id;
  char line[2048], *keyval, *key_file;
  const char *hashname;

  for (i=0; i<n_keys; i++) {
    Free(keys[i].val);
  }
  n_keys = 0;
  command_key_valid = 0;
  cache_valid = 0;

  key_file = CNF_GetKeysFile();
  line_number = 0;

  if (!key_file)
    return;

  in = fopen(key_file, "r");
  if (!in) {
    LOG(LOGS_WARN, LOGF_Keys, "Could not open keyfile %s", key_file);
    return;
  }

  while (fgets(line, sizeof (line), in)) {
    line_number++;

    CPS_NormalizeLine(line);
    if (!*line)
      continue;

    if (!CPS_ParseKey(line, &key_id, &hashname, &keyval)) {
      LOG(LOGS_WARN, LOGF_Keys, "Could not parse key at line %d in file %s", line_number, key_file);
      continue;
    }

    keys[n_keys].hash_id = HSH_GetHashId(hashname);
    if (keys[n_keys].hash_id < 0) {
      LOG(LOGS_WARN, LOGF_Keys, "Unknown hash function in key %lu", key_id);
      continue;
    }

    keys[n_keys].len = UTI_DecodePasswordFromText(keyval);
    if (!keys[n_keys].len) {
      LOG(LOGS_WARN, LOGF_Keys, "Could not decode password in key %lu", key_id);
      continue;
    }

    keys[n_keys].id = key_id;
    keys[n_keys].val = MallocArray(char, keys[n_keys].len);
    memcpy(keys[n_keys].val, keyval, keys[n_keys].len);
    n_keys++;
  }

  fclose(in);

  /* Sort keys into order.  Note, if there's a duplicate, it is
     arbitrary which one we use later - the user should have been
     more careful! */
  qsort((void *) keys, n_keys, sizeof(Key), compare_keys_by_id);

  /* Check for duplicates */
  for (i = 1; i < n_keys; i++) {
    if (keys[i - 1].id == keys[i].id) {
      LOG(LOGS_WARN, LOGF_Keys, "Detected duplicate key %lu", keys[i].id);
    }
  }

  /* Erase any passwords from stack */
  memset(line, 0, sizeof (line));

  for (i=0; i<n_keys; i++) {
    keys[i].auth_delay = determine_hash_delay(keys[i].id);
  }
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
  int position;

  if (cache_valid && key_id == cache_key_id)
    return cache_key_pos;

  position = lookup_key(key_id);

  if (position >= 0) {
    cache_valid = 1;
    cache_key_pos = position;
    cache_key_id = key_id;
  }

  return position;
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
  return get_key_pos(key_id) >= 0;
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
