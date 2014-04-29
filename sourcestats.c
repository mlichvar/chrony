/*
  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Richard P. Curnow  1997-2003
 * Copyright (C) Miroslav Lichvar  2011-2012
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

  This file contains the routines that do the statistical
  analysis on the samples obtained from the sources,
  to determined frequencies and error bounds. */

#include "config.h"

#include "sysincl.h"

#include "sourcestats.h"
#include "memory.h"
#include "regress.h"
#include "util.h"
#include "conf.h"
#include "logging.h"
#include "local.h"

/* ================================================== */
/* Define the maxumum number of samples that we want
   to store per source */
#define MAX_SAMPLES 64

/* User defined maximum and minimum number of samples */
int max_samples;
int min_samples;

/* This is the assumed worst case bound on an unknown frequency,
   2000ppm, which would be pretty bad */
#define WORST_CASE_FREQ_BOUND (2000.0/1.0e6)

/* The minimum allowed skew */
#define MIN_SKEW 1.0e-12

/* ================================================== */

static LOG_FileID logfileid;

/* ================================================== */
/* This data structure is used to hold the history of data from the
   source */

struct SST_Stats_Record {

  /* Reference ID and IP address of source, used for logging to statistics log */
  uint32_t refid;
  IPAddr *ip_addr;

  /* Number of samples currently stored.  The samples are stored in circular
     buffer. */
  int n_samples;

  /* Number of extra samples stored in sample_times and offsets arrays that are
     used to extend runs test */
  int runs_samples;

  /* The index of the newest sample */
  int last_sample;

  /* Flag indicating whether last regression was successful */
  int regression_ok;

  /* The best individual sample that we are holding, in terms of the minimum
     root distance at the present time */
  int best_single_sample;

  /* The index of the sample with minimum delay in peer_delays */
  int min_delay_sample;

  /* This is the estimated offset (+ve => local fast) at a particular time */
  double estimated_offset;
  double estimated_offset_sd;
  struct timeval offset_time;

  /* Number of runs of the same sign amongst the residuals */
  int nruns;

  /* This value contains the estimated frequency.  This is the number
     of seconds that the local clock gains relative to the reference
     source per unit local time.  (Positive => local clock fast,
     negative => local clock slow) */
  double estimated_frequency;

  /* This is the assumed worst case bounds on the estimated frequency.
     We assume that the true frequency lies within +/- half this much
     about estimated_frequency */
  double skew;

  /* This is the direction the skew went in at the last sample */
  SST_Skew_Direction skew_dirn;

  /* This is the estimated residual variance of the data points */
  double variance;

  /* This array contains the sample epochs, in terms of the local
     clock. */
  struct timeval sample_times[MAX_SAMPLES * REGRESS_RUNS_RATIO];

  /* This is an array of offsets, in seconds, corresponding to the
     sample times.  In this module, we use the convention that
     positive means the local clock is FAST of the source and negative
     means it is SLOW.  This is contrary to the convention in the NTP
     stuff; that part of the code is written to correspond with
     RFC1305 conventions. */
  double offsets[MAX_SAMPLES * REGRESS_RUNS_RATIO];

  /* This is an array of the offsets as originally measured.  Local
     clock fast of real time is indicated by positive values.  This
     array is not slewed to adjust the readings when we apply
     adjustments to the local clock, as is done for the array
     'offset'. */
  double orig_offsets[MAX_SAMPLES];

  /* This is an array of peer delays, in seconds, being the roundtrip
     measurement delay to the peer */
  double peer_delays[MAX_SAMPLES];

  /* This is an array of peer dispersions, being the skew and local
     precision dispersion terms from sampling the peer */
  double peer_dispersions[MAX_SAMPLES];

  /* This array contains the root delays of each sample, in seconds */
  double root_delays[MAX_SAMPLES];

  /* This array contains the root dispersions of each sample at the
     time of the measurements */
  double root_dispersions[MAX_SAMPLES];

  /* This array contains the strata that were associated with the sources
     at the times the samples were generated */
  int strata[MAX_SAMPLES];

};

/* ================================================== */

static void find_min_delay_sample(SST_Stats inst);
static int get_buf_index(SST_Stats inst, int i);

/* ================================================== */

