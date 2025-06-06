/*
  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Richard P. Curnow  1997-2003
 * Copyright (C) Miroslav Lichvar  2009-2018, 2020, 2022
 * Copyright (C) Andy Fiddaman  2024
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

  This module keeps track of the source which we are claiming to be
  our reference, for the purposes of generating outgoing NTP packets */

#include "config.h"

#include "sysincl.h"

#include "memory.h"
#include "reference.h"
#include "util.h"
#include "conf.h"
#include "leapdb.h"
#include "logging.h"
#include "local.h"
#include "sched.h"

/* ================================================== */

/* The minimum allowed skew */
#define MIN_SKEW 1.0e-12

/* The update interval of the reference in the local reference mode */
#define LOCAL_REF_UPDATE_INTERVAL 64.0

static int are_we_synchronised;
static int enable_local_stratum;
static int local_stratum;
static int local_orphan;
static double local_distance;
static int local_activate_ok;
static double local_activate;
static double local_wait_synced;
static double local_wait_unsynced;
static struct timespec local_ref_time;
static NTP_Leap our_leap_status;
static int our_leap_sec;
static int our_tai_offset;
static int our_stratum;
static uint32_t our_ref_id;
static IPAddr our_ref_ip;
static struct timespec our_ref_time;
static double unsynchronised_since;
static double our_skew;
static double our_residual_freq;
static double our_root_delay;
static double our_root_dispersion;
static double our_offset_sd;
static double our_frequency_sd;

static double max_update_skew;

static double last_offset;
static double avg2_offset;
static int avg2_moving;

static double correction_time_ratio;

/* Flag indicating that we are initialised */
static int initialised = 0;

/* Current operating mode */
static REF_Mode mode;

/* Threshold and update limit for stepping clock */
static int make_step_limit;
static double make_step_threshold;

/* Number of updates before offset checking, number of ignored updates
   before exiting and the maximum allowed offset */
static int max_offset_delay;
static int max_offset_ignore;
static double max_offset;

/* Threshold for logging clock changes to syslog */
static double log_change_threshold;

/* Flag, threshold and user for sending mail notification on large clock changes */
static int do_mail_change;
static double mail_change_threshold;
static char *mail_change_user;

/* Handler for mode ending */
static REF_ModeEndHandler mode_end_handler = NULL;

/* Filename of the drift file. */
static char *drift_file=NULL;
static double drift_file_age;
static int drift_file_interval;

static void update_drift_file(double, double);

/* Leap second handling mode */
static REF_LeapMode leap_mode;

/* Time of UTC midnight of the upcoming or previous leap second */
static time_t leap_when;

/* Flag indicating the clock was recently corrected for leap second and it may
   not have correct time yet (missing 23:59:60 in the UTC time scale) */
static int leap_in_progress;

/* Timer for the leap second handler */
static SCH_TimeoutID leap_timeout_id;

/* ================================================== */

static LOG_FileID logfileid;

/* ================================================== */

/* Exponential moving averages of absolute clock frequencies
   used as a fallback when synchronisation is lost. */

struct fb_drift {
  double freq;
  double secs;
};

static int fb_drift_min;
static int fb_drift_max;

static struct fb_drift *fb_drifts = NULL;
static int next_fb_drift;
static SCH_TimeoutID fb_drift_timeout_id;

/* Monotonic timestamp of the last reference update */
static double last_ref_update;
static double last_ref_update_interval;

static double last_ref_adjustment;
static int ref_adjustments;

/* ================================================== */

static void update_leap_status(NTP_Leap leap, time_t now, int reset);

/* ================================================== */

static void
handle_slew(struct timespec *raw,
            struct timespec *cooked,
            double dfreq,
            double doffset,
            LCL_ChangeType change_type,
            void *anything)
{
  double delta;
  struct timespec now;

  if (!UTI_IsZeroTimespec(&our_ref_time))
    UTI_AdjustTimespec(&our_ref_time, cooked, &our_ref_time, &delta, dfreq, doffset);

  if (change_type == LCL_ChangeUnknownStep) {
    last_ref_update = 0.0;
    REF_SetUnsynchronised();
  }

  /* When the clock was stepped, check if that doesn't change our leap status
     and also reset the leap timeout to undo the shift in the scheduler */
  if (change_type != LCL_ChangeAdjust && our_leap_sec && !leap_in_progress) {
    LCL_ReadRawTime(&now);
    update_leap_status(our_leap_status, now.tv_sec, 1);
  }
}

/* ================================================== */

