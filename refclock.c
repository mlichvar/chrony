/*
  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Miroslav Lichvar  2009
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

  Routines implementing reference clocks.

  */

#include "refclock.h"
#include "reference.h"
#include "conf.h"
#include "local.h"
#include "memory.h"
#include "util.h"
#include "sources.h"
#include "logging.h"
#include "sched.h"
#include "mkdirpp.h"

/* list of refclock drivers */
extern RefclockDriver RCL_SHM_driver;
extern RefclockDriver RCL_SOCK_driver;
extern RefclockDriver RCL_PPS_driver;

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
  int pps_rate;
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

#define REFCLOCKS_LOG "refclocks.log"
static FILE *logfile = NULL;
static char *logfilename = NULL;
static unsigned long logwrites = 0;

static int valid_sample_time(RCL_Instance instance, struct timeval *tv);
static int pps_stratum(RCL_Instance instance, struct timeval *tv);
static void poll_timeout(void *arg);
static void slew_samples(struct timeval *raw, struct timeval *cooked, double dfreq, double afreq,
             double doffset, int is_step_change, void *anything);
static void log_sample(RCL_Instance instance, struct timeval *sample_time, int pulse, double raw_offset, double cooked_offset);

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

  if (CNF_GetLogRefclocks()) {
    char *logdir = CNF_GetLogDir();
    if (!mkdir_and_parents(logdir)) {
      LOG(LOGS_ERR, LOGF_Refclock, "Could not create directory %s", logdir);
    } else {
      logfilename = MallocArray(char, 2 + strlen(logdir) + strlen(REFCLOCKS_LOG));
      strcpy(logfilename, logdir);
      strcat(logfilename, "/");
      strcat(logfilename, REFCLOCKS_LOG);
    }
  }
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

  if (logfile)
    fclose(logfile);
  Free(logfilename);
}

int
RCL_AddRefclock(RefclockParameters *params)
{
  int pps_source = 0;

  RCL_Instance inst = &refclocks[n_sources];

  if (n_sources == MAX_RCL_SOURCES)
    return 0;

  if (strncmp(params->driver_name, "SHM", 4) == 0) {
    inst->driver = &RCL_SHM_driver;
  } else if (strncmp(params->driver_name, "SOCK", 4) == 0) {
    inst->driver = &RCL_SOCK_driver;
  } else if (strncmp(params->driver_name, "PPS", 4) == 0) {
    inst->driver = &RCL_PPS_driver;
    pps_source = 1;
  } else {
    LOG_FATAL(LOGF_Refclock, "unknown refclock driver %s", params->driver_name);
    return 0;
  }

  if (!inst->driver->init && !inst->driver->poll) {
    LOG_FATAL(LOGF_Refclock, "refclock driver %s is not compiled in", params->driver_name);
    return 0;
  }

  inst->data = NULL;
  inst->driver_parameter = params->driver_parameter;
  inst->driver_poll = params->driver_poll;
  inst->poll = params->poll;
  inst->missed_samples = 0;
  inst->driver_polled = 0;
  inst->leap_status = 0;
  inst->pps_rate = params->pps_rate;
  inst->offset = params->offset;
  inst->delay = params->delay;
  inst->timeout_id = -1;
  inst->source = NULL;

  if (pps_source) {
    if (inst->pps_rate < 1)
      inst->pps_rate = 1;
  } else {
    inst->pps_rate = 0;
  }

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

  if (!valid_sample_time(instance, sample_time))
    return 0;

#if 0
  LOG(LOGS_INFO, LOGF_Refclock, "refclock sample offset=%.9f cooked=%.9f",
      offset, offset - correction + instance->offset);
#endif

  filter_add_sample(&instance->filter, &cooked_time, offset - correction + instance->offset);
  instance->leap_status = leap_status;

  log_sample(instance, &cooked_time, 0, offset, offset - correction + instance->offset);

  return 1;
}

