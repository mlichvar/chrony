/*
 **********************************************************************
 * Copyright (C) Miroslav Lichvar  2016
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

#include <clientlog.c>
#include "test.h"

void
test_unit(void)
{
  int i, j, index;
  struct timeval tv;
  IPAddr ip;
  char conf[][100] = {
    "clientloglimit 10000",
    "ratelimit interval 3 burst 4 leak 3",
    "cmdratelimit interval 3 burst 4 leak 3",
  };

  CNF_Initialise(0);
  for (i = 0; i < sizeof conf / sizeof conf[0]; i++)
    CNF_ParseLine(NULL, i + 1, conf[i]);

  CLG_Initialise();

  TEST_CHECK(ARR_GetSize(records) == 16);

  for (i = 0; i < 500; i++) {
    DEBUG_LOG(0, "iteration %d", i);

    tv.tv_sec = (time_t)random() & 0x0fffffff;
    tv.tv_usec = 0;

    for (j = 0; j < 1000; j++) {
      get_random_address(&ip, IPADDR_UNSPEC, i % 8 ? -1 : i / 8 % 9);
      DEBUG_LOG(0, "address %s", UTI_IPToString(&ip));

      if (random() % 2) {
        index = CLG_LogNTPAccess(&ip, &tv);
        TEST_CHECK(index >= 0);
        CLG_LimitNTPResponseRate(index);
      } else {
        index = CLG_LogCommandAccess(&ip, &tv);
        TEST_CHECK(index >= 0);
        CLG_LimitCommandResponseRate(index);
      }

      UTI_AddDoubleToTimeval(&tv, (1 << random() % 14) / 100.0, &tv);
    }
  }

  DEBUG_LOG(0, "records %d", ARR_GetSize(records));
  TEST_CHECK(ARR_GetSize(records) == 128);

  for (i = j = 0; i < 10000; i++) {
    tv.tv_sec += 1;
    index = CLG_LogNTPAccess(&ip, &tv);
    TEST_CHECK(index >= 0);
    if (!CLG_LimitNTPResponseRate(index))
      j++;
  }

  DEBUG_LOG(0, "requests %u responses %u", i, j);
  TEST_CHECK(j * 4 < i && j * 6 > i);

  CLG_Finalise();
  CNF_Finalise();
}
