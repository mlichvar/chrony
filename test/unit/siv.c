/*
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
 */

#include <config.h>
#include <sysincl.h>
#include <logging.h>
#include <siv.h>
#include "test.h"

#ifdef HAVE_SIV

struct siv_test {
  SIV_Algorithm algorithm;
  const unsigned char key[64];
  int key_length;
  const unsigned char nonce[128];
  int nonce_length;
  const unsigned char assoc[128];
  int assoc_length;
  const unsigned char plaintext[128];
  int plaintext_length;
  const unsigned char ciphertext[128];
  int ciphertext_length;
};

void
test_unit(void)
{
  struct siv_test tests[] = {
    { AEAD_AES_SIV_CMAC_256,
      "\x01\x23\x45\x67\x89\xab\xcd\xef\xf0\x12\x34\x56\x78\x9a\xbc\xde"
      "\xef\x01\x23\x45\x67\x89\xab\xcd\xde\xf0\x12\x34\x56\x78\x9a\xbc", 32,
      "\x10\x11\x12\x13\x14\x15\x16\x17\x18\x19\x1a\x1b\x1c\x1d\x1e\x1f", 16,
      "", 0,
      "", 0,
      "\x22\x3e\xb5\x94\xe0\xe0\x25\x4b\x00\x25\x8e\x21\x9a\x1c\xa4\x21", 16
    },
    { AEAD_AES_SIV_CMAC_256,
      "\x01\x23\x45\x67\x89\xab\xcd\xef\xf0\x12\x34\x56\x78\x9a\xbc\xde"
      "\xef\x01\x23\x45\x67\x89\xab\xcd\xde\xf0\x12\x34\x56\x78\x9a\xbc", 32,
      "\x10\x11\x12\x13\x14\x15\x16\x17\x18\x19\x1a\x1b\x1c\x1d\x1e\x1f", 16,
      "\x4c\x9d\x4f\xca\xed\x8a\xe2\xba\xad\x3f\x3e\xa6\xe9\x3c\x8c\x8b", 16,
      "", 0,
      "\xd7\x20\x19\x89\xc6\xdb\xc6\xd6\x61\xfc\x62\xbc\x86\x5e\xee\xef", 16
    },
    { AEAD_AES_SIV_CMAC_256,
      "\x01\x23\x45\x67\x89\xab\xcd\xef\xf0\x12\x34\x56\x78\x9a\xbc\xde"
      "\xef\x01\x23\x45\x67\x89\xab\xcd\xde\xf0\x12\x34\x56\x78\x9a\xbc", 32,
      "\x10\x11\x12\x13\x14\x15\x16\x17\x18\x19\x1a\x1b\x1c\x1d\x1e\x1f", 16,
      "", 0,
      "\x4c\x9d\x4f\xca\xed\x8a\xe2\xba\xad\x3f\x3e\xa6\xe9\x3c\x8c\x8b", 16,
      "\xb6\xc1\x60\xe9\xc2\xfd\x2a\xe8\xde\xc5\x36\x8b\x2a\x33\xed\xe1"
      "\x14\xff\xb3\x97\x34\x5c\xcb\xe4\x4a\xa4\xde\xac\xd9\x36\x90\x46", 32
    },
    { AEAD_AES_SIV_CMAC_256,
      "\x01\x23\x45\x67\x89\xab\xcd\xef\xf0\x12\x34\x56\x78\x9a\xbc\xde"
      "\xef\x01\x23\x45\x67\x89\xab\xcd\xde\xf0\x12\x34\x56\x78\x9a\xbc", 32,
      "\x10\x11\x12\x13\x14\x15\x16\x17\x18\x19\x1a\x1b\x1c\x1d\x1e", 15,
      "\x4c\x9d\x4f\xca\xed\x8a\xe2\xba\xad\x3f\x3e\xa6\xe9\x3c\x8c", 15,
      "\xba\x99\x79\x31\x23\x7e\x3c\x53\x58\x7e\xd4\x93\x02\xab\xe4", 15,
      "\x03\x8c\x41\x51\xba\x7a\x8f\x77\x6e\x56\x31\x99\x42\x0b\xc7\x03"
      "\xe7\x6c\x67\xc9\xda\xb7\x0d\x5b\x44\x06\x26\x5a\xd0\xd2\x3b", 31
    },
    { AEAD_AES_SIV_CMAC_256,
      "\x01\x23\x45\x67\x89\xab\xcd\xef\xf0\x12\x34\x56\x78\x9a\xbc\xde"
      "\xef\x01\x23\x45\x67\x89\xab\xcd\xde\xf0\x12\x34\x56\x78\x9a\xbc", 32,
      "\x10\x11\x12\x13\x14\x15\x16\x17\x18\x19\x1a\x1b\x1c\x1d\x1e\x1f", 16,
      "\x4c\x9d\x4f\xca\xed\x8a\xe2\xba\xad\x3f\x3e\xa6\xe9\x3c\x8c\x8b", 16,
      "\xba\x99\x79\x31\x23\x7e\x3c\x53\x58\x7e\xd4\x93\x02\xab\xe4\xa7", 16,
      "\x5c\x05\x23\x65\xf4\x57\x0a\xa0\xfb\x38\x3e\xce\x9b\x75\x85\xeb"
      "\x68\x85\x19\x36\x0c\x7c\x48\x11\x40\xcb\x9b\x57\x9a\x0e\x65\x32", 32
    },
    { AEAD_AES_SIV_CMAC_256,
      "\x01\x23\x45\x67\x89\xab\xcd\xef\xf0\x12\x34\x56\x78\x9a\xbc\xde"
      "\xef\x01\x23\x45\x67\x89\xab\xcd\xde\xf0\x12\x34\x56\x78\x9a\xbc", 32,
      "\x10\x11\x12\x13\x14\x15\x16\x17\x18\x19\x1a\x1b\x1c\x1d\x1e\x1f"
      "\xd5", 17,
      "\x4c\x9d\x4f\xca\xed\x8a\xe2\xba\xad\x3f\x3e\xa6\xe9\x3c\x8c\x8b"
      "\xa0", 17,
      "\xba\x99\x79\x31\x23\x7e\x3c\x53\x58\x7e\xd4\x93\x02\xab\xe4\xa7"
      "\x08", 17,
      "\xaf\x58\x4b\xe7\x82\x1e\x96\x19\x29\x91\x25\xe0\xdd\x80\x3b\x49"
      "\xa5\x11\xcd\xb6\x08\xf3\x76\xa0\xb6\xfa\x15\x82\xf3\x95\xe1\xeb"
      "\xbd", 33
    },
    { AEAD_AES_SIV_CMAC_256,
      "\x01\x23\x45\x67\x89\xab\xcd\xef\xf0\x12\x34\x56\x78\x9a\xbc\xde"
      "\xef\x01\x23\x45\x67\x89\xab\xcd\xde\xf0\x12\x34\x56\x78\x9a\xbc", 32,
      "\xb0\x5a\x1b\xc7\x56\xe7\xb6\x2c\xb4\x85\xe5\x56\xa5\x28\xc0\x6c"
      "\x2f\x3b\x0b\x9d\x1a\x0c\xdf\x69\x47\xe0\xcc\xc0\x87\xaa\x5c\x09"
      "\x98\x48\x8d\x6a\x8e\x1e\x05\xd7\x8b\x68\x74\x83\xb5\x1d\xf1\x2c", 48,
      "\xe5\x8b\xd2\x6a\x30\xc5\xc5\x61\xcc\xbd\x7c\x27\xbf\xfe\xf9\x06"
      "\x00\x5b\xd7\xfc\x11\x0b\xcf\x16\x61\xef\xac\x05\xa7\xaf\xec\x27"
      "\x41\xc8\x5e\x9e\x0d\xf9\x2f\xaf\x20\x79\x17\xe5\x17\x91\x2a\x27"
      "\x34\x1c\xbc\xaf\xeb\xef\x7f\x52\xe7\x1e\x4c\x2a\xca\xbd\x2b\xbe"
      "\x34\xd6\xfb\x69\xd3\x3e\x49\x59\x60\xb4\x26\xc9\xb8\xce\xba", 79,
      "\x6c\xe7\xcf\x7e\xab\x7b\xa0\xe1\xa7\x22\xcb\x88\xde\x5e\x42\xd2"
      "\xec\x79\xe0\xa2\xcf\x5f\x0f\x6f\x6b\x89\x57\xcd\xae\x17\xd4\xc2"
      "\xf3\x1b\xa2\xa8\x13\x78\x23\x2f\x83\xa8\xd4\x0c\xc0\xd2\xf3\x99"
      "\xae\x81\xa1\xca\x5b\x5f\x45\xa6\x6f\x0c\x8a\xf3\xd4\x67\x40\x81"
      "\x26\xe2\x01\x86\xe8\x5a\xd5\xf8\x58\x80\x9f\x56\xaa\x76\x96\xbf"
      "\x31", 81,
      "\x9a\x06\x33\xe0\xee\x00\x6a\x9b\xc8\x20\xd5\xe2\xc2\xed\xb5\x75"
      "\xfa\x9e\x42\x2a\x31\x6b\xda\xca\xaa\x7d\x31\x8b\x84\x7a\xb8\xd7"
      "\x8a\x81\x25\x64\xed\x41\x9b\xa9\x77\x10\xbd\x05\x0c\x4e\xc5\x31"
      "\x0c\xa2\x86\xec\x8a\x94\xc8\x24\x23\x3c\x13\xee\xa5\x51\xc9\xdf"
      "\x48\xc9\x55\xc5\x2f\x40\x73\x3f\x98\xbb\x8d\x69\x78\x46\x64\x17"
      "\x8d\x49\x2f\x14\x62\xa4\x7c\x2a\x57\x38\x87\xce\xc6\x72\xd3\x5c"
      "\xa1", 97
    },
    { 0, "", 0 }
  };

  unsigned char plaintext[sizeof (((struct siv_test *)NULL)->plaintext)];
  unsigned char ciphertext[sizeof (((struct siv_test *)NULL)->ciphertext)];
  SIV_Instance siv;
  int i, j, r;

  TEST_CHECK(SIV_CreateInstance(0) == NULL);

  for (i = 0; tests[i].algorithm != 0; i++) {
    DEBUG_LOG("testing %d (%d)", (int)tests[i].algorithm, i);

    assert(tests[i].key_length <= sizeof (tests[i].key));
    assert(tests[i].nonce_length <= sizeof (tests[i].nonce));
    assert(tests[i].assoc_length <= sizeof (tests[i].assoc));
    assert(tests[i].plaintext_length <= sizeof (tests[i].plaintext));
    assert(tests[i].ciphertext_length <= sizeof (tests[i].ciphertext));

    siv = SIV_CreateInstance(tests[i].algorithm);
    TEST_CHECK(siv != NULL);

    TEST_CHECK(SIV_GetKeyLength(tests[i].algorithm) == tests[i].key_length);

    for (j = -1; j < 1024; j++) {
      r = SIV_SetKey(siv, tests[i].key, j);
      TEST_CHECK(r == (j == tests[i].key_length));
    }

    TEST_CHECK(SIV_GetTagLength(siv) == tests[i].ciphertext_length - tests[i].plaintext_length);

    r = SIV_Encrypt(siv, tests[i].nonce, tests[i].nonce_length,
                    tests[i].assoc, tests[i].assoc_length,
                    tests[i].plaintext, tests[i].plaintext_length,
                    ciphertext, tests[i].ciphertext_length);
    TEST_CHECK(r);

#if 0
    for (j = 0; j < tests[i].ciphertext_length; j++) {
      printf("\\x%02x", ciphertext[j]);
      if (j % 16 == 15)
        printf("\n");
    }
    printf("\n");
#endif
    TEST_CHECK(memcmp(ciphertext, tests[i].ciphertext, tests[i].ciphertext_length) == 0);

    for (j = -1; j < tests[i].nonce_length; j++) {
      r = SIV_Encrypt(siv, tests[i].nonce, j,
                      tests[i].assoc, tests[i].assoc_length,
                      tests[i].plaintext, tests[i].plaintext_length,
                      ciphertext, tests[i].ciphertext_length);
      if (j > 0) {
        TEST_CHECK(r);
        TEST_CHECK(memcmp(ciphertext, tests[i].ciphertext, tests[i].ciphertext_length) != 0);
      } else {
        TEST_CHECK(!r);
      }
    }

    for (j = -1; j < tests[i].assoc_length; j++) {
      r = SIV_Encrypt(siv, tests[i].nonce, tests[i].nonce_length,
                      tests[i].assoc, j,
                      tests[i].plaintext, tests[i].plaintext_length,
                      ciphertext, tests[i].ciphertext_length);
      if (j >= 0) {
        TEST_CHECK(r);
        TEST_CHECK(memcmp(ciphertext, tests[i].ciphertext, tests[i].ciphertext_length) != 0);
      } else {
        TEST_CHECK(!r);
      }
    }

    for (j = -1; j < tests[i].plaintext_length; j++) {
      r = SIV_Encrypt(siv, tests[i].nonce, tests[i].nonce_length,
                      tests[i].assoc, tests[i].assoc_length,
                      tests[i].plaintext, j,
                      ciphertext, j + SIV_GetTagLength(siv));
      if (j >= 0) {
        TEST_CHECK(r);
        TEST_CHECK(memcmp(ciphertext, tests[i].ciphertext, j + SIV_GetTagLength(siv)) != 0);
      } else {
        TEST_CHECK(!r);
      }
    }

    for (j = -1; j < 2 * tests[i].plaintext_length; j++) {
      if (j == tests[i].plaintext_length)
        continue;
      r = SIV_Encrypt(siv, tests[i].nonce, tests[i].nonce_length,
                      tests[i].assoc, tests[i].assoc_length,
                      tests[i].plaintext, j,
                      ciphertext, tests[i].ciphertext_length);
      TEST_CHECK(!r);
    }

    for (j = -1; j < 2 * tests[i].ciphertext_length; j++) {
      if (j == tests[i].ciphertext_length)
        continue;
      r = SIV_Encrypt(siv, tests[i].nonce, tests[i].nonce_length,
                      tests[i].assoc, tests[i].assoc_length,
                      tests[i].plaintext, tests[i].plaintext_length,
                      ciphertext, j);
      TEST_CHECK(!r);
    }

    r = SIV_Decrypt(siv, tests[i].nonce, tests[i].nonce_length,
                    tests[i].assoc, tests[i].assoc_length,
                    tests[i].ciphertext, tests[i].ciphertext_length,
                    plaintext, tests[i].plaintext_length);
    TEST_CHECK(r);
    TEST_CHECK(memcmp(plaintext, tests[i].plaintext, tests[i].plaintext_length) == 0);

    for (j = -1; j < tests[i].nonce_length; j++) {
      r = SIV_Decrypt(siv, tests[i].nonce, j,
                      tests[i].assoc, tests[i].assoc_length,
                      tests[i].ciphertext, tests[i].ciphertext_length,
                      plaintext, tests[i].plaintext_length);
      TEST_CHECK(!r);
    }

    for (j = -1; j < tests[i].assoc_length; j++) {
      r = SIV_Decrypt(siv, tests[i].nonce, tests[i].nonce_length,
                      tests[i].assoc, j,
                      tests[i].ciphertext, tests[i].ciphertext_length,
                      plaintext, tests[i].plaintext_length);
      TEST_CHECK(!r);
    }

    for (j = -1; j < 2 * tests[i].ciphertext_length; j++) {
      if (j == tests[i].ciphertext_length)
        continue;

      r = SIV_Decrypt(siv, tests[i].nonce, tests[i].nonce_length,
                      tests[i].assoc, tests[i].assoc_length,
                      tests[i].ciphertext, j,
                      plaintext, tests[i].plaintext_length);
      TEST_CHECK(!r);
    }

    for (j = -1; j < tests[i].plaintext_length; j++) {
      r = SIV_Decrypt(siv, tests[i].nonce, tests[i].nonce_length,
                      tests[i].assoc, tests[i].assoc_length,
                      tests[i].ciphertext, tests[i].ciphertext_length,
                      plaintext, j);
      TEST_CHECK(!r);

      r = SIV_Decrypt(siv, tests[i].nonce, tests[i].nonce_length,
                      tests[i].assoc, tests[i].assoc_length,
                      tests[i].ciphertext, j + SIV_GetTagLength(siv),
                      plaintext, j);
      TEST_CHECK(!r);
    }

    SIV_DestroyInstance(siv);
  }

  siv = SIV_CreateInstance(tests[0].algorithm);
  for (i = 0; i < 1000; i++) {
    for (j = 0; tests[j].algorithm == tests[0].algorithm; j++) {
      r = SIV_SetKey(siv, tests[j].key, tests[j].key_length);
      TEST_CHECK(r);
      r = SIV_Encrypt(siv, tests[j].nonce, tests[j].nonce_length,
                      tests[j].assoc, tests[j].assoc_length,
                      tests[j].plaintext, tests[j].plaintext_length,
                      ciphertext, tests[j].ciphertext_length);
      TEST_CHECK(r);
      r = SIV_Decrypt(siv, tests[j].nonce, tests[j].nonce_length,
                      tests[j].assoc, tests[j].assoc_length,
                      tests[j].ciphertext, tests[j].ciphertext_length,
                      plaintext, tests[j].plaintext_length);
      TEST_CHECK(r);
    }
  }
  SIV_DestroyInstance(siv);
}
#else
void
test_unit(void)
{
  TEST_REQUIRE(0);
}
#endif
