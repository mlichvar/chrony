/*
  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Richard P. Curnow  1997-2001
 * Copyright (C) J. Hannken-Illjes  2001
 * Copyright (C) Miroslav Lichvar  2015
 * Copyright (C) Shaun Ren  2021
 * Copyright (C) Thomas Kupper  2026
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

  Driver file for the OpenBSD operating system.
  */

#include "config.h"

#include "sysincl.h"

#include <sys/sysctl.h>

#include "sys_generic.h"
#include "sys_openbsd.h"
#include "conf.h"
#include "local.h"
#include "logging.h"
#include "privops.h"
#include "sched.h"
#include "util.h"

/* The OpenBSD kernel supports a maximum value of 500000 ppm.
   To avoid extending the range that would need to be tested, use
   the same maximum as on Linux.

   Maximum frequency offset (in ppm) */
#define MAX_FREQ 100000.0

/* RTC synchronisation - once an hour */

static struct timespec last_rtc_sync;
#define RTC_SYNC_INTERVAL (60 * 60.0)

/* ================================================== */

static double
read_frequency(void)
{
  int64_t freq;

  if (PRV_AdjustFreq(NULL, &freq))
    LOG_FATAL("adjfreq() failed");

  return (double)-freq / (1000LL << 32);
}

/* ================================================== */

static double
set_frequency(double freq_ppm)
{
  int64_t freq;

  freq = -freq_ppm * (1000LL << 32);
  if (PRV_AdjustFreq(&freq, NULL))
    LOG_FATAL("adjfreq() failed");

  return read_frequency();
}

/* ================================================== */

static void
synchronise_rtc(void)
{
  struct timespec ts, new_ts;
  double err;

  LCL_ReadRawTime(&ts);

  if (PRV_SetTime(CLOCK_REALTIME, &ts) < 0) {
    DEBUG_LOG("clock_settime() failed");
    return;
  }

  LCL_ReadRawTime(&new_ts);
  err = UTI_DiffTimespecsToDouble(&new_ts, &ts);

  lcl_InvokeDispersionNotifyHandlers(fabs(err));
}

/* ================================================== */

static void
set_sync_status(int synchronised, double est_error, double max_error)
{
  double rtc_sync_elapsed;
  struct timespec now;

  if (!synchronised || !CNF_GetRtcSync())
    return;

  SCH_GetLastEventTime(NULL, NULL, &now);
  rtc_sync_elapsed = UTI_DiffTimespecsToDouble(&now, &last_rtc_sync);
  if (fabs(rtc_sync_elapsed) >= RTC_SYNC_INTERVAL) {
    synchronise_rtc();
    last_rtc_sync = now;
    DEBUG_LOG("rtc synchronised");
  }
}


/* ================================================== */

static struct clockinfo
get_clockinfo(void)
{
  struct clockinfo cinfo;
  size_t cinfo_len;
  int mib[2];

  cinfo_len = sizeof (cinfo);
  mib[0] = CTL_KERN;
  mib[1] = KERN_CLOCKRATE;

  if (sysctl(mib, 2, &cinfo, &cinfo_len, NULL, 0) < 0)
    LOG_FATAL("sysctl() failed");

  return cinfo;
}

/* ================================================== */

static void
reset_adjtime_offset(void)
{
  struct timeval delta;

  memset(&delta, 0, sizeof (delta));

  if (PRV_AdjustTime(&delta, NULL))
    LOG_FATAL("adjtime() failed");
}

/* ================================================== */

/* PRV_SetTime() uses clock_settime() to set the system time.
   clock_setsetime() on OpenBSD is not pledged but
   settimeofday() is. Override clock_settime() here for
   OpenBSD and call settimeofday() from it. */

int
clock_settime(clockid_t clock, const struct timespec *now)
{
  struct timeval tv;

  if (clock != CLOCK_REALTIME)
    return -1;

  UTI_TimespecToTimeval(now, &tv);

  return settimeofday(&tv, NULL);
}

/* ================================================== */