void
SST_Initialise(void)
{
  logfileid = CNF_GetLogStatistics() ? LOG_FileOpen("statistics",
      "   Date (UTC) Time     IP Address    Std dev'n Est offset  Offset sd  Diff freq   Est skew  Stress  Ns  Bs  Nr")
    : -1;
  max_samples = CNF_GetMaxSamples();
  min_samples = CNF_GetMinSamples();
}

/* ================================================== */

void
SST_Finalise(void)
{
}

/* ================================================== */
/* This function creates a new instance of the statistics handler */

SST_Stats
SST_CreateInstance(uint32_t refid, IPAddr *addr)
{
  SST_Stats inst;
  inst = MallocNew(struct SST_Stats_Record);
  inst->refid = refid;
  inst->ip_addr = addr;
  inst->n_samples = 0;
  inst->runs_samples = 0;
  inst->last_sample = 0;
  inst->regression_ok = 0;
  inst->best_single_sample = 0;
  inst->min_delay_sample = 0;
  inst->estimated_frequency = 0;
  inst->skew = 2000.0e-6;
  inst->skew_dirn = SST_Skew_Nochange;
  inst->estimated_offset = 0.0;
  inst->estimated_offset_sd = 86400.0; /* Assume it's at least within a day! */
  inst->offset_time.tv_sec = 0;
  inst->offset_time.tv_usec = 0;
  inst->variance = 16.0;
  inst->nruns = 0;
  return inst;
}

/* ================================================== */
/* This function deletes an instance of the statistics handler. */

void
SST_DeleteInstance(SST_Stats inst)
{
  Free(inst);
}

/* ================================================== */
/* This function is called to prune the register down when it is full.
   For now, just discard the oldest sample.  */

static void
prune_register(SST_Stats inst, int new_oldest)
{
  if (!new_oldest)
    return;

  assert(inst->n_samples >= new_oldest);
  inst->n_samples -= new_oldest;
  inst->runs_samples += new_oldest;
  if (inst->runs_samples > inst->n_samples * (REGRESS_RUNS_RATIO - 1))
    inst->runs_samples = inst->n_samples * (REGRESS_RUNS_RATIO - 1);
  
  assert(inst->n_samples + inst->runs_samples <= MAX_SAMPLES * REGRESS_RUNS_RATIO);

  find_min_delay_sample(inst);
}

/* ================================================== */

void
SST_AccumulateSample(SST_Stats inst, struct timeval *sample_time,
                     double offset,
                     double peer_delay, double peer_dispersion,
                     double root_delay, double root_dispersion,
                     int stratum)
{
  int n, m;

  /* Make room for the new sample */
  if (inst->n_samples > 0 &&
      (inst->n_samples == MAX_SAMPLES || inst->n_samples == max_samples)) {
    prune_register(inst, 1);
  }

  /* Make sure it's newer than the last sample */
  if (inst->n_samples &&
      UTI_CompareTimevals(&inst->sample_times[inst->last_sample], sample_time) >= 0) {
    LOG(LOGS_WARN, LOGF_SourceStats, "Out of order sample detected, discarding history for %s",
        inst->ip_addr ? UTI_IPToString(inst->ip_addr) : UTI_RefidToString(inst->refid));
    prune_register(inst, inst->n_samples);
  }

  n = inst->last_sample = (inst->last_sample + 1) %
    (MAX_SAMPLES * REGRESS_RUNS_RATIO);
  m = n % MAX_SAMPLES;

  inst->sample_times[n] = *sample_time;
  inst->offsets[n] = offset;
  inst->orig_offsets[m] = offset;
  inst->peer_delays[m] = peer_delay;
  inst->peer_dispersions[m] = peer_dispersion;
  inst->root_delays[m] = root_delay;
  inst->root_dispersions[m] = root_dispersion;
  inst->strata[m] = stratum;
 
  if (!inst->n_samples || inst->peer_delays[m] < inst->peer_delays[inst->min_delay_sample])
    inst->min_delay_sample = m;

  ++inst->n_samples;
}

/* ================================================== */
/* Return index of the i-th sample in the sample_times and offset buffers,
   i can be negative down to -runs_samples */

