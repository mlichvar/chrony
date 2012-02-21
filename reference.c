/*
  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Richard P. Curnow  1997-2003
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

  This module keeps track of the source which we are claiming to be
  our reference, for the purposes of generating outgoing NTP packets */

#include "config.h"

#include "sysincl.h"

#include "memory.h"
#include "reference.h"
#include "util.h"
#include "conf.h"
#include "logging.h"
#include "local.h"
#include "sched.h"

/* ================================================== */

static int are_we_synchronised;
static int enable_local_stratum;
static int local_stratum;
static NTP_Leap our_leap_status;
static int our_leap_sec;
static int our_stratum;
static uint32_t our_ref_id;
static IPAddr our_ref_ip;
struct timeval our_ref_time; /* Stored relative to reference, NOT local time */
static double our_skew;
static double our_residual_freq;
static double our_root_delay;
static double our_root_dispersion;

static double max_update_skew;

static double last_offset;
static double avg2_offset;
static int avg2_moving;

static double correction_time_ratio;

/* Flag indicating that we are initialised */
static int initialised = 0;

/* Threshold and update limit for stepping clock */
static int make_step_limit;
static double make_step_threshold;

/* Number of updates before offset checking, number of ignored updates
   before exiting and the maximum allowed offset */
static int max_offset_delay;
static int max_offset_ignore;
static double max_offset;

/* Flag and threshold for logging clock changes to syslog */
static int do_log_change;
static double log_change_threshold;

/* Flag, threshold and user for sending mail notification on large clock changes */
static int do_mail_change;
static double mail_change_threshold;
static char *mail_change_user;

/* Filename of the drift file. */
static char *drift_file=NULL;
static double drift_file_age;

static void update_drift_file(double, double);

/* ================================================== */

static LOG_FileID logfileid;

/* ================================================== */

/* Reference ID supplied when we are locally referenced */
#define LOCAL_REFERENCE_ID 0x7f7f0101UL

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

/* Timestamp of last reference update */
static struct timeval last_ref_update;
static double last_ref_update_interval;

/* ================================================== */

static void
handle_slew(struct timeval *raw,
            struct timeval *cooked,
            double dfreq,
            double doffset,
            int is_step_change,
            void *anything)
{
  if (is_step_change) {
    UTI_AddDoubleToTimeval(&last_ref_update, -doffset, &last_ref_update);
  }
}

/* ================================================== */

void
REF_Initialise(void)
{
  FILE *in;
  char line[1024];
  double file_freq_ppm, file_skew_ppm;
  double our_frequency_ppm;

  are_we_synchronised = 0;
  our_leap_status = LEAP_Unsynchronised;
  our_leap_sec = 0;
  initialised = 1;
  our_root_dispersion = 1.0;
  our_root_delay = 1.0;
  our_frequency_ppm = 0.0;
  our_skew = 1.0; /* i.e. rather bad */
  our_residual_freq = 0.0;
  drift_file_age = 0.0;

  /* Now see if we can get the drift file opened */
  drift_file = CNF_GetDriftFile();
  if (drift_file) {
    in = fopen(drift_file, "r");
    if (in) {
      if (fgets(line, sizeof(line), in)) {
        if (sscanf(line, "%lf%lf", &file_freq_ppm, &file_skew_ppm) == 2) {
          /* We have read valid data */
          our_frequency_ppm = file_freq_ppm;
          our_skew = 1.0e-6 * file_skew_ppm;
          LOG(LOGS_INFO, LOGF_Reference, "Frequency %.3f +/- %.3f ppm read from %s", file_freq_ppm, file_skew_ppm, drift_file);
          LCL_SetAbsoluteFrequency(our_frequency_ppm);
        } else {
          LOG(LOGS_WARN, LOGF_Reference, "Could not parse valid frequency and skew from driftfile %s",
              drift_file);
        }
      } else {
        LOG(LOGS_WARN, LOGF_Reference, "Could not read valid frequency and skew from driftfile %s",
            drift_file);
      }
      fclose(in);
    }
  }
    
  if (our_frequency_ppm == 0.0) {
    our_frequency_ppm = LCL_ReadAbsoluteFrequency();
    if (our_frequency_ppm != 0.0) {
      LOG(LOGS_INFO, LOGF_Reference, "Initial frequency %.3f ppm", our_frequency_ppm);
    }
  }

  logfileid = CNF_GetLogTracking() ? LOG_FileOpen("tracking",
      "   Date (UTC) Time     IP Address   St   Freq ppm   Skew ppm     Offset")
    : -1;

  max_update_skew = fabs(CNF_GetMaxUpdateSkew()) * 1.0e-6;

  correction_time_ratio = CNF_GetCorrectionTimeRatio();

  enable_local_stratum = CNF_AllowLocalReference(&local_stratum);

  CNF_GetMakeStep(&make_step_limit, &make_step_threshold);
  CNF_GetMaxChange(&max_offset_delay, &max_offset_ignore, &max_offset);
  CNF_GetLogChange(&do_log_change, &log_change_threshold);
  CNF_GetMailOnChange(&do_mail_change, &mail_change_threshold, &mail_change_user);

  CNF_GetFallbackDrifts(&fb_drift_min, &fb_drift_max);

  if (fb_drift_max >= fb_drift_min && fb_drift_min > 0) {
    fb_drifts = MallocArray(struct fb_drift, fb_drift_max - fb_drift_min + 1);
    memset(fb_drifts, 0, sizeof (struct fb_drift) * (fb_drift_max - fb_drift_min + 1));
    next_fb_drift = 0;
    fb_drift_timeout_id = -1;
  }

  last_ref_update.tv_sec = 0;
  last_ref_update.tv_usec = 0;
  last_ref_update_interval = 0.0;

  LCL_AddParameterChangeHandler(handle_slew, NULL);

  /* And just to prevent anything wierd ... */
  if (do_log_change) {
    log_change_threshold = fabs(log_change_threshold);
  }

  /* Make first entry in tracking log */
  REF_SetUnsynchronised();

  return;
}

