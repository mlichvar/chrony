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

  Routines implementing reference clocks.

  */

#include "refclock.h"
#include "conf.h"
#include "local.h"
#include "util.h"
#include "sources.h"
#include "logging.h"
#include "sched.h"

/* list of refclock drivers */
extern RefclockDriver RCL_SHM_driver;

struct RCL_Instance_Record {
  RefclockDriver *driver;
  void *data;
  int driver_parameter;
  int poll;
  int missed_samples;
  unsigned long ref_id;
  double offset;
  SCH_TimeoutID timeout_id;
  SRC_Instance source;
};

#define MAX_RCL_SOURCES 8

static struct RCL_Instance_Record refclocks[MAX_RCL_SOURCES];
static int n_sources = 0;

static void poll_timeout(void *arg);

void
RCL_Initialise(void)
{
  CNF_AddRefclocks();
}

void
RCL_Finalise(void)
{
  int i;

  for (i = 0; i < n_sources; i++) {
    RCL_Instance inst = (RCL_Instance)&refclocks[i];

    if (inst->driver->fini)
      inst->driver->fini(inst);
  }
}

int
RCL_AddRefclock(RefclockParameters *params)
{
  RCL_Instance inst = &refclocks[n_sources];

  if (n_sources == MAX_RCL_SOURCES)
    return 0;

  if (strncmp(params->driver_name, "SHM", 4) == 0) {
    inst->driver = &RCL_SHM_driver;
  } else {
    LOG_FATAL(LOGF_Refclock, "unknown refclock driver %s", params->driver_name);
    return 0;
  }

  inst->data = NULL;
  inst->driver_parameter = params->driver_parameter;
  inst->poll = params->poll;
  inst->missed_samples = 0;
  inst->offset = params->offset;
  inst->timeout_id = -1;
  inst->source = NULL;

  if (params->ref_id)
    inst->ref_id = params->ref_id;
  else {
    unsigned char ref[5] = { 0, 0, 0, 0, 0 };

    snprintf((char *)ref, 5, "%s%d", params->driver_name, params->driver_parameter);
    inst->ref_id = ref[0] << 24 | ref[1] << 16 | ref[2] << 8 | ref[3];
  }

  if (inst->driver->init)
    if (!inst->driver->init(inst)) {
      LOG_FATAL(LOGF_Refclock, "refclock %s initialisation failed", params->driver_name);
      return 0;
    }

#if 0
  LOG(LOGS_INFO, LOGF_Refclock, "refclock added");
#endif
  n_sources++;

  return 1;
}

void
RCL_StartRefclocks(void)
{
  int i;

  for (i = 0; i < n_sources; i++) {
    RCL_Instance inst = &refclocks[i];

    inst->source = SRC_CreateNewInstance(inst->ref_id, SRC_REFCLOCK);
    if (inst->driver->poll)
      inst->timeout_id = SCH_AddTimeoutByDelay(0.0, poll_timeout, (void *)inst);
  }
}

void
RCL_ReportSource(RPT_SourceReport *report, struct timeval *now)
{
  int i;
  unsigned long ref_id;

  ref_id = report->ip_addr;

  for (i = 0; i < n_sources; i++) {
    RCL_Instance inst = &refclocks[i];
    if (inst->ref_id == ref_id) {
      report->poll = inst->poll;
      report->mode = RPT_LOCAL_REFERENCE;
      break;
    }
  }
}

void
RCL_SetDriverData(RCL_Instance instance, void *data)
{
  instance->data = data;
}

void *
RCL_GetDriverData(RCL_Instance instance)
{
  return instance->data;
}

int
RCL_GetDriverParameter(RCL_Instance instance)
{
  return instance->driver_parameter;
}

int
RCL_AddSample(RCL_Instance instance, struct timeval *sample_time, double offset, NTP_Leap leap_status)
{
  double correction;
  struct timeval cooked_time;
  SRC_Instance inst = instance->source;

#if 0
  LOG(LOGS_INFO, LOGF_Refclock, "refclock offset: %f", offset);
#endif

  SRC_SetReachable(inst);

  correction = LCL_GetOffsetCorrection(sample_time);
  UTI_AddDoubleToTimeval(sample_time, correction, &cooked_time);

  SRC_AccumulateSample(inst, &cooked_time, offset - correction + instance->offset,
      1e-6, 0.0, 0.0, 0.0, 0, leap_status);

  instance->missed_samples = 0;

  return 1;
}

static void
poll_timeout(void *arg)
{
  double next;

  RCL_Instance inst = (RCL_Instance)arg;

  inst->missed_samples++;
  inst->driver->poll(inst);
  
  if (inst->missed_samples > 9)
    SRC_UnsetReachable(inst->source);

  if (inst->poll >= 0)
    next = 1 << inst->poll;
  else
    next = 1.0 / (1 << -inst->poll);

  inst->timeout_id = SCH_AddTimeoutByDelay(next, poll_timeout, arg);
}