static int
get_runsbuf_index(SST_Stats inst, int i)
{
  return (unsigned int)(inst->last_sample + 2 * MAX_SAMPLES * REGRESS_RUNS_RATIO -
      inst->n_samples + i + 1) % (MAX_SAMPLES * REGRESS_RUNS_RATIO);
}

/* ================================================== */
/* Return index of the i-th sample in the other buffers */

static int
get_buf_index(SST_Stats inst, int i)
{
  return (unsigned int)(inst->last_sample + MAX_SAMPLES * REGRESS_RUNS_RATIO -
      inst->n_samples + i + 1) % MAX_SAMPLES;
}

/* ================================================== */
/* This function is used by both the regression routines to find the
   time interval between each historical sample and the most recent
   one */

static void
convert_to_intervals(SST_Stats inst, double *times_back)
{
  struct timeval *newest_tv;
  int i;

  newest_tv = &(inst->sample_times[inst->last_sample]);
  for (i = -inst->runs_samples; i < inst->n_samples; i++) {
    /* The entries in times_back[] should end up negative */
    UTI_DiffTimevalsToDouble(&times_back[i],
        &inst->sample_times[get_runsbuf_index(inst, i)], newest_tv);
  }
}

/* ================================================== */

static void
find_best_sample_index(SST_Stats inst, double *times_back)
{
  /* With the value of skew that has been computed, see which of the
     samples offers the tightest bound on root distance */

  double root_distance, best_root_distance;
  double elapsed;
  int i, j, best_index;

  if (!inst->n_samples)
    return;

  best_index = -1;
  best_root_distance = DBL_MAX;

  for (i = 0; i < inst->n_samples; i++) {
    j = get_buf_index(inst, i);

    elapsed = -times_back[i];
    assert(elapsed >= 0.0);

    root_distance = inst->root_dispersions[j] + elapsed * inst->skew + 0.5 * inst->root_delays[j];
    if (root_distance < best_root_distance) {
      best_root_distance = root_distance;
      best_index = i;
    }
  }

  assert(best_index >= 0);
  inst->best_single_sample = best_index;

#if 0
  LOG(LOGS_INFO, LOGF_SourceStats, "n=%d best_index=%d", n, best_index);
#endif
}

/* ================================================== */

static void
find_min_delay_sample(SST_Stats inst)
{
  int i, index;

  inst->min_delay_sample = get_buf_index(inst, 0);

  for (i = 1; i < inst->n_samples; i++) {
    index = get_buf_index(inst, i);
    if (inst->peer_delays[index] < inst->peer_delays[inst->min_delay_sample])
      inst->min_delay_sample = index;
  }
}

/* ================================================== */

/* This defines the assumed ratio between the standard deviation of
   the samples and the peer distance as measured from the round trip
   time.  E.g. a value of 4 means that we think the standard deviation
   is four times the fluctuation  of the peer distance */

#define SD_TO_DIST_RATIO 1.0

/* ================================================== */
/* This function runs the linear regression operation on the data.  It
   finds the set of most recent samples that give the tightest
   confidence interval for the frequency, and truncates the register
   down to that number of samples */