void
REF_Initialise(void)
{
  FILE *in;
  double file_freq_ppm, file_skew_ppm;
  double our_frequency_ppm;

  mode = REF_ModeNormal;
  are_we_synchronised = 0;
  our_leap_status = LEAP_Unsynchronised;
  our_leap_sec = 0;
  our_tai_offset = 0;
  initialised = 1;
  our_root_dispersion = 1.0;
  our_root_delay = 1.0;
  our_frequency_ppm = 0.0;
  our_skew = 1.0; /* i.e. rather bad */
  our_residual_freq = 0.0;
  our_frequency_sd = 0.0;
  our_offset_sd = 0.0;
  drift_file_age = 0.0;
  local_activate_ok = 0;

  /* Now see if we can get the drift file opened */
  drift_file = CNF_GetDriftFile(&drift_file_interval);
  if (drift_file) {
    in = UTI_OpenFile(NULL, drift_file, NULL, 'r', 0);
    if (in) {
      if (fscanf(in, "%lf%lf", &file_freq_ppm, &file_skew_ppm) == 2) {
        /* We have read valid data */
        our_frequency_ppm = file_freq_ppm;
        our_skew = 1.0e-6 * file_skew_ppm;
        if (our_skew < MIN_SKEW)
          our_skew = MIN_SKEW;
        LOG(LOGS_INFO, "Frequency %.3f +/- %.3f ppm read from %s",
            file_freq_ppm, file_skew_ppm, drift_file);
        LCL_SetAbsoluteFrequency(our_frequency_ppm);
      } else {
        LOG(LOGS_WARN, "Could not read valid frequency and skew from driftfile %s",
            drift_file);
      }
      fclose(in);
    }
  }
    
  if (our_frequency_ppm == 0.0) {
    our_frequency_ppm = LCL_ReadAbsoluteFrequency();
    if (our_frequency_ppm != 0.0) {
      LOG(LOGS_INFO, "Initial frequency %.3f ppm", our_frequency_ppm);
    }
  }

  logfileid = CNF_GetLogTracking() ? LOG_FileOpen("tracking",
      "   Date (UTC) Time     IP Address   St   Freq ppm   Skew ppm     Offset L Co  Offset sd Rem. corr. Root delay Root disp. Max. error")
    : -1;

  max_update_skew = fabs(CNF_GetMaxUpdateSkew()) * 1.0e-6;

  correction_time_ratio = CNF_GetCorrectionTimeRatio();

  enable_local_stratum = CNF_AllowLocalReference(&local_stratum, &local_orphan,
                                                 &local_distance, &local_activate,
                                                 &local_wait_synced,
                                                 &local_wait_unsynced);
  UTI_ZeroTimespec(&local_ref_time);
  unsynchronised_since = SCH_GetLastEventMonoTime();

  leap_when = 0;
  leap_timeout_id = 0;
  leap_in_progress = 0;
  leap_mode = CNF_GetLeapSecMode();
  /* Switch to step mode if the system driver doesn't support leap */
  if (leap_mode == REF_LeapModeSystem && !LCL_CanSystemLeap())
    leap_mode = REF_LeapModeStep;

  CNF_GetMakeStep(&make_step_limit, &make_step_threshold);
  CNF_GetMaxChange(&max_offset_delay, &max_offset_ignore, &max_offset);
  CNF_GetMailOnChange(&do_mail_change, &mail_change_threshold, &mail_change_user);
  log_change_threshold = CNF_GetLogChange();

  CNF_GetFallbackDrifts(&fb_drift_min, &fb_drift_max);

  if (fb_drift_max >= fb_drift_min && fb_drift_min > 0) {
    fb_drifts = MallocArray(struct fb_drift, fb_drift_max - fb_drift_min + 1);
    memset(fb_drifts, 0, sizeof (struct fb_drift) * (fb_drift_max - fb_drift_min + 1));
    next_fb_drift = 0;
    fb_drift_timeout_id = 0;
  }

  UTI_ZeroTimespec(&our_ref_time);
  last_ref_update = 0.0;
  last_ref_update_interval = 0.0;
  last_ref_adjustment = 0.0;
  ref_adjustments = 0;

  LCL_AddParameterChangeHandler(handle_slew, NULL);

  /* Make first entry in tracking log */
  REF_SetUnsynchronised();
}

/* ================================================== */

void
REF_Finalise(void)
{
  update_leap_status(LEAP_Unsynchronised, 0, 0);

  if (drift_file) {
    update_drift_file(LCL_ReadAbsoluteFrequency(), our_skew);
  }

  LCL_RemoveParameterChangeHandler(handle_slew, NULL);

  Free(fb_drifts);

  initialised = 0;
}

/* ================================================== */

void REF_SetMode(REF_Mode new_mode)
{
  mode = new_mode;
}

/* ================================================== */

REF_Mode
REF_GetMode(void)
{
  return mode;
}

/* ================================================== */

void
REF_SetModeEndHandler(REF_ModeEndHandler handler)
{
  mode_end_handler = handler;
}

/* ================================================== */

REF_LeapMode
REF_GetLeapMode(void)
{
  return leap_mode;
}

/* ================================================== */
/* Update the drift coefficients to the file. */

static void
update_drift_file(double freq_ppm, double skew)
{
  FILE *out;

  /* Create a temporary file with a '.tmp' extension. */
  out = UTI_OpenFile(NULL, drift_file, ".tmp", 'w', 0644);
  if (!out)
    return;

  /* Write the frequency and skew parameters in ppm */
  fprintf(out, "%20.6f %20.6f\n", freq_ppm, 1.0e6 * skew);
  fclose(out);

  /* Rename the temporary file to the correct location */
  if (!UTI_RenameTempFile(NULL, drift_file, ".tmp", NULL))
    ;
}

/* ================================================== */

