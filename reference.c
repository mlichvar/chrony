/*
  $Header: /cvs/src/chrony/reference.c,v 1.40 2003/03/24 23:35:43 richard Exp $

  =======================================================================

  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Richard P. Curnow  1997-2002
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

  This module keeps track of the source which we are claiming to be
  our reference, for the purposes of generating outgoing NTP packets */

#include "sysincl.h"

#include "memory.h"
#include "reference.h"
#include "util.h"
#include "conf.h"
#include "logging.h"
#include "local.h"
#include "mkdirpp.h"

/* ================================================== */

static int are_we_synchronised;
static int enable_local_stratum;
static int local_stratum;
static NTP_Leap our_leap_status;
static int our_stratum;
static unsigned long our_ref_id;
struct timeval our_ref_time; /* Stored relative to reference, NOT local time */
static double our_offset;
static double our_skew;
static double our_residual_freq;
static double our_root_delay;
static double our_root_dispersion;

static double max_update_skew;

/* Flag indicating that we are initialised */
static int initialised = 0;

/* Flag and threshold for logging clock changes to syslog */
static int do_log_change;
static double log_change_threshold;

/* Flag, threshold and user for sending mail notification on large clock changes */
static int do_mail_change;
static double mail_change_threshold;
static char *mail_change_user;

/* Filename of the drift file. */
static char *drift_file=NULL;

static void update_drift_file(double, double);

#define MAIL_PROGRAM "/usr/lib/sendmail"

/* ================================================== */
/* File to which statistics are logged, NULL if none */
static FILE *logfile = NULL;
static char *logfilename = NULL;
static unsigned long logwrites = 0;

#define TRACKING_LOG "tracking.log"

/* ================================================== */

/* Day number of 1 Jan 1970 */
#define MJD_1970 40587

/* Reference ID supplied when we are locally referenced */
#define LOCAL_REFERENCE_ID 0x7f7f0101UL

/* ================================================== */

void
REF_Initialise(void)
{
  char *direc;
  FILE *in;
  char line[1024];
  double file_freq_ppm, file_skew_ppm;
  double our_frequency_ppm;

  are_we_synchronised = 0;
  our_leap_status = LEAP_Normal;
  initialised = 1;
  our_root_dispersion = 1.0;
  our_root_delay = 1.0;
  our_frequency_ppm = 0.0;
  our_skew = 1.0; /* i.e. rather bad */
  our_residual_freq = 0.0;

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
        } else {
          LOG(LOGS_WARN, LOGF_Reference, "Could not parse valid frequency and skew from driftfile %s",
              drift_file);
        }
      } else {
        LOG(LOGS_WARN, LOGF_Reference, "Could not read valid frequency and skew from driftfile %s",
            drift_file);
      }
      fclose(in);
    } else {
      LOG(LOGS_WARN, LOGF_Reference, "Could not open driftfile %s for reading",
          drift_file);
    }

    update_drift_file(our_frequency_ppm,our_skew);
  }
    
  LCL_SetAbsoluteFrequency(our_frequency_ppm);

  if (CNF_GetLogTracking()) {
    direc = CNF_GetLogDir();
    if (!mkdir_and_parents(direc)) {
      LOG(LOGS_ERR, LOGF_Reference, "Could not create directory %s", direc);
      logfile = NULL;
    } else {
      logfilename = MallocArray(char, 2 + strlen(direc) + strlen(TRACKING_LOG));
      strcpy(logfilename, direc);
      strcat(logfilename, "/");
      strcat(logfilename, TRACKING_LOG);
      logfile = fopen(logfilename, "a");
      if (!logfile) {
        LOG(LOGS_WARN, LOGF_Reference, "Couldn't open logfile %s for update", logfilename);
      }
    }
  }

  max_update_skew = fabs(CNF_GetMaxUpdateSkew()) * 1.0e-6;

  enable_local_stratum = CNF_AllowLocalReference(&local_stratum);

  CNF_GetLogChange(&do_log_change, &log_change_threshold);
  CNF_GetMailOnChange(&do_mail_change, &mail_change_threshold, &mail_change_user);

  /* And just to prevent anything wierd ... */
  if (do_log_change) {
    log_change_threshold = fabs(log_change_threshold);
  }

  return;
}

/* ================================================== */