void
SST_DoNewRegression(SST_Stats inst)
{
  double times_back[MAX_SAMPLES * REGRESS_RUNS_RATIO];
  double offsets[MAX_SAMPLES * REGRESS_RUNS_RATIO];
  double peer_distances[MAX_SAMPLES];
  double weights[MAX_SAMPLES];

  int degrees_of_freedom;
  int best_start, times_back_start;
  double est_intercept, est_slope, est_var, est_intercept_sd, est_slope_sd;
  int i, j, nruns;
  double min_distance, mean_distance;
  double sd_weight, sd;
  double old_skew, old_freq, stress;

  convert_to_intervals(inst, times_back + inst->runs_samples);

  if (inst->n_samples > 0) {
    for (i = -inst->runs_samples; i < inst->n_samples; i++) {
      offsets[i + inst->runs_samples] = inst->offsets[get_runsbuf_index(inst, i)];
    }
  
    for (i = 0, mean_distance = 0.0, min_distance = DBL_MAX; i < inst->n_samples; i++) {
      j = get_buf_index(inst, i);
      peer_distances[i] = 0.5 * inst->peer_delays[j] + inst->peer_dispersions[j];
      mean_distance += peer_distances[i];
      if (peer_distances[i] < min_distance) {
        min_distance = peer_distances[i];
      }
    }
    mean_distance /= inst->n_samples;

    /* And now, work out the weight vector */

    sd = mean_distance - min_distance;
    if (sd > min_distance || sd <= 0.0)
      sd = min_distance;

    for (i=0; i<inst->n_samples; i++) {
      sd_weight = 1.0 + SD_TO_DIST_RATIO * (peer_distances[i] - min_distance) / sd;
      weights[i] = sd_weight * sd_weight;
    }
  }

  inst->regression_ok = RGR_FindBestRegression(times_back + inst->runs_samples,
                                         offsets + inst->runs_samples, weights,
                                         inst->n_samples, inst->runs_samples,
                                         min_samples,
                                         &est_intercept, &est_slope, &est_var,
                                         &est_intercept_sd, &est_slope_sd,
                                         &best_start, &nruns, &degrees_of_freedom);

  if (inst->regression_ok) {

    old_skew = inst->skew;
    old_freq = inst->estimated_frequency;
  
    inst->estimated_frequency = est_slope;
    inst->skew = est_slope_sd * RGR_GetTCoef(degrees_of_freedom);
    inst->estimated_offset = est_intercept;
    inst->offset_time = inst->sample_times[inst->last_sample];
    inst->estimated_offset_sd = est_intercept_sd;
    inst->variance = est_var;
    inst->nruns = nruns;

    if (inst->skew < MIN_SKEW)
      inst->skew = MIN_SKEW;

    stress = fabs(old_freq - inst->estimated_frequency) / old_skew;

    if (best_start > 0) {
      /* If we are throwing old data away, retain the current
         assumptions about the skew */
      inst->skew_dirn = SST_Skew_Nochange;
    } else {
      if (inst->skew < old_skew) {
        inst->skew_dirn = SST_Skew_Decrease;
      } else {
        inst->skew_dirn = SST_Skew_Increase;
      }
    }

    if (logfileid != -1) {
      LOG_FileWrite(logfileid, "%s %-15s %10.3e %10.3e %10.3e %10.3e %10.3e %7.1e %3d %3d %3d",
              UTI_TimeToLogForm(inst->offset_time.tv_sec),
              inst->ip_addr ? UTI_IPToString(inst->ip_addr) : UTI_RefidToString(inst->refid),
              sqrt(inst->variance),
              inst->estimated_offset,
              inst->estimated_offset_sd,
              inst->estimated_frequency,
              inst->skew,
              stress,
              inst->n_samples,
              best_start, nruns);
    }

    times_back_start = inst->runs_samples + best_start;
    prune_register(inst, best_start);
  } else {
#if 0
    LOG(LOGS_INFO, LOGF_SourceStats, "too few points (%d) for regression", inst->n_samples);
#endif
    inst->estimated_frequency = 0.0;
    inst->skew = WORST_CASE_FREQ_BOUND;
    times_back_start = 0;
  }

  find_best_sample_index(inst, times_back + times_back_start);

}

/* ================================================== */
/* Return the assumed worst case range of values that this source's
   frequency lies within.  Frequency is defined as the amount of time
   the local clock gains relative to the source per unit local clock
   time. */
void
SST_GetFrequencyRange(SST_Stats inst,
                      double *lo, double *hi)
{
  double freq, skew;
  freq = inst->estimated_frequency;
  skew = inst->skew;
  *lo = freq - skew;
  *hi = freq + skew;

  /* This function is currently used only to determine the values of delta
     and epsilon in the ntp_core module. Limit the skew to a reasonable maximum
     to avoid failing the dispersion test too easily. */
  if (skew > WORST_CASE_FREQ_BOUND) {
    *lo = -WORST_CASE_FREQ_BOUND;
    *hi = WORST_CASE_FREQ_BOUND;
  }
}

/* ================================================== */

