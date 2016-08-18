/*
  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Miroslav Lichvar  2013
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

  PTP hardware clock (PHC) refclock driver.

  */

#include "config.h"

#include "refclock.h"

#ifdef FEAT_PHC

#include "sysincl.h"

#include <linux/ptp_clock.h>

#include "refclock.h"
#include "logging.h"
#include "util.h"

/* From linux/include/linux/posix-timers.h */
#define CPUCLOCK_MAX            3
#define CLOCKFD                 CPUCLOCK_MAX
#define CLOCKFD_MASK            (CPUCLOCK_PERTHREAD_MASK|CPUCLOCK_CLOCK_MASK)

#define FD_TO_CLOCKID(fd)       ((~(clockid_t) (fd) << 3) | CLOCKFD)

#define NUM_READINGS 10

static int no_sys_offset_ioctl = 0;

struct phc_reading {
  struct timespec sys_ts1;
  struct timespec phc_ts;;
  struct timespec sys_ts2;
};

static int read_phc_ioctl(struct phc_reading *readings, int phc_fd, int n)
{
#if defined(PTP_SYS_OFFSET) && NUM_READINGS <= PTP_MAX_SAMPLES
  struct ptp_sys_offset sys_off;
  int i;

  /* Silence valgrind */
  memset(&sys_off, 0, sizeof (sys_off));

  sys_off.n_samples = n;
  if (ioctl(phc_fd, PTP_SYS_OFFSET, &sys_off)) {
    LOG(LOGS_ERR, LOGF_Refclock, "ioctl(PTP_SYS_OFFSET) failed : %s", strerror(errno));
    return 0;
  }

  for (i = 0; i < n; i++) {
    readings[i].sys_ts1.tv_sec = sys_off.ts[i * 2].sec;
    readings[i].sys_ts1.tv_nsec = sys_off.ts[i * 2].nsec;
    readings[i].phc_ts.tv_sec = sys_off.ts[i * 2 + 1].sec;
    readings[i].phc_ts.tv_nsec = sys_off.ts[i * 2 + 1].nsec;
    readings[i].sys_ts2.tv_sec = sys_off.ts[i * 2 + 2].sec;
    readings[i].sys_ts2.tv_nsec = sys_off.ts[i * 2 + 2].nsec;
  }

  return 1;
#else
  /* Not available */
  return 0;
#endif
}

static int read_phc_user(struct phc_reading *readings, int phc_fd, int n)
{
  clockid_t phc_id;
  int i;

  phc_id = FD_TO_CLOCKID(phc_fd);

  for (i = 0; i < n; i++) {
    if (clock_gettime(CLOCK_REALTIME, &readings[i].sys_ts1) ||
        clock_gettime(phc_id, &readings[i].phc_ts) ||
        clock_gettime(CLOCK_REALTIME, &readings[i].sys_ts2)) {
      LOG(LOGS_ERR, LOGF_Refclock, "clock_gettime() failed : %s", strerror(errno));
      return 0;
    }
  }

  return 1;
}

static int phc_initialise(RCL_Instance instance)
{
  struct ptp_clock_caps caps;
  int phc_fd;
  char *path;

  path = RCL_GetDriverParameter(instance);
 
  phc_fd = open(path, O_RDONLY);
  if (phc_fd < 0) {
    LOG_FATAL(LOGF_Refclock, "open() failed on %s", path);
    return 0;
  }

  /* Make sure it is a PHC */
  if (ioctl(phc_fd, PTP_CLOCK_GETCAPS, &caps)) {
    LOG_FATAL(LOGF_Refclock, "ioctl(PTP_CLOCK_GETCAPS) failed : %s", strerror(errno));
    return 0;
  }

  UTI_FdSetCloexec(phc_fd);

  RCL_SetDriverData(instance, (void *)(long)phc_fd);
  return 1;
}

static void phc_finalise(RCL_Instance instance)
{
  close((long)RCL_GetDriverData(instance));
}

static int phc_poll(RCL_Instance instance)
{
  struct phc_reading readings[NUM_READINGS];
  double offset = 0.0, delay, best_delay = 0.0;
  int i, phc_fd, best;
 
  phc_fd = (long)RCL_GetDriverData(instance);

  if (!no_sys_offset_ioctl) {
    if (!read_phc_ioctl(readings, phc_fd, NUM_READINGS)) {
      no_sys_offset_ioctl = 1;
      return 0;
    }
  } else {
    if (!read_phc_user(readings, phc_fd, NUM_READINGS))
      return 0;
  }

  /* Find the fastest reading */
  for (i = 0; i < NUM_READINGS; i++) {
    delay = UTI_DiffTimespecsToDouble(&readings[i].sys_ts2, &readings[i].sys_ts1);

    if (!i || best_delay > delay) {
      best = i;
      best_delay = delay;
    }
  }

  offset = UTI_DiffTimespecsToDouble(&readings[best].phc_ts, &readings[best].sys_ts2) +
           best_delay / 2.0;

  DEBUG_LOG(LOGF_Refclock, "PHC offset: %+.9f delay: %.9f", offset, best_delay);

  return RCL_AddSample(instance, &readings[best].sys_ts2, offset, LEAP_Normal);
}

RefclockDriver RCL_PHC_driver = {
  phc_initialise,
  phc_finalise,
  phc_poll
};

#else

RefclockDriver RCL_PHC_driver = { NULL, NULL, NULL };

#endif