/* ================================================== */

void
REF_Finalise(void)
{
  if (our_leap_sec) {
    LCL_SetLeap(0);
  }

  if (drift_file && drift_file_age > 0.0) {
    update_drift_file(LCL_ReadAbsoluteFrequency(), our_skew);
  }

  Free(fb_drifts);

  initialised = 0;
  return;
}

/* ================================================== */

static double
Sqr(double x)
{
  return x*x;
}

/* ================================================== */
#if 0
static double
Cube(double x)
{
  return x*x*x;
}
#endif

/* ================================================== */
/* Update the drift coefficients to the file. */

static void
update_drift_file(double freq_ppm, double skew)
{
  struct stat buf;
  char *temp_drift_file;
  FILE *out;

  /* Create a temporary file with a '.tmp' extension. */

  temp_drift_file = (char*) Malloc(strlen(drift_file)+8);

  if(!temp_drift_file) {
    return;
  }

  strcpy(temp_drift_file,drift_file);
  strcat(temp_drift_file,".tmp");

  out = fopen(temp_drift_file, "w");
  if (!out) {
    Free(temp_drift_file);
    LOG(LOGS_WARN, LOGF_Reference, "Could not open temporary driftfile %s.tmp for writing",
        drift_file);
    return;
  }

  /* Write the frequency and skew parameters in ppm */
  fprintf(out, "%20.4f %20.4f\n", freq_ppm, 1.0e6 * skew);

  fclose(out);

  /* Clone the file attributes from the existing file if there is one. */

  if (!stat(drift_file,&buf)) {
    if (chown(temp_drift_file,buf.st_uid,buf.st_gid)) {
      LOG(LOGS_WARN, LOGF_Reference, "Could not change ownership of temporary driftfile %s.tmp", drift_file);
    }
    chmod(temp_drift_file,buf.st_mode&0777);
  }

  /* Rename the temporary file to the correct location (see rename(2) for details). */

  if (rename(temp_drift_file,drift_file)) {
    unlink(temp_drift_file);
    Free(temp_drift_file);
    LOG(LOGS_WARN, LOGF_Reference, "Could not replace old driftfile %s with new one %s.tmp (%d)",
        drift_file,drift_file);
    return;
  }

  Free(temp_drift_file);
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

  if (fb_drift_timeout_id != -1) {
    SCH_RemoveTimeout(fb_drift_timeout_id);
    fb_drift_timeout_id = -1;
  }

  if (update_interval < 0.0 || update_interval > last_ref_update_interval * 4.0)
    return;

  for (i = 0; i < fb_drift_max - fb_drift_min + 1; i++) {
    /* Don't allow differences larger than 10 ppm */
    if (fabs(freq_ppm - fb_drifts[i].freq) > 10.0)
      fb_drifts[i].secs = 0.0;

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

#if 0
    LOG(LOGS_INFO, LOGF_Reference, "Fallback drift %d updated: %f ppm %f seconds",
        i + fb_drift_min, fb_drifts[i].freq, fb_drifts[i].secs);
#endif
  }
}