static void
update_fb_drifts(double freq_ppm, double update_interval)
{
  int i, secs;

  assert(are_we_synchronised);

  if (next_fb_drift > 0) {
#if 0
    /* Reset drifts that were used when we were unsynchronised */
    for (i = 0; i < next_fb_drift - fb_drift_min; i++)
      fb_drifts[i].secs = 0.0;
#endif
    next_fb_drift = 0;
  }

  SCH_RemoveTimeout(fb_drift_timeout_id);
  fb_drift_timeout_id = 0;

  if (update_interval < 1.0 || update_interval > last_ref_update_interval * 4.0)
    return;

  for (i = 0; i < fb_drift_max - fb_drift_min + 1; i++) {
    secs = 1 << (i + fb_drift_min);
    if (fb_drifts[i].secs < secs) {
      /* Calculate average over 2 * secs interval before switching to
         exponential updating */
      fb_drifts[i].freq = (fb_drifts[i].freq * fb_drifts[i].secs +
          update_interval * 0.5 * freq_ppm) / (update_interval * 0.5 + fb_drifts[i].secs);
      fb_drifts[i].secs += update_interval * 0.5;
    } else {
      /* Update exponential moving average. The smoothing factor for update
         interval equal to secs is about 0.63, for half interval about 0.39,
         for double interval about 0.86. */
      fb_drifts[i].freq += (1 - 1.0 / exp(update_interval / secs)) *
        (freq_ppm - fb_drifts[i].freq);
    }

    DEBUG_LOG("Fallback drift %d updated: %f ppm %f seconds",
              i + fb_drift_min, fb_drifts[i].freq, fb_drifts[i].secs);
  }
}

/* ================================================== */

static void
fb_drift_timeout(void *arg)
{
  assert(next_fb_drift >= fb_drift_min && next_fb_drift <= fb_drift_max);

  fb_drift_timeout_id = 0;

  DEBUG_LOG("Fallback drift %d active: %f ppm",
            next_fb_drift, fb_drifts[next_fb_drift - fb_drift_min].freq);
  LCL_SetAbsoluteFrequency(fb_drifts[next_fb_drift - fb_drift_min].freq);
  REF_SetUnsynchronised();
}

/* ================================================== */

static void
schedule_fb_drift(void)
{
  int i, c, secs;
  double unsynchronised, now;

  if (fb_drift_timeout_id)
    return; /* already scheduled */

  now = SCH_GetLastEventMonoTime();
  unsynchronised = now - last_ref_update;

  for (c = secs = 0, i = fb_drift_min; i <= fb_drift_max; i++) {
    secs = 1 << i;

    if (fb_drifts[i - fb_drift_min].secs < secs)
      continue;

    if (unsynchronised < secs && i > next_fb_drift)
      break;

    c = i;
  }

  if (c > next_fb_drift) {
    LCL_SetAbsoluteFrequency(fb_drifts[c - fb_drift_min].freq);
    next_fb_drift = c;
    DEBUG_LOG("Fallback drift %d set", c);
  }

  if (i <= fb_drift_max) {
    next_fb_drift = i;
    fb_drift_timeout_id = SCH_AddTimeoutByDelay(secs - unsynchronised, fb_drift_timeout, NULL);
    DEBUG_LOG("Fallback drift %d scheduled", i);
  }
}

/* ================================================== */

static void
end_ref_mode(int result)
{
  mode = REF_ModeIgnore;

  /* Dispatch the handler */
  if (mode_end_handler)
    (mode_end_handler)(result);
}

/* ================================================== */

#define BUFLEN 255
#define S_MAX_USER_LEN "128"

static void
maybe_log_offset(double offset, time_t now)
{
  double abs_offset;
  FILE *p;
  char buffer[BUFLEN], host[BUFLEN];
  struct tm *tm;

  abs_offset = fabs(offset);

  if (abs_offset > log_change_threshold) {
    LOG(LOGS_WARN, "System clock wrong by %.6f seconds", -offset);
  }

  if (do_mail_change &&
      (abs_offset > mail_change_threshold)) {
    snprintf(buffer, sizeof (buffer), "%s -t", MAIL_PROGRAM);
    p = popen(buffer, "w");
    if (p) {
      if (gethostname(host, sizeof(host)) < 0) {
        strcpy(host, "<UNKNOWN>");
      }
      host[sizeof (host) - 1] = '\0';

      fprintf(p, "To: %s\n", mail_change_user);
      fprintf(p, "Subject: chronyd reports change to system clock on node [%s]\n", host);
      fputs("\n", p);

      tm = localtime(&now);
      if (tm) {
        strftime(buffer, sizeof (buffer),
                 "On %A, %d %B %Y\n  with the system clock reading %H:%M:%S (%Z)", tm);
        fputs(buffer, p);
      }

      /* If offset < 0 the local clock is slow, so we are applying a
         positive change to it to bring it into line, hence the
         negation of 'offset' in the next statement (and earlier) */
      fprintf(p,
              "\n\nchronyd started to apply an adjustment of %.3f seconds to it,\n"
              "  which exceeded the reporting threshold of %.3f seconds\n\n",
              -offset, mail_change_threshold);
      pclose(p);
    } else {
      LOG(LOGS_ERR, "Could not send mail notification to user %s\n",
          mail_change_user);
    }
  }

}

