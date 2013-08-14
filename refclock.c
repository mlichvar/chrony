/*
  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Miroslav Lichvar  2009-2011
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

#include "config.h"

#include "refclock.h"
#include "reference.h"
#include "conf.h"
#include "local.h"
#include "memory.h"
#include "util.h"
#include "sources.h"
#include "logging.h"
#include "regress.h"
#include "sched.h"

/* list of refclock drivers */
extern RefclockDriver RCL_SHM_driver;
extern RefclockDriver RCL_SOCK_driver;
extern RefclockDriver RCL_PPS_driver;
extern RefclockDriver RCL_PHC_driver;

struct FilterSample {
  double offset;
  double dispersion;
  struct timeval sample_time;
};

struct MedianFilter {
  int length;
  int index;
  int used;
  int last;
  int avg_var_n;
  double avg_var;
  struct FilterSample *samples;
  int *selected;
  double *x_data;
  double *y_data;
  double *w_data;
};

struct RCL_Instance_Record {
  RefclockDriver *driver;
  void *data;
  char *driver_parameter;
  int driver_parameter_length;
  int driver_poll;
  int driver_polled;
  int poll;
  int leap_status;
  int pps_rate;
  struct MedianFilter filter;
  uint32_t ref_id;
  uint32_t lock_ref;
  double offset;
  double delay;
  double precision;
  SCH_TimeoutID timeout_id;
  SRC_Instance source;
};

#define MAX_RCL_SOURCES 8

static struct RCL_Instance_Record refclocks[MAX_RCL_SOURCES];
static int n_sources = 0;

static LOG_FileID logfileid;

static int valid_sample_time(RCL_Instance instance, struct timeval *tv);
static int pps_stratum(RCL_Instance instance, struct timeval *tv);
static void poll_timeout(void *arg);
static void slew_samples(struct timeval *raw, struct timeval *cooked, double dfreq,
             double doffset, int is_step_change, void *anything);
static void add_dispersion(double dispersion, void *anything);
static void log_sample(RCL_Instance instance, struct timeval *sample_time, int filtered, int pulse, double raw_offset, double cooked_offset, double dispersion);

static void filter_init(struct MedianFilter *filter, int length);
static void filter_fini(struct MedianFilter *filter);
static void filter_reset(struct MedianFilter *filter);
static double filter_get_avg_sample_dispersion(struct MedianFilter *filter);
static void filter_add_sample(struct MedianFilter *filter, struct timeval *sample_time, double offset, double dispersion);
static int filter_get_last_sample(struct MedianFilter *filter, struct timeval *sample_time, double *offset, double *dispersion);
static int filter_select_samples(struct MedianFilter *filter);
static int filter_get_sample(struct MedianFilter *filter, struct timeval *sample_time, double *offset, double *dispersion);
static void filter_slew_samples(struct MedianFilter *filter, struct timeval *when, double dfreq, double doffset);
static void filter_add_dispersion(struct MedianFilter *filter, double dispersion);

void
RCL_Initialise(void)
{
  CNF_AddRefclocks();

  if (n_sources > 0) {
    LCL_AddParameterChangeHandler(slew_samples, NULL);
    LCL_AddDispersionNotifyHandler(add_dispersion, NULL);
  }

  logfileid = CNF_GetLogRefclocks() ? LOG_FileOpen("refclocks",
      "   Date (UTC) Time         Refid  DP L P  Raw offset   Cooked offset      Disp.")
    : -1;
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

  if (n_sources > 0) {
    LCL_RemoveParameterChangeHandler(slew_samples, NULL);
    LCL_RemoveDispersionNotifyHandler(add_dispersion, NULL);
  }
}