void
SST_GetSelectionData(SST_Stats inst, struct timeval *now,
                     int *stratum,
                     double *offset_lo_limit,
                     double *offset_hi_limit,
                     double *root_distance,
                     double *variance, int *select_ok)
{
  double offset, sample_elapsed;
  int i, j;
  
  i = get_runsbuf_index(inst, inst->best_single_sample);
  j = get_buf_index(inst, inst->best_single_sample);

  *stratum = inst->strata[get_buf_index(inst, inst->n_samples - 1)];
  *variance = inst->variance;

  UTI_DiffTimevalsToDouble(&sample_elapsed, now, &inst->sample_times[i]);
  offset = inst->offsets[i] + sample_elapsed * inst->estimated_frequency;
  *root_distance = 0.5 * inst->root_delays[j] +
    inst->root_dispersions[j] + sample_elapsed * inst->skew;

  *offset_lo_limit = offset - *root_distance;
  *offset_hi_limit = offset + *root_distance;

#if 0
  double average_offset, elapsed;
  int average_ok;
  /* average_ok ignored for now */
  UTI_DiffTimevalsToDouble(&elapsed, now, &(inst->offset_time));
  average_offset = inst->estimated_offset + inst->estimated_frequency * elapsed;
  if (fabs(average_offset - offset) <=
      inst->peer_dispersions[j] + 0.5 * inst->peer_delays[j]) {
    average_ok = 1;
  } else {
    average_ok = 0;
  }
#endif

  *select_ok = inst->regression_ok;

  DEBUG_LOG(LOGF_SourceStats, "n=%d off=%f dist=%f var=%f selok=%d",
      inst->n_samples, offset, *root_distance, *variance, *select_ok);
}

/* ================================================== */

void
SST_GetTrackingData(SST_Stats inst, struct timeval *ref_time,
                    double *average_offset, double *offset_sd,
                    double *frequency, double *skew,
                    double *root_delay, double *root_dispersion)
{
  int i, j;
  double elapsed_sample;

  i = get_runsbuf_index(inst, inst->best_single_sample);
  j = get_buf_index(inst, inst->best_single_sample);

  *ref_time = inst->offset_time;
  *average_offset = inst->estimated_offset;
  *offset_sd = inst->estimated_offset_sd;
  *frequency = inst->estimated_frequency;
  *skew = inst->skew;
  *root_delay = inst->root_delays[j];

  UTI_DiffTimevalsToDouble(&elapsed_sample, &inst->offset_time, &inst->sample_times[i]);
  *root_dispersion = inst->root_dispersions[j] + inst->skew * elapsed_sample;

  DEBUG_LOG(LOGF_SourceStats, "n=%d freq=%f (%.3fppm) skew=%f (%.3fppm) avoff=%f offsd=%f disp=%f",
      inst->n_samples, *frequency, 1.0e6* *frequency, *skew, 1.0e6* *skew, *average_offset, *offset_sd, *root_dispersion);

}

/* ================================================== */

void
SST_SlewSamples(SST_Stats inst, struct timeval *when, double dfreq, double doffset)
{
  int m, i;
  double delta_time;
  struct timeval *sample, prev;
  double prev_offset, prev_freq;

  if (!inst->n_samples)
    return;

  for (m = -inst->runs_samples; m < inst->n_samples; m++) {
    i = get_runsbuf_index(inst, m);
    sample = &(inst->sample_times[i]);
    prev = *sample;
    UTI_AdjustTimeval(sample, when, sample, &delta_time, dfreq, doffset);
    prev_offset = inst->offsets[i];
    inst->offsets[i] += delta_time;

    DEBUG_LOG(LOGF_SourceStats, "i=%d old_st=[%s] new_st=[%s] old_off=%f new_off=%f",
        i, UTI_TimevalToString(&prev), UTI_TimevalToString(sample),
        prev_offset, inst->offsets[i]);
  }

  /* Do a half-baked update to the regression estimates */
  prev = inst->offset_time;
  prev_offset = inst->estimated_offset;
  prev_freq = inst->estimated_frequency;
  UTI_AdjustTimeval(&(inst->offset_time), when, &(inst->offset_time),
      &delta_time, dfreq, doffset);
  inst->estimated_offset += delta_time;
  inst->estimated_frequency -= dfreq;

  DEBUG_LOG(LOGF_SourceStats, "old_off_time=[%s] new=[%s] old_off=%f new_off=%f old_freq=%.3fppm new_freq=%.3fppm",
      UTI_TimevalToString(&prev), UTI_TimevalToString(&(inst->offset_time)),
      prev_offset, inst->estimated_offset,
      1.0e6*prev_freq, 1.0e6*inst->estimated_frequency);
}

/* ================================================== */