/* ================================================== */

static int
is_step_limit_reached(double offset, double offset_correction)
{
  if (make_step_limit == 0) {
    return 0;
  } else if (make_step_limit > 0) {
    make_step_limit--;
  }
  return fabs(offset - offset_correction) > make_step_threshold;
}

/* ================================================== */

static int
is_offset_ok(double offset)
{
  if (max_offset_delay < 0)
    return 1;

  if (max_offset_delay > 0) {
    max_offset_delay--;
    return 1;
  }

  if (fabs(offset) > max_offset) {
    LOG(LOGS_WARN, 
        "Adjustment of %.3f seconds exceeds the allowed maximum of %.3f seconds (%s) ",
        -offset, max_offset, !max_offset_ignore ? "exiting" : "ignored");
    if (!max_offset_ignore)
      end_ref_mode(0);
    else if (max_offset_ignore > 0)
      max_offset_ignore--;
    return 0;
  }
  return 1;
}

/* ================================================== */

static int
is_leap_second_day(time_t when)
{
  struct tm *stm;

  stm = gmtime(&when);
  if (!stm)
    return 0;

  /* Allow leap second only on the last day of June and December */
  return (stm->tm_mon == 5 && stm->tm_mday == 30) ||
         (stm->tm_mon == 11 && stm->tm_mday == 31);
}

/* ================================================== */

static void
leap_end_timeout(void *arg)
{
  leap_timeout_id = 0;
  leap_in_progress = 0;

  if (our_tai_offset)
    our_tai_offset += our_leap_sec;
  our_leap_sec = 0;

  if (leap_mode == REF_LeapModeSystem)
    LCL_SetSystemLeap(our_leap_sec, our_tai_offset);

  if (our_leap_status == LEAP_InsertSecond ||
      our_leap_status == LEAP_DeleteSecond)
    our_leap_status = LEAP_Normal;
}

/* ================================================== */

static void
leap_start_timeout(void *arg)
{
  leap_in_progress = 1;

  switch (leap_mode) {
    case REF_LeapModeSystem:
      DEBUG_LOG("Waiting for system clock leap second correction");
      break;
    case REF_LeapModeSlew:
      LCL_NotifyLeap(our_leap_sec);
      LCL_AccumulateOffset(our_leap_sec, 0.0);
      LOG(LOGS_WARN, "Adjusting system clock for leap second");
      break;
    case REF_LeapModeStep:
      LCL_NotifyLeap(our_leap_sec);
      LCL_ApplyStepOffset(our_leap_sec);
      LOG(LOGS_WARN, "System clock was stepped for leap second");
      break;
    case REF_LeapModeIgnore:
      LOG(LOGS_WARN, "Ignoring leap second");
      break;
    default:
      break;
  }

  /* Wait until the leap second is over with some extra room to be safe */
  leap_timeout_id = SCH_AddTimeoutByDelay(2.0, leap_end_timeout, NULL);
}

/* ================================================== */

static void
set_leap_timeout(time_t now)
{
  struct timespec when;

  /* Stop old timer if there is one */
  SCH_RemoveTimeout(leap_timeout_id);
  leap_timeout_id = 0;
  leap_in_progress = 0;

  if (!our_leap_sec)
    return;

  leap_when = (now / (24 * 3600) + 1) * (24 * 3600);

  /* Insert leap second at 0:00:00 UTC, delete at 23:59:59 UTC.  If the clock
     will be corrected by the system, timeout slightly sooner to be sure it
     will happen before the system correction. */
  when.tv_sec = leap_when;
  when.tv_nsec = 0;
  if (our_leap_sec < 0)
    when.tv_sec--;
  if (leap_mode == REF_LeapModeSystem) {
    when.tv_sec--;
    when.tv_nsec = 500000000;
  }

  leap_timeout_id = SCH_AddTimeout(&when, leap_start_timeout, NULL);
}

/* ================================================== */

static void
update_leap_status(NTP_Leap leap, time_t now, int reset)
{
  NTP_Leap ldb_leap;
  int leap_sec, tai_offset;

  leap_sec = 0;
  tai_offset = 0;

  if (now) {
    ldb_leap = LDB_GetLeap(now, &tai_offset);
    if (leap == LEAP_Normal)
      leap = ldb_leap;
  }

  if (leap == LEAP_InsertSecond || leap == LEAP_DeleteSecond) {
    /* Check that leap second is allowed today */

    if (is_leap_second_day(now)) {
      if (leap == LEAP_InsertSecond) {
        leap_sec = 1;
      } else {
        leap_sec = -1;
      }
    } else {
      leap = LEAP_Normal;
    }
  }
  
  if ((leap_sec != our_leap_sec || tai_offset != our_tai_offset)
      && !REF_IsLeapSecondClose(NULL, 0.0)) {
    our_leap_sec = leap_sec;
    our_tai_offset = tai_offset;

    switch (leap_mode) {
      case REF_LeapModeSystem:
        LCL_SetSystemLeap(our_leap_sec, our_tai_offset);
        /* Fall through */
      case REF_LeapModeSlew:
      case REF_LeapModeStep:
      case REF_LeapModeIgnore:
        set_leap_timeout(now);
        break;
      default:
        assert(0);
        break;
    }
  } else if (reset) {
    set_leap_timeout(now);
  }

  our_leap_status = leap;
}