/* ================================================== */

static void
fb_drift_timeout(void *arg)
{
  assert(are_we_synchronised == 0);
  assert(next_fb_drift >= fb_drift_min && next_fb_drift <= fb_drift_max);

  fb_drift_timeout_id = -1;

  LCL_SetAbsoluteFrequency(fb_drifts[next_fb_drift - fb_drift_min].freq);
  REF_SetUnsynchronised();
}

/* ================================================== */

static void
schedule_fb_drift(struct timeval *now)
{
  int i, c, secs;
  double unsynchronised;
  struct timeval when;

  if (fb_drift_timeout_id != -1)
    return; /* already scheduled */

  UTI_DiffTimevalsToDouble(&unsynchronised, now, &last_ref_update);

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
#if 0
    LOG(LOGS_INFO, LOGF_Reference, "Fallback drift %d set", c);
#endif
  }

  if (i <= fb_drift_max) {
    next_fb_drift = i;
    UTI_AddDoubleToTimeval(now, secs - unsynchronised, &when);
    fb_drift_timeout_id = SCH_AddTimeout(&when, fb_drift_timeout, NULL);
#if 0
    LOG(LOGS_INFO, LOGF_Reference, "Fallback drift %d scheduled", i);
#endif
  }
}

/* ================================================== */

#define BUFLEN 255
#define S_MAX_USER_LEN "128"

static void
maybe_log_offset(double offset)
{
  double abs_offset;
  FILE *p;
  char buffer[BUFLEN], host[BUFLEN];
  time_t now;
  struct tm stm;

  abs_offset = fabs(offset);

  if (do_log_change &&
      (abs_offset > log_change_threshold)) {
    LOG(LOGS_WARN, LOGF_Reference,
        "System clock wrong by %.6f seconds, adjustment started",
        -offset);
  }

  if (do_mail_change &&
      (abs_offset > mail_change_threshold)) {
    snprintf(buffer, sizeof(buffer), "%s %." S_MAX_USER_LEN "s", MAIL_PROGRAM, mail_change_user);
    p = popen(buffer, "w");
    if (p) {
      if (gethostname(host, sizeof(host)) < 0) {
        strcpy(host, "<UNKNOWN>");
      }
      fprintf(p, "Subject: chronyd reports change to system clock on node [%s]\n", host);
      fputs("\n", p);
      now = time(NULL);
      stm = *localtime(&now);
      strftime(buffer, sizeof(buffer), "On %A, %d %B %Y\n  with the system clock reading %H:%M:%S (%Z)", &stm);
      fputs(buffer, p);
      /* If offset < 0 the local clock is slow, so we are applying a
         positive change to it to bring it into line, hence the
         negation of 'offset' in the next statement (and earlier) */
      fprintf(p,
              "\n\nchronyd started to apply an adjustment of %.3f seconds to it,\n"
              "  which exceeded the reporting threshold of %.3f seconds\n\n",
              -offset, mail_change_threshold);
      pclose(p);
    } else {
      LOG(LOGS_ERR, LOGF_Reference,
          "Could not send mail notification to user %s\n",
          mail_change_user);
    }
  }

}

/* ================================================== */

static void
maybe_make_step()
{
  if (make_step_limit == 0) {
    return;
  } else if (make_step_limit > 0) {
    make_step_limit--;
  }
  LCL_MakeStep(make_step_threshold);
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

  offset = fabs(offset);
  if (offset > max_offset) {
    LOG(LOGS_WARN, LOGF_Reference,
        "Adjustment of %.3f seconds exceeds the allowed maximum of %.3f seconds (%s) ",
        offset, max_offset, !max_offset_ignore ? "exiting" : "ignored");
    if (!max_offset_ignore)
      SCH_QuitProgram();
    else if (max_offset_ignore > 0)
      max_offset_ignore--;
    return 0;
  }
  return 1;
}

/* ================================================== */

