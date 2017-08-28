/*
  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Miroslav Lichvar  2009-2011, 2013-2014, 2016-2017
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

#include "array.h"
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
  struct timespec sample_time;
};

struct MedianFilter {
  int length;
  int index;
  int used;
  int last;
  int avg_var_n;
  double avg_var;
  double max_var;
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
  int pps_forced;
  int pps_rate;
  int pps_active;
  int max_lock_age;
  struct MedianFilter filter;
  uint32_t ref_id;
  uint32_t lock_ref;
  double offset;
  double delay;
  double precision;
  double pulse_width;
  SCH_TimeoutID timeout_id;
  SRC_Instance source;
};

/* Array of pointers to RCL_Instance_Record */
static ARR_Instance refclocks;

static LOG_FileID logfileid;

static int valid_sample_time(RCL_Instance instance, struct timespec *sample_time);
static int pps_stratum(RCL_Instance instance, struct timespec *ts);
static void poll_timeout(void *arg);
static void slew_samples(struct timespec *raw, struct timespec *cooked, double dfreq,
             double doffset, LCL_ChangeType change_type, void *anything);
static void add_dispersion(double dispersion, void *anything);
static void log_sample(RCL_Instance instance, struct timespec *sample_time, int filtered, int pulse, double raw_offset, double cooked_offset, double dispersion);

static void filter_init(struct MedianFilter *filter, int length, double max_dispersion);
static void filter_fini(struct MedianFilter *filter);
static void filter_reset(struct MedianFilter *filter);
static double filter_get_avg_sample_dispersion(struct MedianFilter *filter);
static void filter_add_sample(struct MedianFilter *filter, struct timespec *sample_time, double offset, double dispersion);
static int filter_get_last_sample(struct MedianFilter *filter, struct timespec *sample_time, double *offset, double *dispersion);
static int filter_get_samples(struct MedianFilter *filter);
static int filter_select_samples(struct MedianFilter *filter);
static int filter_get_sample(struct MedianFilter *filter, struct timespec *sample_time, double *offset, double *dispersion);
static void filter_slew_samples(struct MedianFilter *filter, struct timespec *when, double dfreq, double doffset);
static void filter_add_dispersion(struct MedianFilter *filter, double dispersion);

static RCL_Instance
get_refclock(unsigned int index)
{
  return *(RCL_Instance *)ARR_GetElement(refclocks, index);
}

