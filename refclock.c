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
#include "memory.h"
#include "util.h"
#include "sources.h"
#include "logging.h"
#include "sched.h"

/* list of refclock drivers */
extern RefclockDriver RCL_SHM_driver;
extern RefclockDriver RCL_SOCK_driver;

struct FilterSample {
  double offset;
  struct timeval sample_time;
};

struct MedianFilter {
  int length;
  int index;
  int used;
  struct FilterSample *samples;
};

struct RCL_Instance_Record {
  RefclockDriver *driver;
  void *data;
  char *driver_parameter;
  int driver_poll;
  int driver_polled;
  int poll;
  int missed_samples;
  int leap_status;
  struct MedianFilter filter;
  unsigned long ref_id;
  double offset;
  double delay;
  SCH_TimeoutID timeout_id;
  SRC_Instance source;
};

#define MAX_RCL_SOURCES 8

static struct RCL_Instance_Record refclocks[MAX_RCL_SOURCES];
static int n_sources = 0;

static void poll_timeout(void *arg);
static void slew_samples(struct timeval *raw, struct timeval *cooked, double dfreq, double afreq,
             double doffset, int is_step_change, void *anything);

static void filter_init(struct MedianFilter *filter, int length);
static void filter_fini(struct MedianFilter *filter);
static void filter_reset(struct MedianFilter *filter);
static void filter_add_sample(struct MedianFilter *filter, struct timeval *sample_time, double offset);
static int filter_get_sample(struct MedianFilter *filter, struct timeval *sample_time, double *offset, double *dispersion);
static void filter_slew_samples(struct MedianFilter *filter, struct timeval *when, double dfreq, double doffset);

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

    filter_fini(&inst->filter);
    Free(inst->driver_parameter);
  }

  if (n_sources > 0)
    LCL_RemoveParameterChangeHandler(slew_samples, NULL);
}

int
RCL_AddRefclock(RefclockParameters *params)
{
  RCL_Instance inst = &refclocks[n_sources];

  if (n_sources == MAX_RCL_SOURCES)
    return 0;

  if (strncmp(params->driver_name, "SHM", 4) == 0) {
    inst->driver = &RCL_SHM_driver;
  } else if (strncmp(params->driver_name, "SOCK", 4) == 0) {
    inst->driver = &RCL_SOCK_driver;
  } else {
    LOG_FATAL(LOGF_Refclock, "unknown refclock driver %s", params->driver_name);
    return 0;
  }

  inst->data = NULL;
  inst->driver_parameter = params->driver_parameter;
  inst->driver_poll = params->driver_poll;
  inst->poll = params->poll;
  inst->missed_samples = 0;
  inst->driver_polled = 0;
  inst->leap_status = 0;
  inst->offset = params->offset;
  inst->delay = params->delay;
  inst->timeout_id = -1;
  inst->source = NULL;

  if (inst->driver_poll > inst->poll)
    inst->driver_poll = inst->poll;

  if (params->ref_id)
    inst->ref_id = params->ref_id;
  else {
    unsigned char ref[5] = { 0, 0, 0, 0, 0 };

    snprintf((char *)ref, 5, "%3s%d", params->driver_name, n_sources % 10);
    inst->ref_id = ref[0] << 24 | ref[1] << 16 | ref[2] << 8 | ref[3];
  }

  if (inst->driver->init)
    if (!inst->driver->init(inst)) {
      LOG_FATAL(LOGF_Refclock, "refclock %s initialisation failed", params->driver_name);
      return 0;
    }

  filter_init(&inst->filter, params->filter_length);

#if 0
  LOG(LOGS_INFO, LOGF_Refclock, "refclock added poll=%d dpoll=%d filter=%d",
		  inst->poll, inst->driver_poll, params->filter_length);
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

    inst->source = SRC_CreateNewInstance(inst->ref_id, SRC_REFCLOCK, NULL);
    inst->timeout_id = SCH_AddTimeoutByDelay(0.0, poll_timeout, (void *)inst);
  }

  if (n_sources > 0)
    LCL_AddParameterChangeHandler(slew_samples, NULL);
}

