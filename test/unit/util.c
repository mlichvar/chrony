#include <util.c>
#include "test.h"

void test_unit(void) {
  NTP_int64 ntp_ts, ntp_fuzz;
  struct timespec ts, ts2;
  struct sockaddr_un sun;
  double x, y;
  Float f;
  int i, j, c;
  char buf[16], *s;

  for (i = -31; i < 31; i++) {
    x = pow(2.0, i);
    y = UTI_Log2ToDouble(i);
    TEST_CHECK(y / x > 0.99999 && y / x < 1.00001);
  }

  for (i = -89; i < 63; i++) {
    x = pow(2.0, i);
    y = UTI_FloatNetworkToHost(UTI_FloatHostToNetwork(x));
    TEST_CHECK(y / x > 0.99999 && y / x < 1.00001);
  }

  for (i = 0; i < 100000; i++) {
    x = TST_GetRandomDouble(-1000.0, 1000.0);
    y = UTI_FloatNetworkToHost(UTI_FloatHostToNetwork(x));
    TEST_CHECK(y / x > 0.99999 && y / x < 1.00001);

    UTI_GetRandomBytes(&f, sizeof (f));
    x = UTI_FloatNetworkToHost(f);
    TEST_CHECK(x > 0.0 || x <= 0.0);
  }

  TEST_CHECK(UTI_DoubleToNtp32(1.0) == htonl(65536));
  TEST_CHECK(UTI_DoubleToNtp32(0.0) == htonl(0));
  TEST_CHECK(UTI_DoubleToNtp32(1.0 / (65536.0)) == htonl(1));
  TEST_CHECK(UTI_DoubleToNtp32(1.000001 / (65536.0)) == htonl(2));
  TEST_CHECK(UTI_DoubleToNtp32(1.000001) == htonl(65537));
  TEST_CHECK(UTI_DoubleToNtp32(1000000) == htonl(0xffffffff));
  TEST_CHECK(UTI_DoubleToNtp32(-1.0) == htonl(0));

  ntp_ts.hi = htonl(JAN_1970);
  ntp_ts.lo = 0xffffffff;
  UTI_Ntp64ToTimespec(&ntp_ts, &ts);
  TEST_CHECK(ts.tv_sec == 0);
  TEST_CHECK(ts.tv_nsec == 999999999);

  UTI_AddDoubleToTimespec(&ts, 1e-9, &ts);
  TEST_CHECK(ts.tv_sec == 1);
  TEST_CHECK(ts.tv_nsec == 0);

  ntp_fuzz.hi = 0;
  ntp_fuzz.lo = htonl(0xff1234ff);

  UTI_TimespecToNtp64(&ts, &ntp_ts, &ntp_fuzz);
  TEST_CHECK(ntp_ts.hi == htonl(JAN_1970 + 1));
  TEST_CHECK(ntp_ts.lo == ntp_fuzz.lo);

  ts.tv_sec = ts.tv_nsec = 0;
  UTI_TimespecToNtp64(&ts, &ntp_ts, &ntp_fuzz);
  TEST_CHECK(ntp_ts.hi == 0);
  TEST_CHECK(ntp_ts.lo == 0);

  TEST_CHECK(UTI_IsZeroTimespec(&ts));
  TEST_CHECK(UTI_IsZeroNtp64(&ntp_ts));

  ts.tv_sec = 1;
  ntp_ts.hi = htonl(1);

  TEST_CHECK(!UTI_IsZeroTimespec(&ts));
  TEST_CHECK(!UTI_IsZeroNtp64(&ntp_ts));

  ts.tv_sec = 0;
  ntp_ts.hi = 0;
  ts.tv_nsec = 1;
  ntp_ts.lo = htonl(1);

  TEST_CHECK(!UTI_IsZeroTimespec(&ts));
  TEST_CHECK(!UTI_IsZeroNtp64(&ntp_ts));

  ntp_ts.hi = 0;
  ntp_ts.lo = 0;

  UTI_Ntp64ToTimespec(&ntp_ts, &ts);
  TEST_CHECK(UTI_IsZeroTimespec(&ts));
  UTI_TimespecToNtp64(&ts, &ntp_ts, NULL);
  TEST_CHECK(UTI_IsZeroNtp64(&ntp_ts));

  ntp_fuzz.hi = htonl(1);
  ntp_fuzz.lo = htonl(3);
  ntp_ts.hi = htonl(1);
  ntp_ts.lo = htonl(2);

  TEST_CHECK(UTI_CompareNtp64(&ntp_ts, &ntp_ts) == 0);
  TEST_CHECK(UTI_CompareNtp64(&ntp_ts, &ntp_fuzz) < 0);
  TEST_CHECK(UTI_CompareNtp64(&ntp_fuzz, &ntp_ts) > 0);

  ntp_ts.hi = htonl(0x80000002);
  ntp_ts.lo = htonl(2);

  TEST_CHECK(UTI_CompareNtp64(&ntp_ts, &ntp_ts) == 0);
  TEST_CHECK(UTI_CompareNtp64(&ntp_ts, &ntp_fuzz) < 0);
  TEST_CHECK(UTI_CompareNtp64(&ntp_fuzz, &ntp_ts) > 0);

  ntp_fuzz.hi = htonl(0x90000001);

  TEST_CHECK(UTI_CompareNtp64(&ntp_ts, &ntp_ts) == 0);
  TEST_CHECK(UTI_CompareNtp64(&ntp_ts, &ntp_fuzz) < 0);
  TEST_CHECK(UTI_CompareNtp64(&ntp_fuzz, &ntp_ts) > 0);

  ts.tv_sec = 1;
  ts.tv_nsec = 2;
  ts2.tv_sec = 1;
  ts2.tv_nsec = 3;

  TEST_CHECK(UTI_CompareTimespecs(&ts, &ts) == 0);
  TEST_CHECK(UTI_CompareTimespecs(&ts, &ts2) < 0);
  TEST_CHECK(UTI_CompareTimespecs(&ts2, &ts) > 0);

  ts2.tv_sec = 2;

  TEST_CHECK(UTI_CompareTimespecs(&ts, &ts) == 0);
  TEST_CHECK(UTI_CompareTimespecs(&ts, &ts2) < 0);
  TEST_CHECK(UTI_CompareTimespecs(&ts2, &ts) > 0);

  for (i = -32; i <= 32; i++) {
    for (j = c = 0; j < 1000; j++) {
      UTI_GetNtp64Fuzz(&ntp_fuzz, i);
      if (i <= 0)
        TEST_CHECK(ntp_fuzz.hi == 0);
      if (i < 0)
        TEST_CHECK(ntohl(ntp_fuzz.lo) < 1U << (32 + i));
      else if (i < 32)
        TEST_CHECK(ntohl(ntp_fuzz.hi) < 1U << i);
      if (ntohl(ntp_fuzz.lo) >= 1U << (31 + CLAMP(-31, i, 0)))
        c++;
    }

    if (i == -32)
      TEST_CHECK(c == 0);
    else
      TEST_CHECK(c > 400 && c < 600);
  }

  for (i = c = 0; i < 100000; i++) {
    j = random() % (sizeof (buf) + 1);
    UTI_GetRandomBytes(buf, j);
    if (j && buf[j - 1] % 2)
      c++;
  }
  TEST_CHECK(c > 46000 && c < 48000);

  for (i = 1; i < 2 * BUFFER_LENGTH; i++) {
    sun.sun_family = AF_UNIX;
    for (j = 0; j + 1 < i && j + 1 < sizeof (sun.sun_path); j++)
      sun.sun_path[j] = 'A' + j % 26;
    sun.sun_path[j] = '\0';
    s = UTI_SockaddrToString((struct sockaddr *)&sun);
    if (i <= BUFFER_LENGTH) {
      TEST_CHECK(!strcmp(s, sun.sun_path));
    } else {
      TEST_CHECK(!strncmp(s, sun.sun_path, BUFFER_LENGTH - 2));
      TEST_CHECK(s[BUFFER_LENGTH - 2] == '>');
    }
  }
}
