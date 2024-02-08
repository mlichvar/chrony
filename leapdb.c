/*
  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Miroslav Lichvar  2009-2018, 2020, 2022
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

  This module provides leap second information. */

#include "config.h"

#include "sysincl.h"

#include "conf.h"
#include "leapdb.h"
#include "logging.h"

/* ================================================== */

/* Name of a system timezone containing leap seconds occuring at midnight */
static char *leap_tzname;

/* ================================================== */

static NTP_Leap
get_tz_leap(time_t when, int *tai_offset)
{
  struct tm stm, *tm;
  time_t t;
  char *tz_env, tz_orig[128];
  NTP_Leap tz_leap = LEAP_Normal;

  tm = gmtime(&when);
  if (!tm)
    return tz_leap;

  stm = *tm;

  /* Temporarily switch to the timezone containing leap seconds */
  tz_env = getenv("TZ");
  if (tz_env) {
    if (strlen(tz_env) >= sizeof (tz_orig))
      return tz_leap;
    strcpy(tz_orig, tz_env);
  }
  setenv("TZ", leap_tzname, 1);
  tzset();

  /* Get the TAI-UTC offset, which started at the epoch at 10 seconds */
  t = mktime(&stm);
  if (t != -1)
    *tai_offset = t - when + 10;

  /* Set the time to 23:59:60 and see how it overflows in mktime() */
  stm.tm_sec = 60;
  stm.tm_min = 59;
  stm.tm_hour = 23;

  t = mktime(&stm);

  if (tz_env)
    setenv("TZ", tz_orig, 1);
  else
    unsetenv("TZ");
  tzset();

  if (t == -1)
    return tz_leap;

  if (stm.tm_sec == 60)
    tz_leap = LEAP_InsertSecond;
  else if (stm.tm_sec == 1)
    tz_leap = LEAP_DeleteSecond;

  return tz_leap;
}

/* ================================================== */

void
LDB_Initialise(void)
{
  int tai_offset;

  leap_tzname = CNF_GetLeapSecTimezone();
  if (leap_tzname) {
    /* Check that the timezone has good data for Jun 30 2012 and Dec 31 2012 */
    if (get_tz_leap(1341014400, &tai_offset) == LEAP_InsertSecond && tai_offset == 34 &&
        get_tz_leap(1356912000, &tai_offset) == LEAP_Normal && tai_offset == 35) {
      LOG(LOGS_INFO, "Using %s timezone to obtain leap second data", leap_tzname);
    } else {
      LOG(LOGS_WARN, "Timezone %s failed leap second check, ignoring", leap_tzname);
      leap_tzname = NULL;
    }
  }
}

/* ================================================== */

NTP_Leap
LDB_GetLeap(time_t when, int *tai_offset)
{
  static time_t last_ldb_leap_check;
  static NTP_Leap ldb_leap;
  static int ldb_tai_offset;

  /* Do this check at most twice a day */
  when = when / (12 * 3600) * (12 * 3600);
  if (last_ldb_leap_check == when)
    goto out;

  last_ldb_leap_check = when;
  ldb_leap = LEAP_Normal;
  ldb_tai_offset = 0;

  if (leap_tzname)
    ldb_leap = get_tz_leap(when, &ldb_tai_offset);

out:
  *tai_offset = ldb_tai_offset;
  return ldb_leap;
}

/* ================================================== */

void
LDB_Finalise(void)
{
  /* Nothing to do */
}