void
RCL_ReportSource(RPT_SourceReport *report, struct timeval *now)
{
  int i;
  unsigned long ref_id;

  assert(report->ip_addr.family == IPADDR_INET4);
  ref_id = report->ip_addr.addr.in4;

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

char *
RCL_GetDriverParameter(RCL_Instance instance)
{
  return instance->driver_parameter;
}

int
RCL_AddSample(RCL_Instance instance, struct timeval *sample_time, double offset, NTP_Leap leap_status)
{
  double correction;
  struct timeval cooked_time;

  correction = LCL_GetOffsetCorrection(sample_time);
  UTI_AddDoubleToTimeval(sample_time, correction, &cooked_time);

#if 0
  LOG(LOGS_INFO, LOGF_Refclock, "refclock sample offset=%.9f cooked=%.9f",
      offset, offset - correction + instance->offset);
#endif

  filter_add_sample(&instance->filter, &cooked_time, offset - correction + instance->offset);
  instance->leap_status = leap_status;

  return 1;
}

static void
poll_timeout(void *arg)
{
  double next;
  int poll;

  RCL_Instance inst = (RCL_Instance)arg;

  poll = inst->poll;

  if (inst->driver->poll) {
    poll = inst->driver_poll;
    inst->driver->poll(inst);
    inst->driver_polled++;
  }
  
  if (!(inst->driver->poll && inst->driver_polled < (1 << (inst->poll - inst->driver_poll)))) {
    double offset, dispersion;
    struct timeval sample_time;
    int sample_ok;

    sample_ok = filter_get_sample(&inst->filter, &sample_time, &offset, &dispersion);
    filter_reset(&inst->filter);
    inst->driver_polled = 0;

    if (sample_ok) {
#if 0
      LOG(LOGS_INFO, LOGF_Refclock, "refclock filtered sample: offset=%.9f dispersion=%.9f [%s]",
          offset, dispersion, UTI_TimevalToString(&sample_time));
#endif
      SRC_SetReachable(inst->source);
      SRC_AccumulateSample(inst->source, &sample_time, offset,
          inst->delay, dispersion, inst->delay, dispersion, 0, inst->leap_status);
      inst->missed_samples = 0;
    } else {
      inst->missed_samples++;
      if (inst->missed_samples > 9)
        SRC_UnsetReachable(inst->source);
    }
  }

  if (poll >= 0)
    next = 1 << poll;
  else
    next = 1.0 / (1 << -poll);

  inst->timeout_id = SCH_AddTimeoutByDelay(next, poll_timeout, arg);
}

static void
slew_samples(struct timeval *raw, struct timeval *cooked, double dfreq, double afreq,
             double doffset, int is_step_change, void *anything)
{
  int i;

  for (i = 0; i < n_sources; i++)
    filter_slew_samples(&refclocks[i].filter, cooked, dfreq, doffset);
}

static void
filter_init(struct MedianFilter *filter, int length)
{
  if (length < 1)
    length = 1;

  filter->length = length;
  filter->index = -1;
  filter->used = 0;
  filter->samples = MallocArray(struct FilterSample, filter->length);
}

static void
filter_fini(struct MedianFilter *filter)
{
  Free(filter->samples);
}

static void
filter_reset(struct MedianFilter *filter)
{
  filter->index = -1;
  filter->used = 0;
}

static void
filter_add_sample(struct MedianFilter *filter, struct timeval *sample_time, double offset)
{
  filter->index++;
  filter->index %= filter->length;
  if (filter->used < filter->length)
    filter->used++;

  filter->samples[filter->index].sample_time = *sample_time;
  filter->samples[filter->index].offset = offset;
}

static int
sample_compare(const void *a, const void *b)
{
  const struct FilterSample *s1 = a, *s2 = b;

  if (s1->offset < s2->offset)
    return -1;
  else if (s1->offset > s2->offset)
    return 1;
  return 0;
}

static int
filter_get_sample(struct MedianFilter *filter, struct timeval *sample_time, double *offset, double *dispersion)
{
  if (filter->used == 0)
    return 0;

  if (filter->used == 1) {
    *sample_time = filter->samples[filter->index].sample_time;
    *offset = filter->samples[filter->index].offset;
    *dispersion = 0.0;
  } else {
    int i, from, to;
    double x, x1, y, d;

    /* sort samples by offset */
    qsort(filter->samples, filter->used, sizeof (struct FilterSample), sample_compare);

    /* average the half of the samples closest to the median */ 
    if (filter->used > 2) {
      from = (filter->used + 2) / 4;
      to = filter->used - from;
    } else {
      from = 0;
      to = filter->used;
    }

    for (i = from, x = y = 0.0; i < to; i++) {
#if 0
      LOG(LOGS_INFO, LOGF_Refclock, "refclock averaging offset %.9f [%s]",
          filter->samples[i].offset, UTI_TimevalToString(&filter->samples[i].sample_time));
#endif
      UTI_DiffTimevalsToDouble(&x1, &filter->samples[i].sample_time, &filter->samples[0].sample_time);
      x += x1;
      y += filter->samples[i].offset;
    }

    x /= to - from;
    y /= to - from;

    for (i = from, d = 0.0; i < to; i++)
      d += (filter->samples[i].offset - y) * (filter->samples[i].offset - y);

    d = sqrt(d / (to - from));

    UTI_AddDoubleToTimeval(&filter->samples[0].sample_time, x, sample_time);
    *offset = y;
    *dispersion = d;
  }

  return 1;
}

static void
filter_slew_samples(struct MedianFilter *filter, struct timeval *when, double dfreq, double doffset)
{
  int i;
  double elapsed, delta_time, prev_offset;
  struct timeval *sample;

  for (i = 0; i < filter->used; i++) {
    sample = &filter->samples[i].sample_time;

    UTI_DiffTimevalsToDouble(&elapsed, when, sample);
    delta_time = elapsed * dfreq - doffset;
    UTI_AddDoubleToTimeval(sample, delta_time, sample);

    prev_offset = filter->samples[i].offset;
    filter->samples[i].offset -= delta_time;
#if 0
    LOG(LOGS_INFO, LOGF_Refclock, "i=%d old_off=%.9f new_off=%.9f",
        i, prev_offset, filter->samples[i].offset);
#endif
  }
}