static void
update_leap_status(NTP_Leap leap)
{
  time_t now;
  struct tm stm;
  int leap_sec;

  leap_sec = 0;

  if (leap == LEAP_InsertSecond || leap == LEAP_DeleteSecond) {
    /* Insert/delete leap second only on June 30 or December 31
       and in other months ignore the leap status completely */

    now = time(NULL);
    stm = *gmtime(&now);

    if (stm.tm_mon != 5 && stm.tm_mon != 11) {
      leap = LEAP_Normal;
    } else if ((stm.tm_mon == 5 && stm.tm_mday == 30) ||
        (stm.tm_mon == 11 && stm.tm_mday == 31)) {
      if (leap == LEAP_InsertSecond) {
        leap_sec = 1;
      } else {
        leap_sec = -1;
      }
    }
  }
  
  if (leap_sec != our_leap_sec) {
    LCL_SetLeap(leap_sec);
    our_leap_sec = leap_sec;
  }

  our_leap_status = leap;
}

/* ================================================== */

static void
write_log(struct timeval *ref_time, char *ref, int stratum, double freq, double skew, double offset)
{
  if (logfileid != -1) {
    LOG_FileWrite(logfileid, "%s %-15s %2d %10.3f %10.3f %10.3e",
            UTI_TimeToLogForm(ref_time->tv_sec), ref, stratum, freq, skew, offset);
  }
}

/* ================================================== */

void
REF_SetReference(int stratum,
                 NTP_Leap leap,
                 uint32_t ref_id,
                 IPAddr *ref_ip,
                 struct timeval *ref_time,
                 double offset,
                 double offset_sd,
                 double frequency,
                 double skew,
                 double root_delay,
                 double root_dispersion
                 )
{
  double previous_skew, new_skew;
  double previous_freq, new_freq;
  double old_weight, new_weight, sum_weight;
  double delta_freq1, delta_freq2;
  double skew1, skew2;
  double our_offset;
  double our_frequency;
  double abs_freq_ppm;
  double update_interval;
  double elapsed;
  double correction_rate;
  struct timeval now;

  assert(initialised);

  /* Avoid getting NaNs */
  if (skew < 1e-12)
    skew = 1e-12;
  if (our_skew < 1e-12)
    our_skew = 1e-12;

  /* If we get a serious rounding error in the source stats regression
     processing, there is a remote chance that the skew argument is a
     'not a number'.  If such a quantity gets propagated into the
     machine's kernel clock variables, nasty things will happen ..
     
     To guard against this we need to check whether the skew argument
     is a reasonable real number.  I don't think isnan, isinf etc are
     platform independent, so the following algorithm is used. */

  {
    double t;
    t = (skew + skew) / skew; /* Skew shouldn't be zero either */
    if ((t < 1.9) || (t > 2.1)) {
      LOG(LOGS_WARN, LOGF_Reference, "Bogus skew value encountered");
      return;
    }
  }
    
  LCL_ReadCookedTime(&now, NULL);
  UTI_DiffTimevalsToDouble(&elapsed, &now, ref_time);
  our_offset = offset + elapsed * frequency;

  if (!is_offset_ok(offset))
    return;

  are_we_synchronised = 1;
  our_stratum = stratum + 1;
  our_ref_id = ref_id;
  if (ref_ip)
    our_ref_ip = *ref_ip;
  else
    our_ref_ip.family = IPADDR_UNSPEC;
  our_ref_time = *ref_time;
  our_root_delay = root_delay;
  our_root_dispersion = root_dispersion;

  update_leap_status(leap);

  if (last_ref_update.tv_sec) {
    UTI_DiffTimevalsToDouble(&update_interval, &now, &last_ref_update);
    if (update_interval < 0.0)
      update_interval = 0.0;
  } else {
    update_interval = 0.0;
  }
  last_ref_update = now;

  /* We want to correct the offset quickly, but we also want to keep the
     frequency error caused by the correction itself low.

     Define correction rate as the area of the region bounded by the graph of
     offset corrected in time. Set the rate so that the time needed to correct
     an offset equal to the current sourcestats stddev will be equal to the
     update interval multiplied by the correction time ratio (assuming linear
     adjustment). The offset and the time needed to make the correction are
     inversely proportional.

     This is only a suggestion and it's up to the system driver how the
     adjustment will be executed. */

  correction_rate = correction_time_ratio * 0.5 * offset_sd * update_interval;

  /* Eliminate updates that are based on totally unreliable frequency
     information */

  if (fabs(skew) < max_update_skew) { 

    previous_skew = our_skew;
    new_skew = skew;

    previous_freq = 0.0; /* We assume that the local clock is running
                          according to our previously determined
                          value; note that this is a delta frequency
                          --- absolute frequencies are only known in
                          the local module. */
    new_freq = frequency;

    /* Set new frequency based on weighted average of old and new skew. */

    old_weight = 1.0 / Sqr(previous_skew);
    new_weight = 3.0 / Sqr(new_skew);

    sum_weight = old_weight + new_weight;

    our_frequency = (previous_freq * old_weight + new_freq * new_weight) / sum_weight;

    delta_freq1 = previous_freq - our_frequency;
    delta_freq2 = new_freq - our_frequency;

    skew1 = sqrt((Sqr(delta_freq1) * old_weight + Sqr(delta_freq2) * new_weight) / sum_weight);
    skew2 = (previous_skew * old_weight + new_skew * new_weight) / sum_weight;
    our_skew = skew1 + skew2;

    our_residual_freq = new_freq - our_frequency;

    maybe_log_offset(our_offset);
    LCL_AccumulateFrequencyAndOffset(our_frequency, our_offset, correction_rate);
    
  } else {

#if 0    
    LOG(LOGS_INFO, LOGF_Reference, "Skew %f too large to track, offset=%f", skew, our_offset);
#endif
    maybe_log_offset(our_offset);
    LCL_AccumulateOffset(our_offset, correction_rate);

    our_residual_freq = frequency;
  }

  maybe_make_step();

  abs_freq_ppm = LCL_ReadAbsoluteFrequency();

  write_log(&now,
            our_ref_ip.family != IPADDR_UNSPEC ? UTI_IPToString(&our_ref_ip) : UTI_RefidToString(our_ref_id),
            our_stratum,
            abs_freq_ppm,
            1.0e6*our_skew,
            our_offset);

  if (drift_file) {
    /* Update drift file at most once per hour */
    drift_file_age += update_interval;
    if (drift_file_age < 0.0 || drift_file_age > 3600.0) {
      update_drift_file(abs_freq_ppm, our_skew);
      drift_file_age = 0.0;
    }
  }

  /* Update fallback drifts */
  if (fb_drifts) {
    update_fb_drifts(abs_freq_ppm, update_interval);
  }

  last_ref_update_interval = update_interval;
  last_offset = our_offset;

  /* Update the moving average of squares of offset, quickly on start */
  if (avg2_moving) {
    avg2_offset += 0.1 * (our_offset * our_offset - avg2_offset);
  } else {
    if (avg2_offset > 0.0 && avg2_offset < our_offset * our_offset)
      avg2_moving = 1;
    avg2_offset = our_offset * our_offset;
  }

  /* And now set the freq and offset to zero */
  our_frequency = 0.0;
  our_offset = 0.0;
  
  return;
}

