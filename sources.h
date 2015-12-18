/*
  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Richard P. Curnow  1997-2002
 * Copyright (C) Miroslav Lichvar  2014
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

  This is the header for the module that manages the collection of all
  sources that we are making measurements from.  This include all NTP
  servers & peers, locally connected reference sources, eye/wristwatch
  drivers etc */

#ifndef GOT_SOURCES_H
#define GOT_SOURCES_H

#include "sysincl.h"

#include "ntp.h"
#include "reports.h"

/* Size of the source reachability register */
#define SOURCE_REACH_BITS 8

/* This datatype is used to hold information about sources.  The
   instance must be passed when calling many of the interface
   functions */

typedef struct SRC_Instance_Record *SRC_Instance;

/* Initialisation function */
extern void SRC_Initialise(void);

/* Finalisation function */
extern void SRC_Finalise(void);

typedef enum {
  SRC_NTP,                      /* NTP client/peer */
  SRC_REFCLOCK                  /* Rerefence clock */
} SRC_Type;

/* Function to create a new instance.  This would be called by one of
   the individual source-type instance creation routines. */

extern SRC_Instance SRC_CreateNewInstance(uint32_t ref_id, SRC_Type type, int sel_options, IPAddr *addr, int min_samples, int max_samples);

/* Function to get rid of a source when it is being unconfigured.
   This may cause the current reference source to be reselected, if this
   was the reference source or contributed significantly to a
   falseticker decision. */

extern void SRC_DestroyInstance(SRC_Instance instance);

/* Function to reset a source */
extern void SRC_ResetInstance(SRC_Instance instance);

/* Function to change the sources's reference ID and IP address */
extern void SRC_SetRefid(SRC_Instance instance, uint32_t ref_id, IPAddr *addr);

/* Function to get the range of frequencies, relative to the given
   source, that we believe the local clock lies within.  The return
   values are in terms of the number of seconds fast (+ve) or slow
   (-ve) relative to the source that the local clock becomes after a
   given amount of local time has elapsed.

   Suppose the initial offset relative to the source is U (fast +ve,
   slow -ve) and a time interval T elapses measured in terms of the
   local clock.  Then the error relative to the source at the end of
   the interval should lie in the interval [U+T*lo, U+T*hi]. */

extern void SRC_GetFrequencyRange(SRC_Instance instance, double *lo, double *hi);

/* This function is called by one of the source drivers when it has
   a new sample that is to be accumulated.

   This function causes the frequency estimation to be re-run for the
   designated source, and the clock selection procedure to be re-run
   afterwards.

   sample_time is the local time at which the sample is to be
   considered to have been made, in terms of doing a regression fit of
   offset against local time.

   offset is the offset at the time, in seconds.  Positive indicates
   that the local clock is SLOW relative to the source, negative
   indicates that the local clock is FAST relative to it.

   root_delay and root_dispersion are in seconds, and are as per
   RFC 5905.  root_dispersion only includes the peer's root dispersion
   + local sampling precision + skew dispersion accrued during the
   measurement.  It is the job of the source statistics algorithms +
   track.c to add on the extra dispersion due to the residual standard
   deviation of the offsets from this source after regression, to form
   the root_dispersion field in the packets transmitted to clients or
   peers.

   stratum is the stratum of the source that supplied the sample.

   */

extern void SRC_AccumulateSample(SRC_Instance instance, struct timeval *sample_time, double offset, double peer_delay, double peer_dispersion, double root_delay, double root_dispersion, int stratum, NTP_Leap leap_status);

/* This routine sets the source as receiving reachability updates */
extern void SRC_SetActive(SRC_Instance inst);

/* This routine sets the source as not receiving reachability updates */
extern void SRC_UnsetActive(SRC_Instance inst);

/* This routine updates the reachability register */
extern void SRC_UpdateReachability(SRC_Instance inst, int reachable);

/* This routine marks the source unreachable */
extern void SRC_ResetReachability(SRC_Instance inst);

/* This routine is used to select the best source from amongst those
   we currently have valid data on, and use it as the tracking base
   for the local time.  Updates are made to the local reference only
   when the selected source was updated (set as updated_inst) since
   the last reference update.  This avoids updating the frequency
   tracking for every sample from other sources - only the ones from
   the selected reference make a difference. */
extern void SRC_SelectSource(SRC_Instance updated_inst);

/* Force reselecting the best source */
extern void SRC_ReselectSource(void);

/* Set reselect distance */
extern void SRC_SetReselectDistance(double distance);

/* Predict the offset of the local clock relative to a given source at
   a given local cooked time. Positive indicates local clock is FAST
   relative to reference. */
extern double SRC_PredictOffset(SRC_Instance inst, struct timeval *when);

/* Return the minimum peer delay amongst the previous samples
   currently held in the register */
extern double SRC_MinRoundTripDelay(SRC_Instance inst);

/* This routine determines if a new sample is good enough that it should be
   accumulated */
extern int SRC_IsGoodSample(SRC_Instance inst, double offset, double delay, double max_delay_dev_ratio, double clock_error, struct timeval *when);

extern void SRC_DumpSources(void);

extern void SRC_ReloadSources(void);

extern int SRC_IsSyncPeer(SRC_Instance inst);
extern int SRC_IsReachable(SRC_Instance inst);
extern int SRC_ReadNumberOfSources(void);
extern int SRC_ActiveSources(void);
extern int SRC_ReportSource(int index, RPT_SourceReport *report, struct timeval *now);

extern int SRC_ReportSourcestats(int index, RPT_SourcestatsReport *report, struct timeval *now);

extern SRC_Type SRC_GetType(int index);

extern int SRC_Samples(SRC_Instance inst);

#endif /* GOT_SOURCES_H */

