/*
  $Header: /cvs/src/chrony/sourcestats.c,v 1.40 2003/09/22 21:22:30 richard Exp $

  =======================================================================

  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Richard P. Curnow  1997-2003
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

  This file contains the routines that do the statistical
  analysis on the samples obtained from the sources,
  to determined frequencies and error bounds. */

#include "sysincl.h"

#include "sourcestats.h"
#include "memory.h"
#include "regress.h"
#include "util.h"
#include "conf.h"
#include "logging.h"
#include "local.h"
#include "mkdirpp.h"

/* ================================================== */
/* Define the maxumum number of samples that we want
   to store per source */
#define MAX_SAMPLES 64

/* This is the assumed worst case bound on an unknown frequency,
   2000ppm, which would be pretty bad */
#define WORST_CASE_FREQ_BOUND (2000.0/1.0e6)

/* Day number of 1 Jan 1970 */
#define MJD_1970 40587

/* ================================================== */
/* File to which statistics are logged, NULL if none */
static FILE *logfile = NULL;
static char *logfilename = NULL;
static unsigned long logwrites = 0;

#define STATISTICS_LOG "statistics.log"

/* ================================================== */
/* This data structure is used to hold the history of data from the
   source */

struct SST_Stats_Record {

  /* Reference ID of source, used for logging to statistics log */
  unsigned long refid;

  /* Number of samples currently stored.  sample[n_samples-1] is the
     newest.  The samples are expected to be sorted in order, but that
     probably doesn't matter.  */
  int n_samples;

  /* The index in the registers of the best individual sample that we
     are holding, in terms of the minimum root distance at the present
     time */
  int best_single_sample;

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
  struct timeval sample_times[MAX_SAMPLES];

  /* This is an array of offsets, in seconds, corresponding to the
     sample times.  In this module, we use the convention that
     positive means the local clock is FAST of the source and negative
     means it is SLOW.  This is contrary to the convention in the NTP
     stuff; that part of the code is written to correspond with
     RFC1305 conventions. */
  double offsets[MAX_SAMPLES];

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

  /* This array contains the weights to be used in the regression
     analysis for each of the samples. */
  double weights[MAX_SAMPLES];

  /* This array contains the strata that were associated with the sources
     at the times the samples were generated */
  int strata[MAX_SAMPLES];

};

/* ================================================== */

void
SST_Initialise(void)
{
  char *direc;

  if (CNF_GetLogStatistics()) {
    direc = CNF_GetLogDir();
    if (!mkdir_and_parents(direc)) {
      LOG(LOGS_ERR, LOGF_SourceStats, "Could not create directory %s", direc);
      logfile = NULL;
    } else {
      logfilename = MallocArray(char, 2 + strlen(direc) + strlen(STATISTICS_LOG));
      strcpy(logfilename, direc);
      strcat(logfilename, "/");
      strcat(logfilename, STATISTICS_LOG);
      logfile = fopen(logfilename, "a");
      if (!logfile) {
        LOG(LOGS_WARN, LOGF_SourceStats, "Couldn't open logfile %s for update", logfilename);
      }
    }
  }
}

/* ================================================== */

void
SST_Finalise(void)
{
  if (logfile) {
    fclose(logfile);
  }
}

/* ================================================== */
/* This function creates a new instance of the statistics handler */

SST_Stats
SST_CreateInstance(unsigned long refid)
{
  SST_Stats inst;
  inst = MallocNew(struct SST_Stats_Record);
  inst->refid = refid;
  inst->n_samples = 0;
  inst->estimated_frequency = 0;
  inst->skew = 2000.0e-6;
  inst->skew_dirn = SST_Skew_Nochange;
  inst->estimated_offset = 0.0;
  inst->estimated_offset_sd = 86400.0; /* Assume it's at least within a day! */
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
  return;
}

/* ================================================== */

static void
move_stats_entry(SST_Stats inst, int src, int dest)
{
  inst->sample_times[dest] = inst->sample_times[src];
  inst->offsets[dest] = inst->offsets[src];
  inst->orig_offsets[dest] = inst->orig_offsets[src];
  inst->peer_delays[dest] = inst->peer_delays[src];
  inst->peer_dispersions[dest] = inst->peer_dispersions[src];
  inst->root_delays[dest] = inst->root_delays[src];
  inst->root_dispersions[dest] = inst->root_dispersions[src];
  inst->weights[dest] = inst->weights[src];
  inst->strata[dest] = inst->strata[src];
}