/* ================================================== */

void
REF_SetManualReference
(
 struct timeval *ref_time,
 double offset,
 double frequency,
 double skew
)
{
  double abs_freq_ppm;

  /* We are not synchronised to an external source, as such.  This is
   only supposed to be used with the local source option, really
   ... */
  are_we_synchronised = 0;

  our_skew = skew;
  our_residual_freq = 0.0;

  maybe_log_offset(offset);
  LCL_AccumulateFrequencyAndOffset(frequency, offset, 0.0);
  maybe_make_step();

  abs_freq_ppm = LCL_ReadAbsoluteFrequency();

  write_log(ref_time,
            "127.127.1.1",
            our_stratum,
            abs_freq_ppm,
            1.0e6*our_skew,
            offset);

  if (drift_file) {
    update_drift_file(abs_freq_ppm, our_skew);
  }
}

/* ================================================== */

void
REF_SetUnsynchronised(void)
{
  /* Variables required for logging to statistics log */
  struct timeval now;

  assert(initialised);

  LCL_ReadCookedTime(&now, NULL);

  if (fb_drifts) {
    schedule_fb_drift(&now);
  }

  write_log(&now,
            "0.0.0.0",
            0,
            LCL_ReadAbsoluteFrequency(),
            1.0e6*our_skew,
            0.0);

  are_we_synchronised = 0;

  update_leap_status(LEAP_Unsynchronised);
}

/* ================================================== */