void
REF_Finalise(void)
{
  if (logfile) {
    fclose(logfile);
  }

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
    chown(temp_drift_file,buf.st_uid,buf.st_gid);
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
    sprintf(buffer, "%s %." S_MAX_USER_LEN "s", MAIL_PROGRAM, mail_change_user);
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


void
REF_SetReference(int stratum,
                 NTP_Leap leap,
                 unsigned long ref_id,
                 struct timeval *ref_time,
                 double offset,
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
  double our_frequency;

  double abs_freq_ppm;

  assert(initialised);

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
    

  are_we_synchronised = 1;
  our_stratum = stratum + 1;
  our_leap_status = leap;
  our_ref_id = ref_id;
  our_ref_time = *ref_time;
  our_offset = offset;
  our_root_delay = root_delay;
  our_root_dispersion = root_dispersion;

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
    LCL_AccumulateFrequencyAndOffset(our_frequency, our_offset);
    
  } else {

#if 0    
    LOG(LOGS_INFO, LOGF_Reference, "Skew %f too large to track, offset=%f", skew, our_offset);
#endif
    maybe_log_offset(our_offset);
    LCL_AccumulateOffset(our_offset);

    our_residual_freq = frequency;
  }

  abs_freq_ppm = LCL_ReadAbsoluteFrequency();

  if (logfile) {

    if (((logwrites++) % 32) == 0) {
      fprintf(logfile,
              "=======================================================================\n"
              "   Date (UTC) Time     IP Address   St   Freq ppm   Skew ppm     Offset\n"
              "=======================================================================\n");
    }
          
    fprintf(logfile, "%s %-15s %2d %10.3f %10.3f %10.3e\n",
            UTI_TimeToLogForm(ref_time->tv_sec),
            UTI_IPToDottedQuad(our_ref_id),
            our_stratum,
            abs_freq_ppm,
            1.0e6*our_skew,
            our_offset);
    
    fflush(logfile);
  }

  if (drift_file) {
    update_drift_file(abs_freq_ppm, our_skew);
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
  int millisecond;
  double abs_freq_ppm;

  /* We are not synchronised to an external source, as such.  This is
   only supposed to be used with the local source option, really
   ... */
  are_we_synchronised = 0;

  our_skew = skew;
  our_residual_freq = 0.0;

  maybe_log_offset(offset);
  LCL_AccumulateFrequencyAndOffset(frequency, offset);

  abs_freq_ppm = LCL_ReadAbsoluteFrequency();

  if (logfile) {
    millisecond = ref_time->tv_usec / 1000;

    fprintf(logfile, "%5s %-15s %2d %10.3f %10.3f %10.3e\n",
            UTI_TimeToLogForm(ref_time->tv_sec),
            "127.127.1.1",
            our_stratum,
            abs_freq_ppm,
            1.0e6*our_skew,
            our_offset);

    fflush(logfile);
  }

  if (drift_file) {
    update_drift_file(abs_freq_ppm, our_skew);
  }
}

/* ================================================== */

void
REF_SetUnsynchronised(void)
{
  /* Variables required for logging to statistics log */
  int millisecond;
  struct timeval now;
  double local_clock_err;

  assert(initialised);

  if (logfile) {
    LCL_ReadCookedTime(&now, &local_clock_err);

    millisecond = now.tv_usec / 1000;

    fprintf(logfile, "%s %-15s  0 %10.3f %10.3f %10.3e\n",
            UTI_TimeToLogForm(now.tv_sec),
            "0.0.0.0",
            LCL_ReadAbsoluteFrequency(),
            1.0e6*our_skew,
            0.0);
    
    fflush(logfile);
  }

  are_we_synchronised = 0;
}

/* ================================================== */

void
REF_GetReferenceParams
(
 struct timeval *local_time,
 int *is_synchronised,
 NTP_Leap *leap_status,
 int *stratum,
 unsigned long *ref_id,
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
    extra_dispersion = (our_skew + fabs(our_residual_freq)) * elapsed;

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
    *ref_id = 0UL;
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

void
REF_GetTrackingReport(RPT_TrackingReport *rep)
{
  double elapsed;
  double extra_dispersion;
  struct timeval now_raw, now_cooked;
  double correction;

  LCL_ReadRawTime(&now_raw);
  correction = LCL_GetOffsetCorrection(&now_raw);
  UTI_AddDoubleToTimeval(&now_raw, correction, &now_cooked);

  if (are_we_synchronised) {
    
    UTI_DiffTimevalsToDouble(&elapsed, &now_cooked, &our_ref_time);
    extra_dispersion = (our_skew + fabs(our_residual_freq)) * elapsed;
    
    rep->ref_id = our_ref_id;
    rep->stratum = our_stratum;
    rep->ref_time = our_ref_time;
    UTI_DoubleToTimeval(correction, &rep->current_correction);
    rep->freq_ppm = LCL_ReadAbsoluteFrequency();
    rep->resid_freq_ppm = 1.0e6 * our_residual_freq;
    rep->skew_ppm = 1.0e6 * our_skew;
    rep->root_delay = our_root_delay;
    rep->root_dispersion = our_root_dispersion + extra_dispersion;

  } else if (enable_local_stratum) {

    rep->ref_id = LOCAL_REFERENCE_ID;
    rep->stratum = local_stratum;
    rep->ref_time = now_cooked;
    UTI_DoubleToTimeval(correction, &rep->current_correction);
    rep->freq_ppm = LCL_ReadAbsoluteFrequency();
    rep->resid_freq_ppm = 0.0;
    rep->skew_ppm = 0.0;
    rep->root_delay = 0.0;
    rep->root_dispersion = LCL_GetSysPrecisionAsQuantum();

  } else {

    rep->ref_id = 0UL;
    rep->stratum = 0;
    rep->ref_time.tv_sec = 0;
    rep->ref_time.tv_usec = 0;
    UTI_DoubleToTimeval(correction, &rep->current_correction);
    rep->freq_ppm = LCL_ReadAbsoluteFrequency();
    rep->resid_freq_ppm = 0.0;
    rep->skew_ppm = 0.0;
    rep->root_delay = 0.0;
    rep->root_dispersion = 0.0;
  }

}

/* ================================================== */

void
REF_CycleLogFile(void)
{
  if (logfile && logfilename) {
    fclose(logfile);
    logfile = fopen(logfilename, "a");
    if (!logfile) {
      LOG(LOGS_WARN, LOGF_Reference, "Could not reopen logfile %s", logfilename);
    }
    logwrites = 0;
  }
}

/* ================================================== */
