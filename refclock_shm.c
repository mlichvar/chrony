/*
  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Richard P. Curnow  2009
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
 * 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA
 * 
 **********************************************************************

  =======================================================================

  SHM refclock driver.

  */

#include "refclock.h"
#include "logging.h"
#include "util.h"

#include <sys/types.h>
#include <sys/shm.h>

#define SHMKEY 0x4e545030

struct shmTime {
  int    mode; /* 0 - if valid set
                *       use values, 
                *       clear valid
                * 1 - if valid set 
                *       if count before and after read of values is equal,
                *         use values 
                *       clear valid
                */
  int    count;
  time_t clockTimeStampSec;
  int    clockTimeStampUSec;
  time_t receiveTimeStampSec;
  int    receiveTimeStampUSec;
  int    leap;
  int    precision;
  int    nsamples;
  int    valid;
  int    dummy[10]; 
};

static int shm_initialise(RCL_Instance instance) {
  int id, param;
  struct shmTime *shm;

  param = atoi(RCL_GetDriverParameter(instance));

  id = shmget(SHMKEY + param, sizeof (struct shmTime), IPC_CREAT | 0700);
  if (id == -1) {
    LOG_FATAL(LOGF_Refclock, "shmget() failed");
    return 0;
  }
   
  shm = (struct shmTime *)shmat(id, 0, 0);
  if ((long)shm == -1) {
    LOG_FATAL(LOGF_Refclock, "shmat() failed");
    return 0;
  }

  RCL_SetDriverData(instance, shm);
  return 1;
}

static void shm_finalise(RCL_Instance instance)
{
  shmdt(RCL_GetDriverData(instance));
}

static int shm_poll(RCL_Instance instance)
{
  struct timeval tv1, tv2;
  struct shmTime t, *shm;
  double offset;

  shm = (struct shmTime *)RCL_GetDriverData(instance);

  t = *shm;
  
  if ((t.mode == 1 && t.count != shm->count) ||
    !(t.mode == 0 || t.mode == 1) || !t.valid) {
#if 0
    LOG(LOGS_INFO, LOGF_Refclock, "sample ignored mode: %d count: %d valid: %d", t.mode, t.count, t.valid);
#endif
    return 0;
  }

  shm->valid = 0;

  tv1.tv_sec = t.receiveTimeStampSec;
  tv1.tv_usec = t.receiveTimeStampUSec;
  tv2.tv_sec = t.clockTimeStampSec;
  tv2.tv_usec = t.clockTimeStampUSec;

  UTI_DiffTimevalsToDouble(&offset, &tv2, &tv1);
  return RCL_AddSample(instance, &tv1, offset, t.leap);
}

RefclockDriver RCL_SHM_driver = {
  shm_initialise,
  shm_finalise,
  shm_poll
};