void 
SST_AddDispersion(SST_Stats inst, double dispersion)
{
  int m, i;

  for (m = 0; m < inst->n_samples; m++) {
    i = get_buf_index(inst, m);
    inst->root_dispersions[i] += dispersion;
    inst->peer_dispersions[i] += dispersion;
  }
}

/* ================================================== */

double
SST_PredictOffset(SST_Stats inst, struct timeval *when)
{
  double elapsed;
  
  if (inst->n_samples < 3) {
    /* We don't have any useful statistics, and presumably the poll
       interval is minimal.  We can't do any useful prediction other
       than use the latest sample or zero if we don't have any samples */
    if (inst->n_samples > 0) {
      return inst->offsets[inst->last_sample];
    } else {
      return 0.0;
    }
  } else {
    UTI_DiffTimevalsToDouble(&elapsed, when, &inst->offset_time);
    return inst->estimated_offset + elapsed * inst->estimated_frequency;
  }

}

/* ================================================== */

double
SST_MinRoundTripDelay(SST_Stats inst)
{
  if (!inst->n_samples)
    return DBL_MAX;
  return inst->peer_delays[inst->min_delay_sample];
}

/* ================================================== */

int
SST_IsGoodSample(SST_Stats inst, double offset, double delay,
    double max_delay_dev_ratio, double clock_error, struct timeval *when)
{
  double elapsed, allowed_increase, delay_increase;

  if (inst->n_samples < 3)
    return 1;

  UTI_DiffTimevalsToDouble(&elapsed, when, &inst->offset_time);

  /* Require that the ratio of the increase in delay from the minimum to the
     standard deviation is less than max_delay_dev_ratio. In the allowed
     increase in delay include also skew and clock_error. */
    
  allowed_increase = sqrt(inst->variance) * max_delay_dev_ratio +
    elapsed * (inst->skew + clock_error);
  delay_increase = (delay - SST_MinRoundTripDelay(inst)) / 2.0;

  if (delay_increase < allowed_increase)
    return 1;

  offset -= inst->estimated_offset + elapsed * inst->estimated_frequency;

  /* Before we decide to drop the sample, make sure the difference between
     measured offset and predicted offset is not significantly larger than
     the increase in delay */
  if (fabs(offset) - delay_increase > allowed_increase)
    return 1;

#if 0
  LOG(LOGS_INFO, LOGF_SourceStats, "bad sample: offset=%f delay=%f incr_delay=%f allowed=%f", offset, delay, allowed_increase, delay_increase);
#endif

  return 0;
}

/* ================================================== */
/* This is used to save the register to a file, so that we can reload
   it after restarting the daemon */

void
SST_SaveToFile(SST_Stats inst, FILE *out)
{
  int m, i, j;

  fprintf(out, "%d\n", inst->n_samples);

  for(m = 0; m < inst->n_samples; m++) {
    i = get_runsbuf_index(inst, m);
    j = get_buf_index(inst, m);

    fprintf(out, "%08lx %08lx %.6e %.6e %.6e %.6e %.6e %.6e %.6e %d\n",
            (unsigned long) inst->sample_times[i].tv_sec,
            (unsigned long) inst->sample_times[i].tv_usec,
            inst->offsets[i],
            inst->orig_offsets[j],
            inst->peer_delays[j],
            inst->peer_dispersions[j],
            inst->root_delays[j],
            inst->root_dispersions[j],
            1.0, /* used to be inst->weights[i] */
            inst->strata[j]);

  }
}

/* ================================================== */
/* This is used to reload samples from a file */