void
REF_GetReferenceParams
(
 struct timeval *local_time,
 int *is_synchronised,
 NTP_Leap *leap_status,
 int *stratum,
 uint32_t *ref_id,
 struct timeval *ref_time,
 double *root_delay,
 double *root_dispersion
)
{
  double elapsed;
  double extra_dispersion;

  assert(initialised);

  if (are_we_synchronised) {

    *is_synchronised = 1;

    *stratum = our_stratum;

    UTI_DiffTimevalsToDouble(&elapsed, local_time, &our_ref_time);
    extra_dispersion = (our_skew + fabs(our_residual_freq) + LCL_GetMaxClockError()) * elapsed;

    *leap_status = our_leap_status;
    *ref_id = our_ref_id;
    *ref_time = our_ref_time;
    *root_delay = our_root_delay;
    *root_dispersion = our_root_dispersion + extra_dispersion;

  } else if (enable_local_stratum) {

    *is_synchronised = 1;

    *stratum = local_stratum;
    *ref_id = LOCAL_REFERENCE_ID;
    /* Make the reference time be now less a second - this will
       scarcely affect the client, but will ensure that the transmit
       timestamp cannot come before this (which would cause test 6 to
       fail in the client's read routine) if the local system clock's
       read routine is broken in any way. */
    *ref_time = *local_time;
    --ref_time->tv_sec;

    /* Not much else we can do for leap second bits - maybe need to
       have a way for the administrator to feed leap bits in */
    *leap_status = LEAP_Normal;
    
    *root_delay = 0.0;
    *root_dispersion = LCL_GetSysPrecisionAsQuantum();
    
  } else {

    *is_synchronised = 0;

    *leap_status = LEAP_Unsynchronised;
    *stratum = 0;
    *ref_id = 0;
    ref_time->tv_sec = ref_time->tv_usec = 0;
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
  if (are_we_synchronised) {
    return our_stratum;
  } else if (enable_local_stratum) {
    return local_stratum;
  } else {
    return 16;
  }
}

/* ================================================== */

void
REF_ModifyMaxupdateskew(double new_max_update_skew)
{
  max_update_skew = new_max_update_skew * 1.0e-6;
#if 0
  LOG(LOGS_INFO, LOGF_Reference, "New max update skew = %.3fppm", new_max_update_skew);
#endif
}

/* ================================================== */

void
REF_EnableLocal(int stratum)
{
  enable_local_stratum = 1;
  local_stratum = stratum;
}

/* ================================================== */

void
REF_DisableLocal(void)
{
  enable_local_stratum = 0;
}

/* ================================================== */

int
REF_IsLocalActive(void)
{
  return !are_we_synchronised && enable_local_stratum;
}

/* ================================================== */

void
REF_GetTrackingReport(RPT_TrackingReport *rep)
{
  double elapsed;
  double extra_dispersion;
  struct timeval now_raw, now_cooked;
  double correction;

  LCL_ReadRawTime(&now_raw);
  LCL_GetOffsetCorrection(&now_raw, &correction, NULL);
  UTI_AddDoubleToTimeval(&now_raw, correction, &now_cooked);

  rep->ref_id = 0;
  rep->ip_addr.family = IPADDR_UNSPEC;
  rep->stratum = 0;
  rep->ref_time.tv_sec = 0;
  rep->ref_time.tv_usec = 0;
  rep->current_correction = correction;
  rep->freq_ppm = LCL_ReadAbsoluteFrequency();
  rep->resid_freq_ppm = 0.0;
  rep->skew_ppm = 0.0;
  rep->root_delay = 0.0;
  rep->root_dispersion = 0.0;
  rep->last_update_interval = last_ref_update_interval;
  rep->last_offset = last_offset;
  rep->rms_offset = sqrt(avg2_offset);

  if (are_we_synchronised) {
    
    UTI_DiffTimevalsToDouble(&elapsed, &now_cooked, &our_ref_time);
    extra_dispersion = (our_skew + fabs(our_residual_freq) + LCL_GetMaxClockError()) * elapsed;
    
    rep->ref_id = our_ref_id;
    rep->ip_addr = our_ref_ip;
    rep->stratum = our_stratum;
    rep->ref_time = our_ref_time;
    rep->resid_freq_ppm = 1.0e6 * our_residual_freq;
    rep->skew_ppm = 1.0e6 * our_skew;
    rep->root_delay = our_root_delay;
    rep->root_dispersion = our_root_dispersion + extra_dispersion;

  } else if (enable_local_stratum) {

    rep->ref_id = LOCAL_REFERENCE_ID;
    rep->ip_addr.family = IPADDR_UNSPEC;
    rep->stratum = local_stratum;
    rep->ref_time = now_cooked;
    rep->root_dispersion = LCL_GetSysPrecisionAsQuantum();
  }

}

/* ================================================== */