void
SYS_OpenBSD_Initialise(void)
{
  struct clockinfo cinfo;

  cinfo = get_clockinfo();
  reset_adjtime_offset();

  LCL_ReadRawTime(&last_rtc_sync);

  SYS_Generic_CompleteFreqDriver(MAX_FREQ, 1.0 / cinfo.hz,
                                 read_frequency, set_frequency, NULL,
                                 0.0, 0.0,
                                 NULL, NULL,
                                 NULL, set_sync_status);
}

/* ================================================== */

void
SYS_OpenBSD_Finalise(void)
{
  SYS_Generic_Finalise();
}

/* ================================================== */

#ifdef FEAT_PRIVDROP
void
SYS_OpenBSD_DropRoot(uid_t uid, gid_t gid, SYS_ProcessContext context, int clock_control)
{
  if (context == SYS_MAIN_PROCESS)
    PRV_StartHelper();

  UTI_DropRoot(uid, gid);
}
#endif

/* ================================================== */

#ifdef FEAT_SCFILTER
void
SYS_OpenBSD_EnableSystemCallFilter(int level, SYS_ProcessContext context)
{
  int needs_inet = 0, needs_recvfd = 0, needs_sendfd = 0, needs_settime = 0;
  int needs_main_misc = 0, priv_bind;
  const char **certs, **keys;
  char promises[128];

  priv_bind = CNF_GetNTPPort() > 0 && CNF_GetNTPPort() < 1024;

  /* If level == 0, SYS_EnableSystemCallFilter() is not called.  Therefore
     only a value of 1 is valid here. */
  if (level != 1 && context == SYS_MAIN_PROCESS)
    /* Only log/fatal once in the main process, the child processes will be
       terminated too as a result */
    LOG_FATAL("Unsupported filter level");

  if (context == SYS_MAIN_PROCESS) {
    /* stdio        => allow libc stdio calls
       {r,w,c}path  => allow read/write/change config, drift file, etc
       inet         => allow connections to/from internet
       unix         => allow handling unix sockets
       dns          => allow DNS resolution
       sendfd       => allow sending fd to privops helper (for binding sockets
                       to privileged ports) and/or NTS-KE helper (in
                       NKS_Initialize() open_socket() -> accept_connection())
       settime      => allow set time if system call filter is enabled and user
                       is root (i.e. privops helper is not used) */
    needs_inet = 1;
    needs_main_misc = 1;

    if (geteuid() == 0)
      needs_settime = 1;

    if (priv_bind || (CNF_GetNtsServerCertAndKeyFiles(&certs, &keys) > 0 &&
                      CNF_GetNtsServerProcesses() > 0))
      needs_sendfd = 1;
  } else if (context == SYS_PRIVOPS_HELPER) {
    /* stdio        => allow libc stdio calls
       inet         => allow binding of sockets
       recvfd       => allow receiving fd from main process
       settime      => allow set/adjust time */
    needs_settime = 1;
    if (priv_bind) {
      needs_inet = 1;
      needs_recvfd = 1;
    }
  } else if (context == SYS_NTSKE_HELPER) {
    /* stdio        => allow libc stdio calls
       recvfd       => allow receiving fd from main process. In run_helper()
                       -> handle_helper_request() */
    needs_recvfd = 1;
  }

  if (snprintf(promises, sizeof (promises), "stdio%s%s%s%s%s",
               needs_inet ? " inet" : "",
               needs_main_misc ? " rpath wpath cpath unix dns" : "",
               needs_recvfd ? " recvfd" : "",
               needs_sendfd ? " sendfd" : "",
               needs_settime ? " settime" : "") >= sizeof (promises))
    assert(0);

  DEBUG_LOG("Pledging: %s", promises);

  if (pledge(promises, NULL) < 0)
    LOG_FATAL("pledge() failed");

  LOG(context == SYS_MAIN_PROCESS ? LOGS_INFO : LOGS_DEBUG, "Loaded pledge filter");
}

#endif
