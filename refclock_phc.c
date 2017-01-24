/*
  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Miroslav Lichvar  2013, 2017
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

#include "refclock.h"
#include "logging.h"
#include "memory.h"
#include "util.h"
#include "sys_linux.h"

struct phc_instance {
  int fd;
  int mode;
  int nocrossts;
};

static int phc_initialise(RCL_Instance instance)
{
  struct phc_instance *phc;
  int phc_fd;
  char *path;

  path = RCL_GetDriverParameter(instance);
 
  phc_fd = SYS_Linux_OpenPHC(path, 0);
  if (phc_fd < 0) {
    LOG_FATAL(LOGF_Refclock, "Could not open PHC");
    return 0;
  }

  phc = MallocNew(struct phc_instance);
  phc->fd = phc_fd;
  phc->mode = 0;
  phc->nocrossts = RCL_GetDriverOption(instance, "nocrossts") ? 1 : 0;

  RCL_SetDriverData(instance, phc);
  return 1;
}

static void phc_finalise(RCL_Instance instance)
{
  struct phc_instance *phc;

  phc = (struct phc_instance *)RCL_GetDriverData(instance);
  close(phc->fd);
  Free(phc);
}

static int phc_poll(RCL_Instance instance)
{
  struct phc_instance *phc;
  struct timespec phc_ts, sys_ts;
  double offset, err;

  phc = (struct phc_instance *)RCL_GetDriverData(instance);

  if (!SYS_Linux_GetPHCSample(phc->fd, phc->nocrossts, RCL_GetPrecision(instance),
                              &phc->mode, &phc_ts, &sys_ts, &err))
    return 0;

  offset = UTI_DiffTimespecsToDouble(&phc_ts, &sys_ts);

  DEBUG_LOG(LOGF_Refclock, "PHC offset: %+.9f err: %.9f", offset, err);

  return RCL_AddSample(instance, &sys_ts, offset, LEAP_Normal);
}

RefclockDriver RCL_PHC_driver = {
  phc_initialise,
  phc_finalise,
  phc_poll
};

#else

RefclockDriver RCL_PHC_driver = { NULL, NULL, NULL };

#endif