int
RCL_AddPulse(RCL_Instance instance, struct timeval *pulse_time, double second)
{
  double correction, offset;
  struct timeval cooked_time;
  int rate;

  struct timeval ref_time;
  int is_synchronised, stratum;
  double root_delay, root_dispersion, distance;
  NTP_Leap leap;
  unsigned long ref_id;

  correction = LCL_GetOffsetCorrection(pulse_time);
  UTI_AddDoubleToTimeval(pulse_time, correction, &cooked_time);

  if (!valid_sample_time(instance, pulse_time))
    return 0;

  rate = instance->pps_rate;
  assert(rate > 0);

  /* Ignore the pulse if we are not well synchronized */

  REF_GetReferenceParams(&cooked_time, &is_synchronised, &leap, &stratum,
      &ref_id, &ref_time, &root_delay, &root_dispersion);
  distance = fabs(root_delay) / 2 + root_dispersion;

  if (!is_synchronised || distance >= 0.5 / rate) {
#if 0
    LOG(LOGS_INFO, LOGF_Refclock, "refclock pulse dropped second=%.9f sync=%d dist=%.9f",
        second, is_synchronised, distance);
#endif
    return 0;
  }

  offset = -second - correction + instance->offset;

  /* Adjust the offset to [-0.5/rate, 0.5/rate) interval */
  offset -= (long)(offset * rate) / (double)rate;
  if (offset < -0.5 / rate)
    offset += 1.0 / rate;
  else if (offset >= 0.5 / rate)
    offset -= 1.0 / rate;

#if 0
  LOG(LOGS_INFO, LOGF_Refclock, "refclock pulse second=%.9f offset=%.9f",
      second, offset + instance->offset);
#endif

  filter_add_sample(&instance->filter, &cooked_time, offset);
  instance->leap_status = LEAP_Normal;

  log_sample(instance, &cooked_time, 1, second, offset);

  return 1;
}

void
RCL_CycleLogFile(void)
{
  if (logfile) {
    fclose(logfile);
    logfile = NULL;
    logwrites = 0;
  }
}

static int
valid_sample_time(RCL_Instance instance, struct timeval *tv)
{
  struct timeval raw_time;
  double diff;

  LCL_ReadRawTime(&raw_time);
  UTI_DiffTimevalsToDouble(&diff, &raw_time, tv);
  if (diff < 0.0 || diff > 1 << (instance->poll + 1))
    return 0;
  return 1;
}

static int
pps_stratum(RCL_Instance instance, struct timeval *tv)
{
  struct timeval ref_time;
  int is_synchronised, stratum, i;
  double root_delay, root_dispersion;
  NTP_Leap leap;
  unsigned long ref_id;

  REF_GetReferenceParams(tv, &is_synchronised, &leap, &stratum,
      &ref_id, &ref_time, &root_delay, &root_dispersion);

  /* Don't change our stratum if local stratum is active
     or this is the current source */
  if (ref_id == instance->ref_id || REF_IsLocalActive())
    return stratum - 1;

  /* Or the current source is another PPS refclock */ 
  for (i = 0; i < n_sources; i++) {
    if (refclocks[i].ref_id == ref_id && refclocks[i].pps_rate)
      return stratum - 1;
  }

  return 0;
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
    int sample_ok, stratum;

    sample_ok = filter_get_sample(&inst->filter, &sample_time, &offset, &dispersion);
    filter_reset(&inst->filter);
    inst->driver_polled = 0;

    if (sample_ok) {
#if 0
      LOG(LOGS_INFO, LOGF_Refclock, "refclock filtered sample: offset=%.9f dispersion=%.9f [%s]",
          offset, dispersion, UTI_TimevalToString(&sample_time));
#endif

      if (inst->pps_rate)
        /* Handle special case when PPS is used with local stratum */
        stratum = pps_stratum(inst, &sample_time);
      else
        stratum = 0;

      SRC_SetReachable(inst->source);
      SRC_AccumulateSample(inst->source, &sample_time, offset,
          inst->delay, dispersion, inst->delay, dispersion, stratum, inst->leap_status);
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
log_sample(RCL_Instance instance, struct timeval *sample_time, int pulse, double raw_offset, double cooked_offset)
{
  char sync_stats[4] = {'N', '+', '-', '?'};

  if (!logfilename)
    return;

  if (!logfile) {
      logfile = fopen(logfilename, "a");
      if (!logfile) {
        LOG(LOGS_WARN, LOGF_Refclock, "Couldn't open logfile %s for update", logfilename);
        Free(logfilename);
        logfilename = NULL;
        return;
      }
  }

  if (((logwrites++) % 32) == 0) {
    fprintf(logfile,
        "====================================================================\n"
        "   Date (UTC) Time         Refid  DP L P  Raw offset   Cooked offset\n"
        "====================================================================\n");
  }
  fprintf(logfile, "%s.%06d %-5s %3d %1c %1d %13.6e %13.6e\n",
      UTI_TimeToLogForm(sample_time->tv_sec),
      (int)sample_time->tv_usec,
      UTI_RefidToString(instance->ref_id),
      instance->driver_polled,
      sync_stats[instance->leap_status],
      pulse,
      raw_offset,
      cooked_offset);
  fflush(logfile);
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
