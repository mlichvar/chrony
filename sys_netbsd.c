/*
  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Richard P. Curnow  1997-2001
 * Copyright (C) J. Hannken-Illjes  2001
 * Copyright (C) Miroslav Lichvar  2015
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

  Driver file for the NetBSD operating system.
  */

#include "config.h"

#include "sysincl.h"

#include "sys_netbsd.h"
#include "sys_timex.h"
#include "logging.h"
#include "util.h"

/* Maximum frequency offset accepted by the kernel (in ppm) */
#define MAX_FREQ 500.0

/* Minimum assumed rate at which the kernel updates the clock frequency */
#define MIN_TICK_RATE 100

/* Interval between kernel updates of the adjtime() offset */
#define ADJTIME_UPDATE_INTERVAL 1.0

/* Maximum adjtime() slew rate (in ppm) */
#define MAX_ADJTIME_SLEWRATE 5000.0

/* Minimum offset adjtime() slews faster than MAX_FREQ */
#define MIN_FASTSLEW_OFFSET 1.0

/* ================================================== */

/* Positive offset means system clock is fast of true time, therefore
   slew backwards */

static void
accrue_offset(double offset, double corr_rate)
{
  struct timeval newadj, oldadj;

  UTI_DoubleToTimeval(-offset, &newadj);

  if (adjtime(&newadj, &oldadj) < 0)
    LOG_FATAL(LOGF_SysNetBSD, "adjtime() failed");

  /* Add the old remaining adjustment if not zero */
  UTI_TimevalToDouble(&oldadj, &offset);
  if (offset != 0.0) {
    UTI_AddDoubleToTimeval(&newadj, offset, &newadj);
    if (adjtime(&newadj, NULL) < 0)
      LOG_FATAL(LOGF_SysNetBSD, "adjtime() failed");
  }
}

/* ================================================== */

static void
get_offset_correction(struct timeval *raw,
                      double *corr, double *err)
{
  struct timeval remadj;
  double adjustment_remaining;

  if (adjtime(NULL, &remadj) < 0)
    LOG_FATAL(LOGF_SysNetBSD, "adjtime() failed");

  UTI_TimevalToDouble(&remadj, &adjustment_remaining);

  *corr = adjustment_remaining;
  if (err) {
    if (*corr != 0.0)
      *err = 1.0e-6 * MAX_ADJTIME_SLEWRATE / ADJTIME_UPDATE_INTERVAL;
    else
      *err = 0.0;
  }
}

/* ================================================== */

void
SYS_NetBSD_Initialise(void)
{
  SYS_Timex_InitialiseWithFunctions(MAX_FREQ, 1.0 / MIN_TICK_RATE,
                                    NULL, NULL, NULL,
                                    MIN_FASTSLEW_OFFSET, MAX_ADJTIME_SLEWRATE,
                                    accrue_offset, get_offset_correction);
}

/* ================================================== */

void
SYS_NetBSD_Finalise(void)
{
  SYS_Timex_Finalise();
}

/* ================================================== */

#ifdef FEAT_PRIVDROP
void
SYS_NetBSD_DropRoot(uid_t uid, gid_t gid)
{
  int fd;

  if (setgroups(0, NULL))
    LOG_FATAL(LOGF_SysNetBSD, "setgroups() failed : %s", strerror(errno));

  if (setgid(gid))
    LOG_FATAL(LOGF_SysNetBSD, "setgid(%d) failed : %s", gid, strerror(errno));

  if (setuid(uid))
    LOG_FATAL(LOGF_SysNetBSD, "setuid(%d) failed : %s", uid, strerror(errno));

  DEBUG_LOG(LOGF_SysNetBSD, "Root dropped to uid %d gid %d", uid, gid);

  /* Check if we have write access to /dev/clockctl */
  fd = open("/dev/clockctl", O_WRONLY);
  if (fd < 0)
    LOG_FATAL(LOGF_SysNetBSD, "Can't write to /dev/clockctl");
  close(fd);
}
#endif