void
RCL_Initialise(void)
{
  refclocks = ARR_CreateInstance(sizeof (RCL_Instance));

  CNF_AddRefclocks();

  if (ARR_GetSize(refclocks) > 0) {
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
  unsigned int i;

  for (i = 0; i < ARR_GetSize(refclocks); i++) {
    RCL_Instance inst = get_refclock(i);

    if (inst->driver->fini)
      inst->driver->fini(inst);

    filter_fini(&inst->filter);
    Free(inst->driver_parameter);
    SRC_DestroyInstance(inst->source);
    Free(inst);
  }

  if (ARR_GetSize(refclocks) > 0) {
    LCL_RemoveParameterChangeHandler(slew_samples, NULL);
    LCL_RemoveDispersionNotifyHandler(add_dispersion, NULL);
  }

  ARR_DestroyInstance(refclocks);
}

int
RCL_AddRefclock(RefclockParameters *params)
{
  RCL_Instance inst;

  inst = MallocNew(struct RCL_Instance_Record);
  *(RCL_Instance *)ARR_GetNewElement(refclocks) = inst;

  if (strcmp(params->driver_name, "SHM") == 0) {
    inst->driver = &RCL_SHM_driver;
  } else if (strcmp(params->driver_name, "SOCK") == 0) {
    inst->driver = &RCL_SOCK_driver;
  } else if (strcmp(params->driver_name, "PPS") == 0) {
    inst->driver = &RCL_PPS_driver;
  } else if (strcmp(params->driver_name, "PHC") == 0) {
    inst->driver = &RCL_PHC_driver;
  } else {
    LOG_FATAL("unknown refclock driver %s", params->driver_name);
    return 0;
  }

  if (!inst->driver->init && !inst->driver->poll) {
    LOG_FATAL("refclock driver %s is not compiled in", params->driver_name);
    return 0;
  }

  inst->data = NULL;
  inst->driver_parameter = params->driver_parameter;
  inst->driver_parameter_length = 0;
  inst->driver_poll = params->driver_poll;
  inst->poll = params->poll;
  inst->driver_polled = 0;
  inst->leap_status = LEAP_Normal;
  inst->pps_forced = params->pps_forced;
  inst->pps_rate = params->pps_rate;
  inst->pps_active = 0;
  inst->max_lock_age = params->max_lock_age;
  inst->lock_ref = params->lock_ref_id;
  inst->offset = params->offset;
  inst->delay = params->delay;
  inst->precision = LCL_GetSysPrecisionAsQuantum();
  inst->precision = MAX(inst->precision, params->precision);
  inst->pulse_width = params->pulse_width;
  inst->timeout_id = -1;
  inst->source = NULL;

  if (inst->driver_parameter) {
    int i;

    inst->driver_parameter_length = strlen(inst->driver_parameter);
    for (i = 0; i < inst->driver_parameter_length; i++)
      if (inst->driver_parameter[i] == ':')
        inst->driver_parameter[i] = '\0';
  }

  if (inst->pps_rate < 1)
    inst->pps_rate = 1;

  if (params->ref_id)
    inst->ref_id = params->ref_id;
  else {
    unsigned char ref[5] = { 0, 0, 0, 0, 0 };
    unsigned int index = ARR_GetSize(refclocks) - 1;

    snprintf((char *)ref, sizeof (ref), "%3.3s", params->driver_name);
    ref[3] = index % 10 + '0';
    if (index >= 10)
      ref[2] = (index / 10) % 10 + '0';

    inst->ref_id = (uint32_t)ref[0] << 24 | ref[1] << 16 | ref[2] << 8 | ref[3];
  }

  if (inst->driver->poll) {
    int max_samples;

    if (inst->driver_poll > inst->poll)
      inst->driver_poll = inst->poll;

    max_samples = 1 << (inst->poll - inst->driver_poll);
    if (max_samples < params->filter_length) {
      if (max_samples < 4) {
        LOG(LOGS_WARN, "Setting filter length for %s to %d",
            UTI_RefidToString(inst->ref_id), max_samples);
      }
      params->filter_length = max_samples;
    }
  }

  if (inst->driver->init)
    if (!inst->driver->init(inst)) {
      LOG_FATAL("refclock %s initialisation failed", params->driver_name);
      return 0;
    }

  filter_init(&inst->filter, params->filter_length, params->max_dispersion);

  inst->source = SRC_CreateNewInstance(inst->ref_id, SRC_REFCLOCK, params->sel_options, NULL,
                                       params->min_samples, params->max_samples, 0.0, 0.0);

  DEBUG_LOG("refclock %s refid=%s poll=%d dpoll=%d filter=%d",
      params->driver_name, UTI_RefidToString(inst->ref_id),
      inst->poll, inst->driver_poll, params->filter_length);

  Free(params->driver_name);

  return 1;
}

void
RCL_StartRefclocks(void)
{
  unsigned int i, j, n;

  n = ARR_GetSize(refclocks);

  for (i = 0; i < n; i++) {
    RCL_Instance inst = get_refclock(i);

    SRC_SetActive(inst->source);
    inst->timeout_id = SCH_AddTimeoutByDelay(0.0, poll_timeout, (void *)inst);

    if (inst->lock_ref) {
      /* Replace lock refid with index to refclocks */
      for (j = 0; j < n && get_refclock(j)->ref_id != inst->lock_ref; j++)
        ;
      inst->lock_ref = j < n ? j : -1;
    } else
      inst->lock_ref = -1;
  }
}

void
RCL_ReportSource(RPT_SourceReport *report, struct timespec *now)
{
  unsigned int i;
  uint32_t ref_id;

  assert(report->ip_addr.family == IPADDR_INET4);
  ref_id = report->ip_addr.addr.in4;

  for (i = 0; i < ARR_GetSize(refclocks); i++) {
    RCL_Instance inst = get_refclock(i);
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
RCL_AddSample(RCL_Instance instance, struct timespec *sample_time, double offset, int leap)
{
  double correction, dispersion;
  struct timespec cooked_time;

  if (instance->pps_forced)
    return RCL_AddPulse(instance, sample_time, -offset);

  LCL_GetOffsetCorrection(sample_time, &correction, &dispersion);
  UTI_AddDoubleToTimespec(sample_time, correction, &cooked_time);
  dispersion += instance->precision;

  /* Make sure the timestamp and offset provided by the driver are sane */
  if (!UTI_IsTimeOffsetSane(sample_time, offset) ||
      !valid_sample_time(instance, &cooked_time))
    return 0;

  switch (leap) {
    case LEAP_Normal:
    case LEAP_InsertSecond:
    case LEAP_DeleteSecond:
      instance->leap_status = leap;
      break;
    default:
      DEBUG_LOG("refclock sample ignored bad leap %d", leap);
      return 0;
  }

  filter_add_sample(&instance->filter, &cooked_time, offset - correction + instance->offset, dispersion);
  instance->pps_active = 0;

  log_sample(instance, &cooked_time, 0, 0, offset, offset - correction + instance->offset, dispersion);

  /* for logging purposes */
  if (!instance->driver->poll)
    instance->driver_polled++;

  return 1;
}

int
RCL_AddPulse(RCL_Instance instance, struct timespec *pulse_time, double second)
{
  double correction, dispersion;
  struct timespec cooked_time;

  LCL_GetOffsetCorrection(pulse_time, &correction, &dispersion);
  UTI_AddDoubleToTimespec(pulse_time, correction, &cooked_time);
  second += correction;

  if (!UTI_IsTimeOffsetSane(pulse_time, 0.0))
    return 0;

  return RCL_AddCookedPulse(instance, &cooked_time, second, dispersion, correction);
}

static int
check_pulse_edge(RCL_Instance instance, double offset, double distance)
{
  double max_error;

  if (instance->pulse_width <= 0.0)
    return 1;

  max_error = 1.0 / instance->pps_rate - instance->pulse_width;
  max_error = MIN(instance->pulse_width, max_error);
  max_error *= 0.5;

  if (fabs(offset) > max_error || distance > max_error) {
      DEBUG_LOG("refclock pulse ignored offset=%.9f distance=%.9f max_error=%.9f",
                offset, distance, max_error);
      return 0;
  }

  return 1;
}

int
RCL_AddCookedPulse(RCL_Instance instance, struct timespec *cooked_time,
                   double second, double dispersion, double raw_correction)
{
  double offset;
  int rate;
  NTP_Leap leap;

  if (!UTI_IsTimeOffsetSane(cooked_time, second) ||
      !valid_sample_time(instance, cooked_time))
    return 0;

  leap = LEAP_Normal;
  dispersion += instance->precision;
  rate = instance->pps_rate;

  offset = -second + instance->offset;

  /* Adjust the offset to [-0.5/rate, 0.5/rate) interval */
  offset -= (long)(offset * rate) / (double)rate;
  if (offset < -0.5 / rate)
    offset += 1.0 / rate;
  else if (offset >= 0.5 / rate)
    offset -= 1.0 / rate;

  if (instance->lock_ref != -1) {
    RCL_Instance lock_refclock;
    struct timespec ref_sample_time;
    double sample_diff, ref_offset, ref_dispersion, shift;

    lock_refclock = get_refclock(instance->lock_ref);

    if (!filter_get_last_sample(&lock_refclock->filter,
          &ref_sample_time, &ref_offset, &ref_dispersion)) {
      DEBUG_LOG("refclock pulse ignored no ref sample");
      return 0;
    }

    ref_dispersion += filter_get_avg_sample_dispersion(&lock_refclock->filter);

    sample_diff = UTI_DiffTimespecsToDouble(cooked_time, &ref_sample_time);
    if (fabs(sample_diff) >= (double)instance->max_lock_age / rate) {
      DEBUG_LOG("refclock pulse ignored samplediff=%.9f",
          sample_diff);
      return 0;
    }

    /* Align the offset to the reference sample */
    if ((ref_offset - offset) >= 0.0)
      shift = (long)((ref_offset - offset) * rate + 0.5) / (double)rate;
    else
      shift = (long)((ref_offset - offset) * rate - 0.5) / (double)rate;

    offset += shift;

    if (fabs(ref_offset - offset) + ref_dispersion + dispersion >= 0.2 / rate) {
      DEBUG_LOG("refclock pulse ignored offdiff=%.9f refdisp=%.9f disp=%.9f",
          ref_offset - offset, ref_dispersion, dispersion);
      return 0;
    }

    if (!check_pulse_edge(instance, ref_offset - offset, 0.0))
      return 0;

    leap = lock_refclock->leap_status;

    DEBUG_LOG("refclock pulse offset=%.9f offdiff=%.9f samplediff=%.9f",
              offset, ref_offset - offset, sample_diff);
  } else {
    struct timespec ref_time;
    int is_synchronised, stratum;
    double root_delay, root_dispersion, distance;
    uint32_t ref_id;

    /* Ignore the pulse if we are not well synchronized and the local
       reference is not active */

    REF_GetReferenceParams(cooked_time, &is_synchronised, &leap, &stratum,
        &ref_id, &ref_time, &root_delay, &root_dispersion);
    distance = fabs(root_delay) / 2 + root_dispersion;

    if (leap == LEAP_Unsynchronised || distance >= 0.5 / rate) {
      DEBUG_LOG("refclock pulse ignored offset=%.9f sync=%d dist=%.9f",
                offset, leap != LEAP_Unsynchronised, distance);
      /* Drop also all stored samples */
      filter_reset(&instance->filter);
      return 0;
    }

    if (!check_pulse_edge(instance, offset, distance))
      return 0;
  }

  filter_add_sample(&instance->filter, cooked_time, offset, dispersion);
  instance->leap_status = leap;
  instance->pps_active = 1;

  log_sample(instance, cooked_time, 0, 1, offset + raw_correction - instance->offset,
             offset, dispersion);

  /* for logging purposes */
  if (!instance->driver->poll)
    instance->driver_polled++;

  return 1;
}

double
RCL_GetPrecision(RCL_Instance instance)
{
  return instance->precision;
}

int
RCL_GetDriverPoll(RCL_Instance instance)
{
  return instance->driver_poll;
}

static int
valid_sample_time(RCL_Instance instance, struct timespec *sample_time)
{
  struct timespec now, last_sample_time;
  double diff, last_offset, last_dispersion;

  LCL_ReadCookedTime(&now, NULL);
  diff = UTI_DiffTimespecsToDouble(&now, sample_time);

  if (diff < 0.0 || diff > UTI_Log2ToDouble(instance->poll + 1) ||
      (filter_get_samples(&instance->filter) > 0 &&
       filter_get_last_sample(&instance->filter, &last_sample_time,
                              &last_offset, &last_dispersion) &&
       UTI_CompareTimespecs(&last_sample_time, sample_time) >= 0)) {
    DEBUG_LOG("%s refclock sample time %s not valid age=%.6f",
              UTI_RefidToString(instance->ref_id),
              UTI_TimespecToString(sample_time), diff);
    return 0;
  }

  return 1;
}

static int
pps_stratum(RCL_Instance instance, struct timespec *ts)
{
  struct timespec ref_time;
  int is_synchronised, stratum;
  unsigned int i;
  double root_delay, root_dispersion;
  NTP_Leap leap;
  uint32_t ref_id;
  RCL_Instance refclock;

  REF_GetReferenceParams(ts, &is_synchronised, &leap, &stratum,
      &ref_id, &ref_time, &root_delay, &root_dispersion);

  /* Don't change our stratum if the local reference is active
     or this is the current source */
  if (ref_id == instance->ref_id ||
      (!is_synchronised && leap != LEAP_Unsynchronised))
    return stratum - 1;

  /* Or the current source is another PPS refclock */ 
  for (i = 0; i < ARR_GetSize(refclocks); i++) {
    refclock = get_refclock(i);
    if (refclock->ref_id == ref_id &&
        refclock->pps_active && refclock->lock_ref == -1)
      return stratum - 1;
  }

  return 0;
}

static void
poll_timeout(void *arg)
{
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
    struct timespec sample_time;
    int sample_ok, stratum;

    sample_ok = filter_get_sample(&inst->filter, &sample_time, &offset, &dispersion);
    inst->driver_polled = 0;

    if (sample_ok) {
      if (inst->pps_active && inst->lock_ref == -1)
        /* Handle special case when PPS is used with local stratum */
        stratum = pps_stratum(inst, &sample_time);
      else
        stratum = 0;

      SRC_UpdateReachability(inst->source, 1);
      SRC_AccumulateSample(inst->source, &sample_time, offset,
          inst->delay, dispersion, inst->delay, dispersion, stratum, inst->leap_status);
      SRC_SelectSource(inst->source);

      log_sample(inst, &sample_time, 1, 0, 0.0, offset, dispersion);
    } else {
      SRC_UpdateReachability(inst->source, 0);
    }
  }

  inst->timeout_id = SCH_AddTimeoutByDelay(UTI_Log2ToDouble(poll), poll_timeout, arg);
}

static void
slew_samples(struct timespec *raw, struct timespec *cooked, double dfreq,
             double doffset, LCL_ChangeType change_type, void *anything)
{
  unsigned int i;

  for (i = 0; i < ARR_GetSize(refclocks); i++) {
    if (change_type == LCL_ChangeUnknownStep)
      filter_reset(&get_refclock(i)->filter);
    else
      filter_slew_samples(&get_refclock(i)->filter, cooked, dfreq, doffset);
  }
}

static void
add_dispersion(double dispersion, void *anything)
{
  unsigned int i;

  for (i = 0; i < ARR_GetSize(refclocks); i++)
    filter_add_dispersion(&get_refclock(i)->filter, dispersion);
}

static void
log_sample(RCL_Instance instance, struct timespec *sample_time, int filtered, int pulse, double raw_offset, double cooked_offset, double dispersion)
{
  char sync_stats[4] = {'N', '+', '-', '?'};

  if (logfileid == -1)
    return;

  if (!filtered) {
    LOG_FileWrite(logfileid, "%s.%06d %-5s %3d %1c %1d %13.6e %13.6e %10.3e",
      UTI_TimeToLogForm(sample_time->tv_sec),
      (int)sample_time->tv_nsec / 1000,
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
      (int)sample_time->tv_nsec / 1000,
      UTI_RefidToString(instance->ref_id),
      sync_stats[instance->leap_status],
      cooked_offset,
      dispersion);
  }
}

static void
filter_init(struct MedianFilter *filter, int length, double max_dispersion)
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
  filter->max_var = max_dispersion * max_dispersion;
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
filter_add_sample(struct MedianFilter *filter, struct timespec *sample_time, double offset, double dispersion)
{
  filter->index++;
  filter->index %= filter->length;
  filter->last = filter->index;
  if (filter->used < filter->length)
    filter->used++;

  filter->samples[filter->index].sample_time = *sample_time;
  filter->samples[filter->index].offset = offset;
  filter->samples[filter->index].dispersion = dispersion;

  DEBUG_LOG("filter sample %d t=%s offset=%.9f dispersion=%.9f",
      filter->index, UTI_TimespecToString(sample_time), offset, dispersion);
}

static int
filter_get_last_sample(struct MedianFilter *filter, struct timespec *sample_time, double *offset, double *dispersion)
{
  if (filter->last < 0)
    return 0;

  *sample_time = filter->samples[filter->last].sample_time;
  *offset = filter->samples[filter->last].offset;
  *dispersion = filter->samples[filter->last].dispersion;
  return 1;
}

static int
filter_get_samples(struct MedianFilter *filter)
{
  return filter->used;
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
filter_get_sample(struct MedianFilter *filter, struct timespec *sample_time, double *offset, double *dispersion)
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

    filter->x_data[i] = UTI_DiffTimespecsToDouble(&s->sample_time, &ls->sample_time);
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

  /* drop the sample if variance is larger than allowed maximum */
  if (filter->max_var > 0.0 && var > filter->max_var) {
    DEBUG_LOG("filter dispersion too large disp=%.9f max=%.9f",
        sqrt(var), sqrt(filter->max_var));
    return 0;
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

  UTI_AddDoubleToTimespec(&ls->sample_time, x, sample_time);
  *offset = y;
  *dispersion = d;

  filter_reset(filter);

  return 1;
}

static void
filter_slew_samples(struct MedianFilter *filter, struct timespec *when, double dfreq, double doffset)
{
  int i, first, last;
  double delta_time;
  struct timespec *sample;

  if (filter->last < 0)
    return;

  /* always slew the last sample as it may be needed by PPS refclocks */
  if (filter->used > 0) {
    first = 0;
    last = filter->used - 1;
  } else {
    first = last = filter->last;
  }

  for (i = first; i <= last; i++) {
    sample = &filter->samples[i].sample_time;
    UTI_AdjustTimespec(sample, when, sample, &delta_time, dfreq, doffset);
    filter->samples[i].offset -= delta_time;
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