int
SST_LoadFromFile(SST_Stats inst, FILE *in)
{
  int i, line_number;
  char line[1024];
  unsigned long sec, usec;
  double weight;

  assert(!inst->n_samples);

  if (fgets(line, sizeof(line), in) &&
      sscanf(line, "%d", &inst->n_samples) == 1 &&
      inst->n_samples > 0 && inst->n_samples <= MAX_SAMPLES) {

    line_number = 2;

    for (i=0; i<inst->n_samples; i++) {
      if (!fgets(line, sizeof(line), in) ||
          (sscanf(line, "%lx%lx%lf%lf%lf%lf%lf%lf%lf%d\n",
                  &(sec), &(usec),
                  &(inst->offsets[i]),
                  &(inst->orig_offsets[i]),
                  &(inst->peer_delays[i]),
                  &(inst->peer_dispersions[i]),
                  &(inst->root_delays[i]),
                  &(inst->root_dispersions[i]),
                  &weight, /* not used anymore */
                  &(inst->strata[i])) != 10)) {

        /* This is the branch taken if the read FAILED */

        LOG(LOGS_WARN, LOGF_SourceStats, "Failed to read data from line %d of dump file", line_number);
        inst->n_samples = 0; /* Load abandoned if any sign of corruption */
        return 0;
      } else {

        /* This is the branch taken if the read is SUCCESSFUL */
        inst->sample_times[i].tv_sec = sec;
        inst->sample_times[i].tv_usec = usec;

        line_number++;
      }
    }
  } else {
    LOG(LOGS_WARN, LOGF_SourceStats, "Could not read number of samples from dump file");
    inst->n_samples = 0; /* Load abandoned if any sign of corruption */
    return 0;
  }

  inst->last_sample = inst->n_samples - 1;
  inst->runs_samples = 0;

  find_min_delay_sample(inst);

  return 1;

}

/* ================================================== */

void
SST_DoSourceReport(SST_Stats inst, RPT_SourceReport *report, struct timeval *now)
{
  int i, j;
  struct timeval ago;

  if (inst->n_samples > 0) {
    i = get_runsbuf_index(inst, inst->n_samples - 1);
    j = get_buf_index(inst, inst->n_samples - 1);
    report->orig_latest_meas = inst->orig_offsets[j];
    report->latest_meas = inst->offsets[i];
    report->latest_meas_err = 0.5*inst->root_delays[j] + inst->root_dispersions[j];
    report->stratum = inst->strata[j];

    UTI_DiffTimevals(&ago, now, &inst->sample_times[i]);
    report->latest_meas_ago = ago.tv_sec;
  } else {
    report->latest_meas_ago = 86400 * 365 * 10;
    report->orig_latest_meas = 0;
    report->latest_meas = 0;
    report->latest_meas_err = 0;
    report->stratum = 0;
  }
}


/* ================================================== */

SST_Skew_Direction SST_LastSkewChange(SST_Stats inst)
{
  return inst->skew_dirn;
}

/* ================================================== */

int
SST_Samples(SST_Stats inst)
{
  return inst->n_samples;
}

/* ================================================== */

void
SST_DoSourcestatsReport(SST_Stats inst, RPT_SourcestatsReport *report, struct timeval *now)
{
  double dspan;
  double elapsed, sample_elapsed;
  int li, lj, bi, bj;

  report->n_samples = inst->n_samples;
  report->n_runs = inst->nruns;

  if (inst->n_samples > 1) {
    li = get_runsbuf_index(inst, inst->n_samples - 1);
    lj = get_buf_index(inst, inst->n_samples - 1);
    UTI_DiffTimevalsToDouble(&dspan, &inst->sample_times[li],
        &inst->sample_times[get_runsbuf_index(inst, 0)]);
    report->span_seconds = (unsigned long) (dspan + 0.5);

    if (inst->n_samples > 3) {
      UTI_DiffTimevalsToDouble(&elapsed, now, &inst->offset_time);
      bi = get_runsbuf_index(inst, inst->best_single_sample);
      bj = get_buf_index(inst, inst->best_single_sample);
      UTI_DiffTimevalsToDouble(&sample_elapsed, now, &inst->sample_times[bi]);
      report->est_offset = inst->estimated_offset + elapsed * inst->estimated_frequency;
      report->est_offset_err = (inst->estimated_offset_sd +
                 sample_elapsed * inst->skew +
                 (0.5*inst->root_delays[bj] + inst->root_dispersions[bj]));
    } else {
      report->est_offset = inst->offsets[li];
      report->est_offset_err = 0.5*inst->root_delays[lj] + inst->root_dispersions[lj];
    }
  } else {
    report->span_seconds = 0;
    report->est_offset = 0;
    report->est_offset_err = 0;
  }

  report->resid_freq_ppm = 1.0e6 * inst->estimated_frequency;
  report->skew_ppm = 1.0e6 * inst->skew;
  report->sd = sqrt(inst->variance);
}

/* ================================================== */