/* ================================================== */

static double
get_root_dispersion(struct timespec *ts)
{
  if (UTI_IsZeroTimespec(&our_ref_time))
    return 1.0;

  return our_root_dispersion +
         fabs(UTI_DiffTimespecsToDouble(ts, &our_ref_time)) *
         (our_skew + fabs(our_residual_freq) + LCL_GetMaxClockError());
}

/* ================================================== */

static void
update_sync_status(struct timespec *now)
{
  double elapsed;

  elapsed = fabs(UTI_DiffTimespecsToDouble(now, &our_ref_time));

  LCL_SetSyncStatus(are_we_synchronised,
                    our_offset_sd + elapsed * our_frequency_sd,
                    our_root_delay / 2.0 + get_root_dispersion(now));
}

/* ================================================== */

static void
write_log(struct timespec *now, int combined_sources, double freq,
          double offset, double offset_sd, double uncorrected_offset,
          double orig_root_distance)
{
  const char leap_codes[4] = {'N', '+', '-', '?'};
  double root_dispersion, max_error;
  static double last_sys_offset = 0.0;

  if (logfileid == -1)
    return;

  max_error = orig_root_distance + fabs(last_sys_offset);
  root_dispersion = get_root_dispersion(now);
  last_sys_offset = offset - uncorrected_offset;

  LOG_FileWrite(logfileid,
                "%s %-15s %2d %10.3f %10.3f %10.3e %1c %2d %10.3e %10.3e %10.3e %10.3e %10.3e",
                UTI_TimeToLogForm(now->tv_sec),
                our_ref_ip.family != IPADDR_UNSPEC ?
                  UTI_IPToString(&our_ref_ip) : UTI_RefidToString(our_ref_id),
                our_stratum, freq, 1.0e6 * our_skew, offset,
                leap_codes[our_leap_status], combined_sources, offset_sd,
                uncorrected_offset, our_root_delay, root_dispersion, max_error);
}

/* ================================================== */

static void
special_mode_sync(int valid, double offset)
{
  int step;

  switch (mode) {
    case REF_ModeInitStepSlew:
      if (!valid) {
        LOG(LOGS_WARN, "No suitable source for initstepslew");
        end_ref_mode(0);
        break;
      }

      step = fabs(offset) >= CNF_GetInitStepThreshold();

      LOG(LOGS_INFO, "System's initial offset : %.6f seconds %s of true (%s)",
          fabs(offset), offset >= 0 ? "fast" : "slow", step ? "step" : "slew");

      if (step)
        LCL_ApplyStepOffset(offset);
      else
        LCL_AccumulateOffset(offset, 0.0);

      end_ref_mode(1);

      break;
    case REF_ModeUpdateOnce:
    case REF_ModePrintOnce:
      if (!valid) {
        LOG(LOGS_WARN, "No suitable source for synchronisation");
        end_ref_mode(0);
        break;
      }

      step = mode == REF_ModeUpdateOnce;

      LOG(LOGS_INFO, "System clock wrong by %.6f seconds (%s)",
          -offset, step ? "step" : "ignored");

      if (step)
        LCL_ApplyStepOffset(offset);

      end_ref_mode(1);

      break;
    case REF_ModeIgnore:
      /* Do nothing until the mode is changed */
      break;
    default:
      assert(0);
  }
}

/* ================================================== */

static void
get_clock_estimates(int manual,
                    double measured_freq, double measured_skew,
                    double *estimated_freq, double *estimated_skew,
                    double *residual_freq)
{
  double gain, expected_freq, expected_skew, extra_skew;

  /* We assume that the local clock is running according to our previously
     determined value */
  expected_freq = 0.0;
  expected_skew = our_skew;

  /* Set new frequency based on weighted average of the expected and measured
     skew.  Disable updates that are based on totally unreliable frequency
     information unless it is a manual reference. */
  if (manual) {
    gain = 1.0;
  } else if (fabs(measured_skew) > max_update_skew) {
    DEBUG_LOG("Skew %f too large to track", measured_skew);
    gain = 0.0;
  } else {
    gain = 3.0 * SQUARE(expected_skew) /
           (3.0 * SQUARE(expected_skew) + SQUARE(measured_skew));
  }

  gain = CLAMP(0.0, gain, 1.0);

  *estimated_freq = expected_freq + gain * (measured_freq - expected_freq);
  *residual_freq = measured_freq - *estimated_freq;

  extra_skew = sqrt(SQUARE(expected_freq - *estimated_freq) * (1.0 - gain) +
                    SQUARE(measured_freq - *estimated_freq) * gain);

  *estimated_skew = expected_skew + gain * (measured_skew - expected_skew) + extra_skew;
}

/* ================================================== */

static void
fuzz_ref_time(struct timespec *ts)
{
  uint32_t rnd;

  /* Add a random value from interval [-1.0, 0.0] */
  UTI_GetRandomBytes(&rnd, sizeof (rnd));
  UTI_AddDoubleToTimespec(ts, -(double)rnd / (uint32_t)-1, ts);
}