/* ================================================== */
/* This function is called to prune the register down when it is full.
   For now, just discard the oldest sample.  */

static void
prune_register(SST_Stats inst, int new_oldest, int *bad_points)
{
  int i, j;

  if (!(new_oldest < inst->n_samples)) {
    CROAK("new_oldest should be < n_samples");
  }
   
  for (i=0, j=new_oldest; j<inst->n_samples; j++) {
    if (!bad_points || !bad_points[j]) {
      if (j != i) {
        move_stats_entry(inst, j, i);
      }
      i++;
    }
  }
  inst->n_samples = i;

}

/* ================================================== */

void
SST_AccumulateSample(SST_Stats inst, struct timeval *sample_time,
                     double offset,
                     double peer_delay, double peer_dispersion,
                     double root_delay, double root_dispersion,
                     int stratum)
{
  int n;
#if 0
  double root_distance;
#endif

  if (inst->n_samples == MAX_SAMPLES) {
    prune_register(inst, 1, NULL);
  }

  n = inst->n_samples;

  inst->sample_times[n] = *sample_time;
  inst->offsets[n] = offset;
  inst->orig_offsets[n] = offset;
  inst->peer_delays[n] = peer_delay;
  inst->peer_dispersions[n] = peer_dispersion;
  inst->root_delays[n] = root_delay;
  inst->root_dispersions[n] = root_dispersion;

#if 0
  /* The weight is worked out when we run the regression algorithm */
  root_distance = root_dispersion + 0.5 * fabs(root_delay);
  
  /* For now, this is the formula for the weight functions */
  inst->weights[n] = root_distance * root_distance;
#endif

  inst->strata[n] = stratum;
 
  ++inst->n_samples;

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

  newest_tv = &(inst->sample_times[inst->n_samples - 1]);
  for (i=0; i<inst->n_samples; i++) {
    /* The entries in times_back[] should end up negative */
    UTI_DiffTimevalsToDouble(&(times_back[i]), &(inst->sample_times[i]), newest_tv);
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
  int i, n, best_index;

  n = inst->n_samples - 1;
  best_root_distance = inst->root_dispersions[n] + 0.5 * fabs(inst->root_delays[n]);
  best_index = n;
#if 0
  LOG(LOGS_INFO, LOGF_SourceStats, "n=%d brd=%f", n, best_root_distance);
#endif
  for (i=0; i<n; i++) {
    elapsed = -times_back[i];
#if 0
    LOG(LOGS_INFO, LOGF_SourceStats, "n=%d i=%d latest=[%s] doing=[%s] elapsed=%f", n, i, 
        UTI_TimevalToString(&(inst->sample_times[n])),
        UTI_TimevalToString(&(inst->sample_times[i])),
        elapsed);
#endif

    /* Because the loop does not consider the most recent sample, this assertion must hold */
    if (elapsed <= 0.0) {
      LOG(LOGS_ERR, LOGF_SourceStats, "Elapsed<0! n=%d i=%d latest=[%s] doing=[%s] elapsed=%f",
          n, i, 
          UTI_TimevalToString(&(inst->sample_times[n])),
          UTI_TimevalToString(&(inst->sample_times[i])),
          elapsed);

      elapsed = fabs(elapsed);
    }

    root_distance = inst->root_dispersions[i] + elapsed * inst->skew + 0.5 * fabs(inst->root_delays[i]);
    if (root_distance < best_root_distance) {
      best_root_distance = root_distance;
      best_index = i;
    }
  }
  
  inst->best_single_sample = best_index;

#if 0
  LOG(LOGS_INFO, LOGF_SourceStats, "n=%d best_index=%d", n, best_index);
#endif

  return;
}

/* ================================================== */

/* This defines the assumed ratio between the standard deviation of
   the samples and the peer distance as measured from the round trip
   time.  E.g. a value of 4 means that we think the standard deviation
   is a quarter of the peer distance */

#define SD_TO_DIST_RATIO 8.0

/* ================================================== */
/* This function runs the linear regression operation on the data.  It
   finds the set of most recent samples that give the tightest
   confidence interval for the frequency, and truncates the register
   down to that number of samples */

void
SST_DoNewRegression(SST_Stats inst)
{
  double times_back[MAX_SAMPLES];
  double peer_distances[MAX_SAMPLES];

  int bad_points[MAX_SAMPLES];
  int degrees_of_freedom;
  int best_start;
  double est_intercept, est_slope, est_var, est_intercept_sd, est_slope_sd;
  int i, nruns;
  double min_distance;
  double sd_weight;
  double old_skew, old_freq, stress;

  int regression_ok;

  convert_to_intervals(inst, times_back);

  if (inst->n_samples > 0) {
    for (i=0; i<inst->n_samples; i++) {
      peer_distances[i] = 0.5 * fabs(inst->peer_delays[i]) + inst->peer_dispersions[i];
    }
  
    min_distance = peer_distances[0];
    for (i=1; i<inst->n_samples; i++) {
      if (peer_distances[i] < min_distance) {
        min_distance = peer_distances[i];
      }
    }

    /* And now, work out the weight vector */

    for (i=0; i<inst->n_samples; i++) {
      sd_weight = 1.0 + SD_TO_DIST_RATIO * (peer_distances[i] - min_distance) / min_distance;
      inst->weights[i] = sd_weight * sd_weight;
    } 
  }         

  regression_ok = RGR_FindBestRegression(times_back, inst->offsets, inst->weights,
                                         inst->n_samples,
                                         &est_intercept, &est_slope, &est_var,
                                         &est_intercept_sd, &est_slope_sd,
                                         &best_start, &nruns, &degrees_of_freedom);

  /* This is a legacy of when the regression routine found outliers
     for us.  We don't use it anymore. */
  memset((void *) bad_points, 0, MAX_SAMPLES * sizeof(int));

  if (regression_ok) {

    old_skew = inst->skew;
    old_freq = inst->estimated_frequency;
  
    inst->estimated_frequency = est_slope;
    inst->skew = est_slope_sd * RGR_GetTCoef(degrees_of_freedom);
    inst->estimated_offset = est_intercept;
    inst->offset_time = inst->sample_times[inst->n_samples - 1];
    inst->estimated_offset_sd = est_intercept_sd;
    inst->variance = est_var;
    inst->nruns = nruns;

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

    if (logfile) {

      if (((logwrites++) % 32) == 0) {
        fprintf(logfile,
                "==============================================================================================================\n"
                "   Date (UTC) Time     IP Address    Std dev'n Est offset  Offset sd  Diff freq   Est skew  Stress  Ns  Bs  Nr\n"
                "==============================================================================================================\n");
      }
      
            
      fprintf(logfile, "%s %-15s %10.3e %10.3e %10.3e %10.3e %10.3e %7.1e %3d %3d %3d\n",
              UTI_TimeToLogForm(inst->offset_time.tv_sec),
              UTI_IPToDottedQuad(inst->refid),
              sqrt(inst->variance),
              inst->estimated_offset,
              inst->estimated_offset_sd,
              inst->estimated_frequency,
              inst->skew,
              stress,
              inst->n_samples,
              best_start, nruns);

      fflush(logfile);
    }

    prune_register(inst, best_start, bad_points);

  } else {
#if 0
    LOG(LOGS_INFO, LOGF_SourceStats, "too few points (%d) for regression", inst->n_samples);
#endif
    inst->estimated_frequency = 0.0;
    inst->skew = WORST_CASE_FREQ_BOUND;
  }

  find_best_sample_index(inst, times_back);

}

/* ================================================== */
/* This function does a simple regression on what is in the register,
   without trying to optimise the error bounds on the frequency by
   deleting old samples */

void
SST_DoUpdateRegression(SST_Stats inst)
{
  double times_back[MAX_SAMPLES];
  double freq_error_bound;
  double est_intercept, est_slope, est_var_base, est_intercept_sd, est_slope_sd;

  convert_to_intervals(inst, times_back);

  if (inst->n_samples >= 3) { /* Otherwise, we're wasting our time - we
                                 can't do a useful linear regression
                                 with less than 3 points */
    
    RGR_WeightedRegression(times_back, inst->offsets, inst->weights,
                           inst->n_samples,
                           &est_intercept, &est_slope, &est_var_base,
                           &est_intercept_sd, &est_slope_sd);

    freq_error_bound = est_slope_sd * RGR_GetTCoef(inst->n_samples - 2);

    inst->estimated_frequency = est_slope;
    inst->skew = freq_error_bound;

  } else {
    inst->estimated_frequency = 0.0;
    inst->skew = WORST_CASE_FREQ_BOUND;
  }

  find_best_sample_index(inst, times_back);

}

/* ================================================== */

void
SST_GetReferenceData(SST_Stats inst, struct timeval *now, 
                     int *stratum, double *offset,
                     double *root_delay, double *root_dispersion,
                     double *frequency, double *skew)
{

  double elapsed;
  int n;

  *frequency = inst->estimated_frequency;
  *skew = inst->skew;

  n = inst->best_single_sample;

  UTI_DiffTimevalsToDouble(&elapsed, now, &(inst->sample_times[n]));
  *root_delay = inst->root_delays[n];
  *root_dispersion = inst->root_dispersions[n] + elapsed * inst->skew;
  *offset = inst->offsets[n] + elapsed * inst->estimated_frequency;
  *stratum = inst->strata[n];

#ifdef TRACEON
  LOG(LOGS_INFO, LOGF_SourceStats, "n=%d freq=%f skew=%f del=%f disp=%f ofs=%f str=%d",
      n, *frequency, *skew, *root_delay, *root_dispersion, *offset, *stratum);
#endif

  return;
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
}

/* ================================================== */

/* ================================================== */

void
SST_GetSelectionData(SST_Stats inst, struct timeval *now,
                     int *stratum,
                     double *best_offset, double *best_root_delay,
                     double *best_root_dispersion,
                     double *variance, int *average_ok)
{
  double average_offset;
  double sample_elapsed;
  double elapsed;
  int n;
  double peer_distance;
  
  n = inst->best_single_sample;
  *stratum = inst->strata[n];
  *variance = inst->variance;

  peer_distance = inst->peer_dispersions[n] + 0.5 * fabs(inst->peer_delays[n]);
  UTI_DiffTimevalsToDouble(&elapsed, now, &(inst->offset_time));

  UTI_DiffTimevalsToDouble(&sample_elapsed, now, &(inst->sample_times[n]));
  *best_offset = inst->offsets[n] + sample_elapsed * inst->estimated_frequency;
  *best_root_delay = inst->root_delays[n];
  *best_root_dispersion = inst->root_dispersions[n] + sample_elapsed * inst->skew;

  average_offset = inst->estimated_offset + inst->estimated_frequency * elapsed;
  if (fabs(average_offset - *best_offset) <= peer_distance) {
    *average_ok = 1;
  } else {
    *average_ok = 0;
  }

#ifdef TRACEON
  LOG(LOGS_INFO, LOGF_SourceStats, "n=%d off=%f del=%f dis=%f var=%f pdist=%f avoff=%f avok=%d",
      n, *best_offset, *best_root_delay, *best_root_dispersion, *variance,
      peer_distance, average_offset, *average_ok);
#endif

  return;
}

/* ================================================== */

void
SST_GetTrackingData(SST_Stats inst, struct timeval *now,
                    double *average_offset, double *offset_sd,
                    double *accrued_dispersion,
                    double *frequency, double *skew)
{
  int n;
  double peer_distance;
  double elapsed_offset, elapsed_sample;

  n = inst->best_single_sample;

  *frequency = inst->estimated_frequency;
  *skew = inst->skew;

  peer_distance = inst->peer_dispersions[n] + 0.5 * fabs(inst->peer_delays[n]);
  UTI_DiffTimevalsToDouble(&elapsed_offset, now, &(inst->offset_time));
  *average_offset = inst->estimated_offset + inst->estimated_frequency * elapsed_offset;
  *offset_sd = inst->estimated_offset_sd + elapsed_offset * inst->skew;

  UTI_DiffTimevalsToDouble(&elapsed_sample, now, &(inst->sample_times[n]));
  *accrued_dispersion = inst->skew * elapsed_sample;

#ifdef TRACEON
  LOG(LOGS_INFO, LOGF_SourceStats, "n=%d freq=%f (%.3fppm) skew=%f (%.3fppm) pdist=%f avoff=%f offsd=%f accrdis=%f",
      n, *frequency, 1.0e6* *frequency, *skew, 1.0e6* *skew, peer_distance, *average_offset, *offset_sd, *accrued_dispersion);
#endif

}

/* ================================================== */

void
SST_SlewSamples(SST_Stats inst, struct timeval *when, double dfreq, double doffset)
{
  int n, i;
  double elapsed;
  double delta_time;
  struct timeval *sample, prev;
  double prev_offset, prev_freq;

  n = inst->n_samples;

  for (i=0; i<n; i++) {
    sample = &(inst->sample_times[i]);
    prev = *sample;
#if 0
    UTI_AdjustTimeval(sample, when, sample, dfreq, doffset);
    /* Can't easily use this because we need to slew offset */
#endif
    UTI_DiffTimevalsToDouble(&elapsed, when, sample);
    delta_time = elapsed * dfreq - doffset;
    UTI_AddDoubleToTimeval(sample, delta_time, sample);
    prev_offset = inst->offsets[i];
    inst->offsets[i] += delta_time;
#ifdef TRACEON
    LOG(LOGS_INFO, LOGF_SourceStats, "i=%d old_st=[%s] new_st=[%s] old_off=%f new_off=%f",
        i, UTI_TimevalToString(&prev), UTI_TimevalToString(sample),
        prev_offset, inst->offsets[i]);
#endif
  }

  /* Do a half-baked update to the regression estimates */
  UTI_DiffTimevalsToDouble(&elapsed, when, &(inst->offset_time));
  prev = inst->offset_time;
  delta_time = elapsed * dfreq - doffset;
  UTI_AddDoubleToTimeval(&(inst->offset_time), delta_time, &(inst->offset_time));
  prev_offset = inst->estimated_offset;
  prev_freq = inst->estimated_frequency;
  inst->estimated_offset += delta_time;
  inst->estimated_frequency -= dfreq;

#ifdef TRACEON
  LOG(LOGS_INFO, LOGF_SourceStats, "old_off_time=[%s] new=[%s] old_off=%f new_off=%f old_freq=%.3fppm new_freq=%.3fppm",
      UTI_TimevalToString(&prev), UTI_TimevalToString(&(inst->offset_time)),
      prev_offset, inst->estimated_offset,
      1.0e6*prev_freq, 1.0e6*inst->estimated_frequency);
#endif

  return;
}

/* ================================================== */

double
SST_PredictOffset(SST_Stats inst, struct timeval *when)
{
  double elapsed;
  
  if (inst->n_samples < 3) {
    /* We don't have any useful statistics, and presumably the poll
       interval is minimal.  We can't do any useful prediction other
       than use the latest sample */
    return inst->offsets[inst->n_samples - 1];
  } else {
    UTI_DiffTimevalsToDouble(&elapsed, when, &inst->offset_time);
    return inst->estimated_offset + elapsed * inst->estimated_frequency;
  }

}

/* ================================================== */

double
SST_MinRoundTripDelay(SST_Stats inst)
{
  double min_delay, delay;
  int i;

  if (inst->n_samples == 0) {
    return DBL_MAX;
  } else {
    min_delay = fabs(inst->peer_delays[0]);
    for (i=1; i<inst->n_samples; i++) {
      delay = fabs(inst->peer_delays[i]);
      if (delay < min_delay) {
        min_delay = delay;
      }
    }
    return min_delay;
  }
}

/* ================================================== */
/* This is used to save the register to a file, so that we can reload
   it after restarting the daemon */

void
SST_SaveToFile(SST_Stats inst, FILE *out)
{
  int i;

  fprintf(out, "%d\n", inst->n_samples);

  for(i=0; i<inst->n_samples; i++) {

    fprintf(out, "%08lx %08lx %.6e %.6e %.6e %.6e %.6e %.6e %.6e %d\n",
            (unsigned long) inst->sample_times[i].tv_sec,
            (unsigned long) inst->sample_times[i].tv_usec,
            inst->offsets[i],
            inst->orig_offsets[i],
            inst->peer_delays[i],
            inst->peer_dispersions[i],
            inst->root_delays[i],
            inst->root_dispersions[i],
            inst->weights[i],
            inst->strata[i]);

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

  if (fgets(line, sizeof(line), in) &&
      (sscanf(line, "%d", &inst->n_samples) == 1)) {

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
                  &(inst->weights[i]),
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

  return 1;

}

/* ================================================== */

void
SST_DoSourceReport(SST_Stats inst, RPT_SourceReport *report, struct timeval *now)
{
  int n, nb;
  double est_offset, est_err, elapsed, sample_elapsed;
  struct timeval ago;

  if (inst->n_samples > 0) {
    n = inst->n_samples - 1;
    report->orig_latest_meas = (long)(0.5 + 1.0e6 * inst->orig_offsets[n]);
    report->latest_meas = (long)(0.5 + 1.0e6 * inst->offsets[n]);
    report->latest_meas_err = (unsigned long)(0.5 + 1.0e6 * (0.5*inst->root_delays[n] + inst->root_dispersions[n]));
    report->stratum = inst->strata[n];

    UTI_DiffTimevals(&ago, now, &inst->sample_times[n]);
    report->latest_meas_ago = ago.tv_sec;

    if (inst->n_samples > 3) {
      UTI_DiffTimevalsToDouble(&elapsed, now, &inst->offset_time);
      nb = inst->best_single_sample;
      UTI_DiffTimevalsToDouble(&sample_elapsed, now, &(inst->sample_times[nb]));
      est_offset = inst->estimated_offset + elapsed * inst->estimated_frequency;
      est_err = (inst->estimated_offset_sd +
                 sample_elapsed * inst->skew +
                 (0.5*inst->root_delays[nb] + inst->root_dispersions[nb]));
      report->est_offset = (long)(0.5 + 1.0e6 * est_offset);
      report->est_offset_err = (unsigned long) (0.5 + 1.0e6 * est_err);
      report->resid_freq = (long) (0.5 * 1.0e9 * inst->estimated_frequency);
      report->resid_skew = (unsigned long) (0.5 + 1.0e9 * inst->skew);
    } else {
      report->est_offset = report->latest_meas;
      report->est_offset_err = report->latest_meas_err;
      report->resid_freq = 0;
      report->resid_skew = 0;
    }
  } else {
    report->orig_latest_meas = 0;
    report->latest_meas = 0;
    report->latest_meas_err = 0;
    report->stratum = 0;
    report->est_offset = 0;
    report->est_offset_err = 0;
    report->resid_freq = 0;
    report->resid_skew = 0;
  }
}


/* ================================================== */

SST_Skew_Direction SST_LastSkewChange(SST_Stats inst)
{
  return inst->skew_dirn;
}

/* ================================================== */

void
SST_DoSourcestatsReport(SST_Stats inst, RPT_SourcestatsReport *report)
{
  double dspan;
  int n;

  report->n_samples = inst->n_samples;
  report->n_runs = inst->nruns;

  if (inst->n_samples > 1) {
    n = inst->n_samples - 1;
    UTI_DiffTimevalsToDouble(&dspan, &inst->sample_times[n], &inst->sample_times[0]);
    report->span_seconds = (unsigned long) (dspan + 0.5);
  } else {
    report->span_seconds = 0;
  }

  report->resid_freq_ppm = 1.0e6 * inst->estimated_frequency;
  report->skew_ppm = 1.0e6 * inst->skew;
  report->sd_us = 1.0e6 * sqrt(inst->variance);
}

/* ================================================== */

void
SST_CycleLogFile(void)
{
  if (logfile && logfilename) {
    fclose(logfile);
    logfile = fopen(logfilename, "a");
    if (!logfile) {
      LOG(LOGS_WARN, LOGF_SourceStats, "Could not reopen logfile %s", logfilename);
    }
    logwrites = 0;
  }
}

/* ================================================== */
