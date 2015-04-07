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

  */

#include "config.h"

#include "sysincl.h"

#include "rtc.h"
#include "local.h"
#include "logging.h"
#include "conf.h"

#if defined LINUX && defined FEAT_RTC
#include "rtc_linux.h"
#endif /* defined LINUX */

/* ================================================== */

static int driver_initialised = 0;

static struct {
  int  (*init)(void);
  void (*fini)(void);
  int  (*time_pre_init)(void);
  void (*time_init)(void (*after_hook)(void*), void *anything);
  void (*start_measurements)(void);
  int  (*write_parameters)(void);
  int  (*get_report)(RPT_RTC_Report *report);
  int  (*trim)(void);
} driver =
{
#if defined LINUX && defined FEAT_RTC
  RTC_Linux_Initialise,
  RTC_Linux_Finalise,
  RTC_Linux_TimePreInit,
  RTC_Linux_TimeInit,
  RTC_Linux_StartMeasurements,
  RTC_Linux_WriteParameters,
  RTC_Linux_GetReport,
  RTC_Linux_Trim
#else
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL
#endif 
};
     
/* ================================================== */
/* Set the system clock to the time of last modification of driftfile
   if it's in the future */

static void
fallback_time_init(void)
{
  struct timeval now;
  struct stat buf;
  char *drift_file;

  drift_file = CNF_GetDriftFile();
  if (!drift_file)
    return;

  if (stat(drift_file, &buf))
    return;

  LCL_ReadCookedTime(&now, NULL);

  if (now.tv_sec < buf.st_mtime) {
    if (LCL_ApplyStepOffset(now.tv_sec - buf.st_mtime))
      LOG(LOGS_INFO, LOGF_Rtc, "System clock set from driftfile %s",
          drift_file);
  }
}

/* ================================================== */

void
RTC_Initialise(int initial_set)
{
  char *file_name;

  /* Do an initial read of the RTC and set the system time to it.  This
     is analogous to what /sbin/hwclock -s would do on Linux.  If that fails
     or RTC is not supported, set the clock to the time of the last
     modification of driftfile, so we at least get closer to the truth. */
  if (initial_set) {
    if (!driver.time_pre_init || !driver.time_pre_init()) {
      fallback_time_init();
    }
  }

  driver_initialised = 0;

  /* This is how we tell whether the user wants to load the RTC
     driver, if he is on a machine where it is an option. */
  file_name = CNF_GetRtcFile();

  if (file_name) {
    if (CNF_GetRtcSync()) {
      LOG_FATAL(LOGF_Rtc, "rtcfile directive cannot be used with rtcsync");
    }

    if (driver.init) {
      if ((driver.init)()) {
        driver_initialised = 1;
      }
    } else {
      LOG(LOGS_ERR, LOGF_Rtc, "RTC not supported on this operating system");
    }
  }
}

/* ================================================== */

void
RTC_Finalise(void)
{
  if (driver.fini) {
    (driver.fini)();
  }
}

/* ================================================== */
/* Start the processing to get a single measurement from the real time
   clock, and use it to trim the system time, based on knowing the
   drift rate of the RTC and the error the last time we set it.  The
   TimePreInit routine has already run, so we can be sure that the
   trim required is not *too* large.

   We are called with a hook to a function to be called after the
   initialisation is complete.  We also call this if we cannot do the
   initialisation. */

void
RTC_TimeInit(void (*after_hook)(void *), void *anything)
{
  if (driver_initialised) {
    (driver.time_init)(after_hook, anything);
  } else {
    (after_hook)(anything);
  }
}

/* ================================================== */
/* Start the RTC measurement process */

void
RTC_StartMeasurements(void)
{
  if (driver_initialised) {
    (driver.start_measurements)();
  }
  /* Benign if driver not present */
}

/* ================================================== */
/* Write RTC information out to RTC file.  Return 0 for success, 1 if
   RTC driver not running, or 2 if the file cannot be written. */

int
RTC_WriteParameters(void)
{
  if (driver_initialised) {
    return (driver.write_parameters)();
  } else {
    return RTC_ST_NODRV;
  }
}

/* ================================================== */

int
RTC_GetReport(RPT_RTC_Report *report)
{
  if (driver_initialised) {
    return (driver.get_report)(report);
  } else {
    return 0;
  }
}

/* ================================================== */

int
RTC_Trim(void)
{
  if (driver_initialised) {
    return (driver.trim)();
  } else {
    return 0;
  }
}

/* ================================================== */