/* ================================================== */

static double
get_correction_rate(double offset_sd, double update_interval)
{
  /* We want to correct the offset quickly, but we also want to keep the
     frequency error caused by the correction itself low.

     Define correction rate as the area of the region bounded by the graph of
     offset corrected in time.  Set the rate so that the time needed to correct
     an offset equal to the current sourcestats stddev will be equal to the
     update interval multiplied by the correction time ratio (assuming linear
     adjustment).  The offset and the time needed to make the correction are
     inversely proportional.

     This is only a suggestion and it's up to the system driver how the
     adjustment will be executed. */

  return correction_time_ratio * 0.5 * offset_sd * update_interval;
}

/* ================================================== */

void
REF_SetReference(int stratum, NTP_Leap leap, int combined_sources,
                 uint32_t ref_id, IPAddr *ref_ip, struct timespec *ref_time,
                 double offset, double offset_sd,
                 double frequency, double frequency_sd, double skew,
                 double root_delay, double root_dispersion)
{
  double uncorrected_offset, accumulate_offset, step_offset;
  double residual_frequency, local_abs_frequency;
  double elapsed, mono_now, update_interval, orig_root_distance;
  struct timespec now, raw_now;
  int manual;

  assert(initialised);

  /* Special modes are implemented elsewhere */
  if (mode != REF_ModeNormal) {
    special_mode_sync(1, offset);
    return;
  }

  manual = leap == LEAP_Unsynchronised;

  mono_now = SCH_GetLastEventMonoTime();
  LCL_ReadRawTime(&raw_now);
  LCL_GetOffsetCorrection(&raw_now, &uncorrected_offset, NULL);
  UTI_AddDoubleToTimespec(&raw_now, uncorrected_offset, &now);

  elapsed = UTI_DiffTimespecsToDouble(&now, ref_time);
  offset += elapsed * frequency;

  if (last_ref_update != 0.0) {
    update_interval = mono_now - last_ref_update;
  } else {
    update_interval = 0.0;
  }

  /* Get new estimates of the frequency and skew including the new data */
  get_clock_estimates(manual, frequency, skew,
                      &frequency, &skew, &residual_frequency);

  if (!is_offset_ok(offset))
    return;

  orig_root_distance = our_root_delay / 2.0 + get_root_dispersion(&now);

  are_we_synchronised = leap != LEAP_Unsynchronised;
  our_stratum = stratum + 1;
  our_ref_id = ref_id;
  if (ref_ip)
    our_ref_ip = *ref_ip;
  else
    our_ref_ip.family = IPADDR_UNSPEC;
  our_ref_time = *ref_time;
  our_skew = skew;
  our_residual_freq = residual_frequency;
  our_root_delay = root_delay;
  our_root_dispersion = root_dispersion;
  our_frequency_sd = frequency_sd;
  our_offset_sd = offset_sd;
  last_ref_update = mono_now;
  last_ref_update_interval = update_interval;
  last_offset = offset;

  /* Check if the clock should be stepped */
  if (is_step_limit_reached(offset, uncorrected_offset)) {
    /* Cancel the uncorrected offset and correct the total offset by step */
    accumulate_offset = uncorrected_offset;
    step_offset = offset - uncorrected_offset;
  } else {
    accumulate_offset = offset;
    step_offset = 0.0;
  }

  /* Adjust the clock */
  LCL_AccumulateFrequencyAndOffset(frequency, accumulate_offset,
                                   get_correction_rate(offset_sd, update_interval));
    
  maybe_log_offset(offset, raw_now.tv_sec);

  if (step_offset != 0.0) {
    if (LCL_ApplyStepOffset(step_offset))
      LOG(LOGS_WARN, "System clock was stepped by %.6f seconds", -step_offset);
    else
      LCL_AccumulateOffset(step_offset, 0.0);
  }

  update_leap_status(leap, raw_now.tv_sec, 0);
  update_sync_status(&now);

  /* Add a random error of up to one second to the reference time to make it
     less useful when disclosed to NTP and cmdmon clients for estimating
     receive timestamps in the interleaved symmetric NTP mode */
  fuzz_ref_time(&our_ref_time);

  local_abs_frequency = LCL_ReadAbsoluteFrequency();

  write_log(&now, combined_sources, local_abs_frequency,
            offset, offset_sd, uncorrected_offset, orig_root_distance);

  if (drift_file) {
    /* Update drift file at most once per hour */
    drift_file_age += update_interval;
    if (drift_file_age >= drift_file_interval) {
      update_drift_file(local_abs_frequency, our_skew);
      drift_file_age = 0.0;
    }
  }

  /* Update fallback drifts */
  if (fb_drifts && are_we_synchronised) {
    update_fb_drifts(local_abs_frequency, update_interval);
    schedule_fb_drift();
  }

  /* Update the moving average of squares of offset, quickly on start */
  if (avg2_moving) {
    avg2_offset += 0.1 * (SQUARE(offset) - avg2_offset);
  } else {
    if (avg2_offset > 0.0 && avg2_offset < SQUARE(offset))
      avg2_moving = 1;
    avg2_offset = SQUARE(offset);
  }

  ref_adjustments = 0;
}

