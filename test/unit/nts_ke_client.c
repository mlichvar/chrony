/*
 **********************************************************************
 * Copyright (C) Miroslav Lichvar  2020
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
#include "test.h"

#ifdef FEAT_NTS

#include <nts_ke_client.c>
#include <local.h>

static void
prepare_response(NKSN_Instance session, int valid)
{
  uint16_t data[16];
  int i, index, length;

  if (valid)
    index = -1;
  else
    index = random() % 10;
  DEBUG_LOG("index=%d", index);

  NKSN_BeginMessage(session);

  memset(data, 0, sizeof (data));
  length = 2;
  assert(sizeof (data[0]) == 2);

  if (index == 0) {
    data[0] = htons(random() % 100);
    TEST_CHECK(NKSN_AddRecord(session, 1, random() % 2 ? NKE_RECORD_ERROR : NKE_RECORD_WARNING,
                              data, length));
  } else if (index == 1) {
    TEST_CHECK(NKSN_AddRecord(session, 1, NKE_RECORD_ERROR + 1000, data, length));
  }

  if (index != 2) {
    if (index == 3)
      data[0] = htons(NKE_NEXT_PROTOCOL_NTPV4 + random() % 10 + 1);
    else
      data[0] = htons(NKE_NEXT_PROTOCOL_NTPV4);
    if (index == 4)
      length = 3 + random() % 10;
    TEST_CHECK(NKSN_AddRecord(session, 1, NKE_RECORD_NEXT_PROTOCOL, data, length));
  }

  if (index != 5) {
    if (index == 6)
      do {
        data[0] = htons(random() % 100);
      } while (SIV_GetKeyLength(ntohs(data[0])) > 0);
    else
      data[0] = htons(random() % 2 && SIV_GetKeyLength(AEAD_AES_128_GCM_SIV) > 0 ?
                                      AEAD_AES_128_GCM_SIV : AEAD_AES_SIV_CMAC_256);
    if (index == 7)
      length = 3 + random() % 10;
    TEST_CHECK(NKSN_AddRecord(session, 1, NKE_RECORD_AEAD_ALGORITHM, data, length));
  }

  if (random() % 2) {
    const char server[] = "127.0.0.1";
    TEST_CHECK(NKSN_AddRecord(session, 1, NKE_RECORD_NTPV4_SERVER_NEGOTIATION,
                              server, sizeof (server) - 1));
  }

  if (random() % 2) {
    data[0] = htons(123);
    TEST_CHECK(NKSN_AddRecord(session, 1, NKE_RECORD_NTPV4_PORT_NEGOTIATION, data, length));
  }

  if (random() % 2) {
    length = random() % (sizeof (data) + 1);
    TEST_CHECK(NKSN_AddRecord(session, 0, 1000 + random() % 1000, data, length));
  }

  if (index != 8) {
    for (i = 0; i < NKE_MAX_COOKIES; i++) {
      length = (random() % sizeof (data) + 1) / 4 * 4;
      if (index == 9)
        length += (length < sizeof (data) ? 1 : -1) * (random() % 3 + 1);
      TEST_CHECK(NKSN_AddRecord(session, 0, NKE_RECORD_COOKIE, data, length));
    }
  }

  TEST_CHECK(NKSN_EndMessage(session));
}

void
test_unit(void)
{
  NKC_Instance inst;
  IPSockAddr addr;
  int i, r, valid;

  char conf[][100] = {
    "nosystemcert",
  };

  CNF_Initialise(0, 0);
  for (i = 0; i < sizeof conf / sizeof conf[0]; i++)
    CNF_ParseLine(NULL, i + 1, conf[i]);

  LCL_Initialise();

  SCK_GetLoopbackIPAddress(AF_INET, &addr.ip_addr);
  addr.port = 0;

  inst = NKC_CreateInstance(&addr, "test", 0);
  TEST_CHECK(inst);

  for (i = 0; i < 10000; i++) {
    valid = random() % 2;
    prepare_response(inst->session, valid);
    r = process_response(inst);
    TEST_CHECK(r == valid);
  }

  NKC_DestroyInstance(inst);

  LCL_Finalise();
  CNF_Finalise();
}
#else
void
test_unit(void)
{
  TEST_REQUIRE(0);
}
#endif