int
RCL_AddRefclock(RefclockParameters *params)
{
  int pps_source = 0;

  RCL_Instance inst = &refclocks[n_sources];

  if (n_sources == MAX_RCL_SOURCES)
    return 0;

  if (strcmp(params->driver_name, "SHM") == 0) {
    inst->driver = &RCL_SHM_driver;
    inst->precision = 1e-6;
  } else if (strcmp(params->driver_name, "SOCK") == 0) {
    inst->driver = &RCL_SOCK_driver;
    inst->precision = 1e-9;
    pps_source = 1;
  } else if (strcmp(params->driver_name, "PPS") == 0) {
    inst->driver = &RCL_PPS_driver;
    inst->precision = 1e-9;
    pps_source = 1;
  } else if (strcmp(params->driver_name, "PHC") == 0) {
    inst->driver = &RCL_PHC_driver;
    inst->precision = 1e-9;
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
  inst->driver_parameter_length = 0;
  inst->driver_poll = params->driver_poll;
  inst->poll = params->poll;
  inst->driver_polled = 0;
  inst->leap_status = LEAP_Normal;
  inst->pps_rate = params->pps_rate;
  inst->lock_ref = params->lock_ref_id;
  inst->offset = params->offset;
  inst->delay = params->delay;
  if (params->precision > 0.0)
    inst->precision = params->precision;
  inst->timeout_id = -1;
  inst->source = NULL;

  if (inst->driver_parameter) {
    int i;

    inst->driver_parameter_length = strlen(inst->driver_parameter);
    for (i = 0; i < inst->driver_parameter_length; i++)
      if (inst->driver_parameter[i] == ':')
        inst->driver_parameter[i] = '\0';
  }

  if (pps_source) {
    if (inst->pps_rate < 1)
      inst->pps_rate = 1;
  } else {
    inst->pps_rate = 0;
  }

  if (params->ref_id)
    inst->ref_id = params->ref_id;
  else {
    unsigned char ref[5] = { 0, 0, 0, 0, 0 };

    snprintf((char *)ref, 5, "%3.3s%d", params->driver_name, n_sources % 10);
    inst->ref_id = ref[0] << 24 | ref[1] << 16 | ref[2] << 8 | ref[3];
  }

  if (inst->driver->poll) {
    int max_samples;

    if (inst->driver_poll > inst->poll)
      inst->driver_poll = inst->poll;

    max_samples = 1 << (inst->poll - inst->driver_poll);
    if (max_samples < params->filter_length) {
      if (max_samples < 4) {
        LOG(LOGS_WARN, LOGF_Refclock, "Setting filter length for %s to %d",
            UTI_RefidToString(inst->ref_id), max_samples);
      }
      params->filter_length = max_samples;
    }
  }

  if (inst->driver->init)
    if (!inst->driver->init(inst)) {
      LOG_FATAL(LOGF_Refclock, "refclock %s initialisation failed", params->driver_name);
      return 0;
    }

  filter_init(&inst->filter, params->filter_length);

  inst->source = SRC_CreateNewInstance(inst->ref_id, SRC_REFCLOCK, params->sel_option, NULL);

#if 0
  LOG(LOGS_INFO, LOGF_Refclock, "refclock added poll=%d dpoll=%d filter=%d",
		  inst->poll, inst->driver_poll, params->filter_length);
#endif
  n_sources++;

  Free(params->driver_name);

  return 1;
}

void
RCL_StartRefclocks(void)
{
  int i, j;

  for (i = 0; i < n_sources; i++) {
    RCL_Instance inst = &refclocks[i];

    SRC_SetSelectable(inst->source);
    inst->timeout_id = SCH_AddTimeoutByDelay(0.0, poll_timeout, (void *)inst);

    if (inst->lock_ref) {
      /* Replace lock refid with index to refclocks */
      for (j = 0; j < n_sources && refclocks[j].ref_id != inst->lock_ref; j++)
        ;
      inst->lock_ref = (j < n_sources) ? j : -1;
    } else
      inst->lock_ref = -1;
  }
}

void
RCL_ReportSource(RPT_SourceReport *report, struct timeval *now)
{
  int i;
  uint32_t ref_id;

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

char *
RCL_GetDriverOption(RCL_Instance instance, char *name)
{
  char *s, *e;
  int n;

  s = instance->driver_parameter;
  e = s + instance->driver_parameter_length;
  n = strlen(name);

  while (1) {
    s += strlen(s) + 1;
    if (s >= e)
      break;
    if (!strncmp(name, s, n)) {
      if (s[n] == '=')
        return s + n + 1;
      if (s[n] == '\0')
        return s + n;
    }
  }

  return NULL;
}

int
RCL_AddSample(RCL_Instance instance, struct timeval *sample_time, double offset, int leap)
{
  double correction, dispersion;
  struct timeval cooked_time;

  LCL_GetOffsetCorrection(sample_time, &correction, &dispersion);
  UTI_AddDoubleToTimeval(sample_time, correction, &cooked_time);
  dispersion += instance->precision + filter_get_avg_sample_dispersion(&instance->filter);

  if (!valid_sample_time(instance, sample_time))
    return 0;

  filter_add_sample(&instance->filter, &cooked_time, offset - correction + instance->offset, dispersion);

  switch (leap) {
    case LEAP_Normal:
    case LEAP_InsertSecond:
    case LEAP_DeleteSecond:
      instance->leap_status = leap;
      break;
    default:
      instance->leap_status = LEAP_Unsynchronised;
      break;
  }

  log_sample(instance, &cooked_time, 0, 0, offset, offset - correction + instance->offset, dispersion);

  /* for logging purposes */
  if (!instance->driver->poll)
    instance->driver_polled++;

  return 1;
}

int
RCL_AddPulse(RCL_Instance instance, struct timeval *pulse_time, double second)
{
  double correction, dispersion, offset;
  struct timeval cooked_time;
  int rate;

  LCL_GetOffsetCorrection(pulse_time, &correction, &dispersion);
  UTI_AddDoubleToTimeval(pulse_time, correction, &cooked_time);
  dispersion += instance->precision + filter_get_avg_sample_dispersion(&instance->filter);

  if (!valid_sample_time(instance, pulse_time))
    return 0;

  rate = instance->pps_rate;
  assert(rate > 0);

  offset = -second - correction + instance->offset;

  /* Adjust the offset to [-0.5/rate, 0.5/rate) interval */
  offset -= (long)(offset * rate) / (double)rate;
  if (offset < -0.5 / rate)
    offset += 1.0 / rate;
  else if (offset >= 0.5 / rate)
    offset -= 1.0 / rate;

  if (instance->lock_ref != -1) {
    struct timeval ref_sample_time;
    double sample_diff, ref_offset, ref_dispersion, shift;

    if (!filter_get_last_sample(&refclocks[instance->lock_ref].filter,
          &ref_sample_time, &ref_offset, &ref_dispersion))
      return 0;

    UTI_DiffTimevalsToDouble(&sample_diff, &cooked_time, &ref_sample_time);
    if (fabs(sample_diff) >= 2.0 / rate)
      return 0;

    /* Align the offset to the reference sample */
    if ((ref_offset - offset) >= 0.0)
      shift = (long)((ref_offset - offset) * rate + 0.5) / (double)rate;
    else
      shift = (long)((ref_offset - offset) * rate - 0.5) / (double)rate;

    offset += shift;

    if (fabs(ref_offset - offset) + ref_dispersion + dispersion >= 0.2 / rate)
      return 0;

#if 0
    LOG(LOGS_INFO, LOGF_Refclock, "refclock pulse second=%.9f offset=%.9f offdiff=%.9f samplediff=%.9f",
        second, offset, ref_offset - offset, sample_diff);
#endif
  } else {
    struct timeval ref_time;
    int is_synchronised, stratum;
    double root_delay, root_dispersion, distance;
    NTP_Leap leap;
    uint32_t ref_id;

    /* Ignore the pulse if we are not well synchronized */

    REF_GetReferenceParams(&cooked_time, &is_synchronised, &leap, &stratum,
        &ref_id, &ref_time, &root_delay, &root_dispersion);
    distance = fabs(root_delay) / 2 + root_dispersion;

    if (!is_synchronised || distance >= 0.5 / rate) {
#if 0
      LOG(LOGS_INFO, LOGF_Refclock, "refclock pulse dropped second=%.9f sync=%d dist=%.9f",
          second, is_synchronised, distance);
#endif
      /* Drop also all stored samples */
      filter_reset(&instance->filter);
      return 0;
    }
  }

  filter_add_sample(&instance->filter, &cooked_time, offset, dispersion);
  instance->leap_status = LEAP_Normal;

  log_sample(instance, &cooked_time, 0, 1, offset + correction - instance->offset, offset, dispersion);

  /* for logging purposes */
  if (!instance->driver->poll)
    instance->driver_polled++;

  return 1;
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
  uint32_t ref_id;

  REF_GetReferenceParams(tv, &is_synchronised, &leap, &stratum,
      &ref_id, &ref_time, &root_delay, &root_dispersion);

  /* Don't change our stratum if local stratum is active
     or this is the current source */
  if (ref_id == instance->ref_id || REF_IsLocalActive())
    return stratum - 1;

  /* Or the current source is another PPS refclock */ 
  for (i = 0; i < n_sources; i++) {
    if (refclocks[i].ref_id == ref_id &&
        refclocks[i].pps_rate && refclocks[i].lock_ref == -1)
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
    inst->driver_polled = 0;

    if (sample_ok) {
      if (inst->pps_rate && inst->lock_ref == -1)
        /* Handle special case when PPS is used with local stratum */
        stratum = pps_stratum(inst, &sample_time);
      else
        stratum = 0;

      SRC_UpdateReachability(inst->source, 1);
      SRC_AccumulateSample(inst->source, &sample_time, offset,
          inst->delay, dispersion, inst->delay, dispersion, stratum, inst->leap_status);

      log_sample(inst, &sample_time, 1, 0, 0.0, offset, dispersion);
    } else {
      SRC_UpdateReachability(inst->source, 0);
    }
  }

  if (poll >= 0)
    next = 1 << poll;
  else
    next = 1.0 / (1 << -poll);

  inst->timeout_id = SCH_AddTimeoutByDelay(next, poll_timeout, arg);
}

static void
slew_samples(struct timeval *raw, struct timeval *cooked, double dfreq,
             double doffset, int is_step_change, void *anything)
{
  int i;

  for (i = 0; i < n_sources; i++)
    filter_slew_samples(&refclocks[i].filter, cooked, dfreq, doffset);
}

static void
add_dispersion(double dispersion, void *anything)
{
  int i;

  for (i = 0; i < n_sources; i++)
    filter_add_dispersion(&refclocks[i].filter, dispersion);
}

static void
log_sample(RCL_Instance instance, struct timeval *sample_time, int filtered, int pulse, double raw_offset, double cooked_offset, double dispersion)
{
  char sync_stats[4] = {'N', '+', '-', '?'};

  if (logfileid == -1)
    return;

  if (!filtered) {
    LOG_FileWrite(logfileid, "%s.%06d %-5s %3d %1c %1d %13.6e %13.6e %10.3e",
      UTI_TimeToLogForm(sample_time->tv_sec),
      (int)sample_time->tv_usec,
      UTI_RefidToString(instance->ref_id),
      instance->driver_polled,
      sync_stats[instance->leap_status],
      pulse,
      raw_offset,
      cooked_offset,
      dispersion);
  } else {
    LOG_FileWrite(logfileid, "%s.%06d %-5s   - %1c -       -       %13.6e %10.3e",
      UTI_TimeToLogForm(sample_time->tv_sec),
      (int)sample_time->tv_usec,
      UTI_RefidToString(instance->ref_id),
      sync_stats[instance->leap_status],
      cooked_offset,
      dispersion);
  }
}

static void
filter_init(struct MedianFilter *filter, int length)
{
  if (length < 1)
    length = 1;

  filter->length = length;
  filter->index = -1;
  filter->used = 0;
  filter->last = -1;
  /* set first estimate to system precision */
  filter->avg_var_n = 0;
  filter->avg_var = LCL_GetSysPrecisionAsQuantum() * LCL_GetSysPrecisionAsQuantum();
  filter->samples = MallocArray(struct FilterSample, filter->length);
  filter->selected = MallocArray(int, filter->length);
  filter->x_data = MallocArray(double, filter->length);
  filter->y_data = MallocArray(double, filter->length);
  filter->w_data = MallocArray(double, filter->length);
}

static void
filter_fini(struct MedianFilter *filter)
{
  Free(filter->samples);
  Free(filter->selected);
  Free(filter->x_data);
  Free(filter->y_data);
  Free(filter->w_data);
}

static void
filter_reset(struct MedianFilter *filter)
{
  filter->index = -1;
  filter->used = 0;
}

static double
filter_get_avg_sample_dispersion(struct MedianFilter *filter)
{
  return sqrt(filter->avg_var);
}

static void
filter_add_sample(struct MedianFilter *filter, struct timeval *sample_time, double offset, double dispersion)
{
  filter->index++;
  filter->index %= filter->length;
  filter->last = filter->index;
  if (filter->used < filter->length)
    filter->used++;

  filter->samples[filter->index].sample_time = *sample_time;
  filter->samples[filter->index].offset = offset;
  filter->samples[filter->index].dispersion = dispersion;
}

static int
filter_get_last_sample(struct MedianFilter *filter, struct timeval *sample_time, double *offset, double *dispersion)
{
  if (filter->last < 0)
    return 0;

  *sample_time = filter->samples[filter->last].sample_time;
  *offset = filter->samples[filter->last].offset;
  *dispersion = filter->samples[filter->last].dispersion;
  return 1;
}

static const struct FilterSample *tmp_sorted_array;

static int
sample_compare(const void *a, const void *b)
{
  const struct FilterSample *s1, *s2;

  s1 = &tmp_sorted_array[*(int *)a];
  s2 = &tmp_sorted_array[*(int *)b];

  if (s1->offset < s2->offset)
    return -1;
  else if (s1->offset > s2->offset)
    return 1;
  return 0;
}

int
filter_select_samples(struct MedianFilter *filter)
{
  int i, j, k, o, from, to, *selected;
  double min_dispersion;

  if (filter->used < 1)
    return 0;

  /* for lengths below 4 require full filter,
     for 4 and above require at least 4 samples */
  if ((filter->length < 4 && filter->used != filter->length) ||
      (filter->length >= 4 && filter->used < 4))
    return 0;

  selected = filter->selected;

  if (filter->used > 4) {
    /* select samples with dispersion better than 1.5 * minimum */

    for (i = 1, min_dispersion = filter->samples[0].dispersion; i < filter->used; i++) {
      if (min_dispersion > filter->samples[i].dispersion)
        min_dispersion = filter->samples[i].dispersion;
    }

    for (i = j = 0; i < filter->used; i++) {
      if (filter->samples[i].dispersion <= 1.5 * min_dispersion)
        selected[j++] = i;
    }
  } else {
    j = 0;
  }

  if (j < 4) {
    /* select all samples */

    for (j = 0; j < filter->used; j++)
      selected[j] = j;
  }

  /* and sort their indices by offset */
  tmp_sorted_array = filter->samples;
  qsort(selected, j, sizeof (int), sample_compare);

  /* select 60 percent of the samples closest to the median */ 
  if (j > 2) {
    from = j / 5;
    if (from < 1)
      from = 1;
    to = j - from;
  } else {
    from = 0;
    to = j;
  }

  /* mark unused samples and sort the rest from oldest to newest */

  o = filter->used - filter->index - 1;

  for (i = 0; i < from; i++)
    selected[i] = -1;
  for (; i < to; i++)
    selected[i] = (selected[i] + o) % filter->used;
  for (; i < filter->used; i++)
    selected[i] = -1;

  for (i = from; i < to; i++) {
    j = selected[i];
    selected[i] = -1;
    while (j != -1 && selected[j] != j) {
      k = selected[j];
      selected[j] = j;
      j = k;
    }
  }

  for (i = j = 0, k = -1; i < filter->used; i++) {
    if (selected[i] != -1)
      selected[j++] = (selected[i] + filter->used - o) % filter->used;
  }

  return j;
}

static int
filter_get_sample(struct MedianFilter *filter, struct timeval *sample_time, double *offset, double *dispersion)
{
  struct FilterSample *s, *ls;
  int i, n, dof;
  double x, y, d, e, var, prev_avg_var;

  n = filter_select_samples(filter);

  if (n < 1)
    return 0;

  ls = &filter->samples[filter->selected[n - 1]];

  /* prepare data */
  for (i = 0; i < n; i++) {
    s = &filter->samples[filter->selected[i]];

    UTI_DiffTimevalsToDouble(&filter->x_data[i], &s->sample_time, &ls->sample_time);
    filter->y_data[i] = s->offset;
    filter->w_data[i] = s->dispersion;
  }

  /* mean offset, sample time and sample dispersion */ 
  for (i = 0, x = y = e = 0.0; i < n; i++) {
    x += filter->x_data[i];
    y += filter->y_data[i];
    e += filter->w_data[i];
  }
  x /= n;
  y /= n;
  e /= n;

  e -= sqrt(filter->avg_var);

  if (n >= 4) {
    double b0, b1, s2, sb0, sb1;

    /* set y axis to the mean sample time */
    for (i = 0; i < n; i++)
      filter->x_data[i] -= x;

    /* make a linear fit and use the estimated standard deviation of intercept
       as dispersion */
    RGR_WeightedRegression(filter->x_data, filter->y_data, filter->w_data, n,
        &b0, &b1, &s2, &sb0, &sb1);
    var = s2;
    d = sb0;
    dof = n - 2;
  } else if (n >= 2) {
    for (i = 0, d = 0.0; i < n; i++)
      d += (filter->y_data[i] - y) * (filter->y_data[i] - y);
    var = d / (n - 1);
    d = sqrt(var);
    dof = n - 1;
  } else {
    var = filter->avg_var;
    d = sqrt(var);
    dof = 1;
  }

  /* avoid having zero dispersion */
  if (var < 1e-20) {
    var = 1e-20;
    d = sqrt(var);
  }

  prev_avg_var = filter->avg_var;

  /* update exponential moving average of the variance */
  if (filter->avg_var_n > 50) {
    filter->avg_var += dof / (dof + 50.0) * (var - filter->avg_var);
  } else {
    filter->avg_var = (filter->avg_var * filter->avg_var_n + var * dof) /
      (dof + filter->avg_var_n);
    if (filter->avg_var_n == 0)
      prev_avg_var = filter->avg_var;
    filter->avg_var_n += dof;
  }

  /* reduce noise in sourcestats weights by using the long-term average
     instead of the estimated variance if it's not significantly lower */
  if (var * dof / RGR_GetChi2Coef(dof) < prev_avg_var)
    d = sqrt(filter->avg_var) * d / sqrt(var);

  if (d < e)
    d = e;

  UTI_AddDoubleToTimeval(&ls->sample_time, x, sample_time);
  *offset = y;
  *dispersion = d;

  filter_reset(filter);

  return 1;
}

static void
filter_slew_samples(struct MedianFilter *filter, struct timeval *when, double dfreq, double doffset)
{
  int i;
  double delta_time, prev_offset;
  struct timeval *sample;

  for (i = 0; i < filter->used; i++) {
    sample = &filter->samples[i].sample_time;
    UTI_AdjustTimeval(sample, when, sample, &delta_time, dfreq, doffset);
    prev_offset = filter->samples[i].offset;
    filter->samples[i].offset -= delta_time;
#if 0
    LOG(LOGS_INFO, LOGF_Refclock, "i=%d old_off=%.9f new_off=%.9f",
        i, prev_offset, filter->samples[i].offset);
#else
    (void)prev_offset;
#endif
  }
}

static void
filter_add_dispersion(struct MedianFilter *filter, double dispersion)
{
  int i;

  for (i = 0; i < filter->used; i++) {
    filter->samples[i].dispersion += dispersion;
  }
}