/* ================================================== */

int
REF_AdjustReference(double offset, double frequency)
{
  double adj_corr_rate, ref_corr_rate, mono_now;

  mono_now = SCH_GetLastEventMonoTime();
  ref_adjustments++;

  adj_corr_rate = get_correction_rate(fabs(offset), mono_now - last_ref_adjustment);
  ref_corr_rate = get_correction_rate(our_offset_sd, last_ref_update_interval) /
                  ref_adjustments;
  last_ref_adjustment = mono_now;

  return LCL_AccumulateFrequencyAndOffsetNoHandlers(frequency, offset,
                                                    MAX(adj_corr_rate, ref_corr_rate));
}

/* ================================================== */

void
REF_SetManualReference
(
 struct timespec *ref_time,
 double offset,
 double frequency,
 double skew
)
{
  /* We are not synchronised to an external source, as such.  This is
     only supposed to be used with the local source option, really.
     Log as MANU in the tracking log, packets will have NTP_REFID_LOCAL. */
  REF_SetReference(0, LEAP_Unsynchronised, 1, 0x4D414E55UL, NULL,
                   ref_time, offset, 0.0, frequency, skew, skew, 0.0, 0.0);
}

/* ================================================== */

void
REF_SetUnsynchronised(void)
{
  /* Variables required for logging to statistics log */
  struct timespec now, now_raw;
  double uncorrected_offset;

  assert(initialised);

  /* Special modes are implemented elsewhere */
  if (mode != REF_ModeNormal) {
    special_mode_sync(0, 0.0);
    return;
  }

  LCL_ReadRawTime(&now_raw);
  LCL_GetOffsetCorrection(&now_raw, &uncorrected_offset, NULL);
  UTI_AddDoubleToTimespec(&now_raw, uncorrected_offset, &now);

  if (fb_drifts) {
    schedule_fb_drift();
  }

  update_leap_status(LEAP_Unsynchronised, 0, 0);
  our_ref_ip.family = IPADDR_INET4;
  our_ref_ip.addr.in4 = 0;
  our_stratum = 0;

  if (are_we_synchronised)
    unsynchronised_since = SCH_GetLastEventMonoTime();
  are_we_synchronised = 0;

  LCL_SetSyncStatus(0, 0.0, 0.0);

  write_log(&now, 0, LCL_ReadAbsoluteFrequency(), 0.0, 0.0, uncorrected_offset,
            our_root_delay / 2.0 + get_root_dispersion(&now));
}

/* ================================================== */

void
REF_UpdateLeapStatus(NTP_Leap leap)
{
  struct timespec raw_now, now;

  /* Wait for a full reference update if not already synchronised */
  if (!are_we_synchronised)
    return;

  SCH_GetLastEventTime(&now, NULL, &raw_now);

  update_leap_status(leap, raw_now.tv_sec, 0);

  /* Update also the synchronisation status */
  update_sync_status(&now);
}

/* ================================================== */

void
REF_GetReferenceParams
(
 struct timespec *local_time,
 int *is_synchronised,
 NTP_Leap *leap_status,
 int *stratum,
 uint32_t *ref_id,
 struct timespec *ref_time,
 double *root_delay,
 double *root_dispersion
)
{
  double dispersion, delta, distance;
  int wait_local_ok;

  assert(initialised);

  if (are_we_synchronised) {
    dispersion = get_root_dispersion(local_time);
    wait_local_ok = UTI_DiffTimespecsToDouble(local_time, &our_ref_time) >= local_wait_synced;
  } else {
    dispersion = 0.0;
    wait_local_ok = SCH_GetLastEventMonoTime() - unsynchronised_since >= local_wait_unsynced;
  }

  distance = our_root_delay / 2 + dispersion;

  if (local_activate == 0.0 || (are_we_synchronised && distance < local_activate))
    local_activate_ok = 1;

  /* Local reference is active when enabled and the clock is not synchronised
     or the root distance exceeds the threshold */

  if (are_we_synchronised &&
      !(enable_local_stratum && local_activate_ok && wait_local_ok &&
        distance > local_distance)) {

    *is_synchronised = 1;

    *stratum = our_stratum;

    *leap_status = !leap_in_progress ? our_leap_status : LEAP_Unsynchronised;
    *ref_id = our_ref_id;
    *ref_time = our_ref_time;
    *root_delay = our_root_delay;
    *root_dispersion = dispersion;

  } else if (enable_local_stratum && local_activate_ok && wait_local_ok) {

    *is_synchronised = 0;

    *stratum = local_stratum;
    *ref_id = NTP_REFID_LOCAL;

    /* Keep the reference timestamp up to date.  Adjust the timestamp to make
       sure that the transmit timestamp cannot come before this (which might
       fail a test of an NTP client). */
    delta = UTI_DiffTimespecsToDouble(local_time, &local_ref_time);
    if (delta > LOCAL_REF_UPDATE_INTERVAL || delta < 1.0) {
      UTI_AddDoubleToTimespec(local_time, -1.0, &local_ref_time);
      fuzz_ref_time(&local_ref_time);
    }

    *ref_time = local_ref_time;

    /* Not much else we can do for leap second bits - maybe need to
       have a way for the administrator to feed leap bits in */
    *leap_status = LEAP_Normal;
    
    *root_delay = 0.0;
    *root_dispersion = 0.0;
    
  } else {

    *is_synchronised = 0;

    *leap_status = LEAP_Unsynchronised;
    *stratum = NTP_MAX_STRATUM;
    *ref_id = NTP_REFID_UNSYNC;
    UTI_ZeroTimespec(ref_time);
    /* These values seem to be standard for a client, and
       any peer or client of ours will ignore them anyway because
       we don't claim to be synchronised */
    *root_dispersion = 1.0;
    *root_delay = 1.0;

  }
}

/* ================================================== */

int
REF_GetOurStratum(void)
{
  struct timespec now_cooked, ref_time;
  int synchronised, stratum;
  NTP_Leap leap_status;
  uint32_t ref_id;
  double root_delay, root_dispersion;

  SCH_GetLastEventTime(&now_cooked, NULL, NULL);
  REF_GetReferenceParams(&now_cooked, &synchronised, &leap_status, &stratum,
                         &ref_id, &ref_time, &root_delay, &root_dispersion);

  return stratum;
}

/* ================================================== */

int
REF_GetOrphanStratum(void)
{
  if (!enable_local_stratum || !local_orphan || mode != REF_ModeNormal)
    return NTP_MAX_STRATUM;
  return local_stratum;
}

/* ================================================== */

double
REF_GetSkew(void)
{
  return our_skew;
}

/* ================================================== */

void
REF_ModifyMaxupdateskew(double new_max_update_skew)
{
  max_update_skew = new_max_update_skew * 1.0e-6;
  LOG(LOGS_INFO, "New maxupdateskew %f ppm", new_max_update_skew);
}

/* ================================================== */

void
REF_ModifyMakestep(int limit, double threshold)
{
  make_step_limit = limit;
  make_step_threshold = threshold;
  LOG(LOGS_INFO, "New makestep %f %d", threshold, limit);
}

/* ================================================== */

void
REF_EnableLocal(int stratum, double distance, int orphan, double activate,
                double wait_synced, double wait_unsynced)
{
  enable_local_stratum = 1;
  local_stratum = CLAMP(1, stratum, NTP_MAX_STRATUM - 1);
  local_distance = distance;
  local_orphan = !!orphan;
  local_activate = activate;
  local_wait_synced = wait_synced;
  local_wait_unsynced = wait_unsynced;
  LOG(LOGS_INFO, "%s local reference mode", "Enabled");
}

/* ================================================== */

void
REF_DisableLocal(void)
{
  enable_local_stratum = 0;
  LOG(LOGS_INFO, "%s local reference mode", "Disabled");
}

/* ================================================== */

#define LEAP_SECOND_CLOSE 5

static int
is_leap_close(double t)
{
  return leap_when != 0 &&
         t >= leap_when - LEAP_SECOND_CLOSE && t < leap_when + LEAP_SECOND_CLOSE;
}

/* ================================================== */

int REF_IsLeapSecondClose(struct timespec *ts, double offset)
{
  struct timespec now, now_raw;

  SCH_GetLastEventTime(&now, NULL, &now_raw);

  if (is_leap_close(now.tv_sec) || is_leap_close(now_raw.tv_sec))
    return 1;

  if (ts && (is_leap_close(ts->tv_sec) || is_leap_close(ts->tv_sec + offset)))
    return 1;

  return 0;
}

/* ================================================== */

int
REF_GetTaiOffset(struct timespec *ts)
{
  int tai_offset;

  LDB_GetLeap(ts->tv_sec, &tai_offset);

  return tai_offset;
}

/* ================================================== */

void
REF_GetTrackingReport(RPT_TrackingReport *rep)
{
  struct timespec now_raw, now_cooked;
  double correction;
  int synchronised;

  LCL_ReadRawTime(&now_raw);
  LCL_GetOffsetCorrection(&now_raw, &correction, NULL);
  UTI_AddDoubleToTimespec(&now_raw, correction, &now_cooked);

  REF_GetReferenceParams(&now_cooked, &synchronised,
                         &rep->leap_status, &rep->stratum,
                         &rep->ref_id, &rep->ref_time,
                         &rep->root_delay, &rep->root_dispersion);

  if (rep->stratum == NTP_MAX_STRATUM && !synchronised)
    rep->stratum = 0;

  rep->ip_addr.family = IPADDR_UNSPEC;
  rep->current_correction = correction;
  rep->freq_ppm = LCL_ReadAbsoluteFrequency();
  rep->resid_freq_ppm = 0.0;
  rep->skew_ppm = 0.0;
  rep->last_update_interval = last_ref_update_interval;
  rep->last_offset = last_offset;
  rep->rms_offset = sqrt(avg2_offset);

  if (synchronised) {
    rep->ip_addr = our_ref_ip;
    rep->resid_freq_ppm = 1.0e6 * our_residual_freq;
    rep->skew_ppm = 1.0e6 * our_skew;
  }
}
