/*
  $Header: /cvs/src/chrony/local.c,v 1.21 2003/09/22 21:22:30 richard Exp $

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

  The routines in this file present a common local (system) clock
  interface to the rest of the software.

  They interface with the system specific driver files in sys_*.c
  */

#include <assert.h>
#include <stddef.h>

#include "local.h"
#include "localp.h"
#include "memory.h"
#include "util.h"
#include "logging.h"

/* ================================================== */

/* Variable to store the current frequency, in ppm */
static double current_freq_ppm;

/* ================================================== */
/* Store the system dependent drivers */

static lcl_ReadFrequencyDriver drv_read_freq;
static lcl_SetFrequencyDriver drv_set_freq;
static lcl_AccrueOffsetDriver drv_accrue_offset;
static lcl_ApplyStepOffsetDriver drv_apply_step_offset;
static lcl_OffsetCorrectionDriver drv_offset_convert;
static lcl_ImmediateStepDriver drv_immediate_step;

/* ================================================== */

/* Types and variables associated with handling the parameter change
   list */

typedef struct _ChangeListEntry {
  struct _ChangeListEntry *next;
  struct _ChangeListEntry *prev;
  LCL_ParameterChangeHandler handler;
  void *anything;
} ChangeListEntry;

static ChangeListEntry change_list;

/* ================================================== */

/* Types and variables associated with handling the parameter change
   list */

typedef struct _DispersionNotifyListEntry {
  struct _DispersionNotifyListEntry *next;
  struct _DispersionNotifyListEntry *prev;
  LCL_DispersionNotifyHandler handler;
  void *anything;
} DispersionNotifyListEntry;

static DispersionNotifyListEntry dispersion_notify_list;

/* ================================================== */

static int precision_log;
static double precision_quantum;

/* ================================================== */

/* Define the number of increments of the system clock that we want
   to see to be fairly sure that we've got something approaching
   the minimum increment.  Even on a crummy implementation that can't
   interpolate between 10ms ticks, we should get this done in
   under 1s of busy waiting. */
#define NITERS 100

static void
calculate_sys_precision(void)
{
  struct timeval tv, old_tv, first_tv;
  struct timezone tz;
  int dusec, best_dusec;
  int iters;

  gettimeofday(&old_tv, &tz);
  first_tv = old_tv;
  best_dusec = 1000000; /* Assume we must be better than a second */
  iters = 0;
  do {
    gettimeofday(&tv, &tz);
    dusec = 1000000*(tv.tv_sec - old_tv.tv_sec) + (tv.tv_usec - old_tv.tv_usec);
    old_tv = tv;
    if (dusec > 0)  {
      if (dusec < best_dusec) {
        best_dusec = dusec;
      }
      iters++;
    }
  } while (iters < NITERS);
  if (!(best_dusec > 0)) {
    CROAK("best_dusec should be positive");
  }
  precision_log = 0;
  while (best_dusec < 500000) {
    precision_log--;
    best_dusec *= 2;
  }

  precision_quantum = 1.0 / (double)(1<<(-precision_log));

  return;
}

/* ================================================== */

void
LCL_Initialise(void)
{
  change_list.next = change_list.prev = &change_list;

  dispersion_notify_list.next = dispersion_notify_list.prev = &dispersion_notify_list;

  /* Null out the system drivers, so that we die
     if they never get defined before use */
  
  drv_read_freq = NULL;
  drv_set_freq = NULL;
  drv_accrue_offset = NULL;
  drv_offset_convert = NULL;

  /* This ought to be set from the system driver layer */
  current_freq_ppm = 0.0;

  calculate_sys_precision();
}

/* ================================================== */

void
LCL_Finalise(void)
{
  return;
}

/* ================================================== */

/* Routine to read the system precision as a log to base 2 value. */
int
LCL_GetSysPrecisionAsLog(void)
{
  return precision_log;
}

/* ================================================== */
/* Routine to read the system precision in terms of the actual time step */

double
LCL_GetSysPrecisionAsQuantum(void)
{
  return precision_quantum;
}

/* ================================================== */

void
LCL_AddParameterChangeHandler(LCL_ParameterChangeHandler handler, void *anything)
{
  ChangeListEntry *ptr, *new_entry;

  /* Check that the handler is not already registered */
  for (ptr = change_list.next; ptr != &change_list; ptr = ptr->next) {
    if (!(ptr->handler != handler || ptr->anything != anything)) {
      CROAK("a handler is already registered");
    }
  }

  new_entry = MallocNew(ChangeListEntry);

  new_entry->handler = handler;
  new_entry->anything = anything;

  /* Chain it into the list */
  new_entry->next = &change_list;
  new_entry->prev = change_list.prev;
  change_list.prev->next = new_entry;
  change_list.prev = new_entry;

  return;
}

/* ================================================== */

/* Remove a handler */
extern 
void LCL_RemoveParameterChangeHandler(LCL_ParameterChangeHandler handler, void *anything)
{

  ChangeListEntry *ptr;
  int ok;

  ptr = NULL;
  ok = 0;

  for (ptr = change_list.next; ptr != &change_list; ptr = ptr->next) {
    if (ptr->handler == handler && ptr->anything == anything) {
      ok = 1;
      break;
    }
  }

  if (!ok) {
    CROAK("did not find a matching handler");
  }

  /* Unlink entry from the list */
  ptr->next->prev = ptr->prev;
  ptr->prev->next = ptr->next;

  free(ptr);

  return;
}

/* ================================================== */

void
LCL_AddDispersionNotifyHandler(LCL_DispersionNotifyHandler handler, void *anything)
{
  DispersionNotifyListEntry *ptr, *new_entry;

  /* Check that the handler is not already registered */
  for (ptr = dispersion_notify_list.next; ptr != &dispersion_notify_list; ptr = ptr->next) {
    if (!(ptr->handler != handler || ptr->anything != anything)) {
      CROAK("a handler is already registered");
    }
  }

  new_entry = MallocNew(DispersionNotifyListEntry);

  new_entry->handler = handler;
  new_entry->anything = anything;

  /* Chain it into the list */
  new_entry->next = &dispersion_notify_list;
  new_entry->prev = dispersion_notify_list.prev;
  dispersion_notify_list.prev->next = new_entry;
  dispersion_notify_list.prev = new_entry;

  return;
}

/* ================================================== */

/* Remove a handler */
extern 
void LCL_RemoveDispersionNotifyHandler(LCL_DispersionNotifyHandler handler, void *anything)
{

  DispersionNotifyListEntry *ptr;
  int ok;

  ptr = NULL;
  ok = 0;

  for (ptr = dispersion_notify_list.next; ptr != &dispersion_notify_list; ptr = ptr->next) {
    if (ptr->handler == handler && ptr->anything == anything) {
      ok = 1;
      break;
    }
  }

  if (!ok) {
    CROAK("no matching handler found");
  }

  /* Unlink entry from the list */
  ptr->next->prev = ptr->prev;
  ptr->prev->next = ptr->next;

  free(ptr);

  return;
}

/* ================================================== */
/* At the moment, this is just gettimeofday(), because
   I can't think of a Unix system where it would not be */

void
LCL_ReadRawTime(struct timeval *result)
{
  struct timezone tz;

  if (!(gettimeofday(result, &tz) >= 0)) {
    CROAK("Could not get time of day");
  }
  return;

}

/* ================================================== */

void
LCL_ReadCookedTime(struct timeval *result, double *err)
{
  struct timeval raw;
  double correction;

  LCL_ReadRawTime(&raw);

  /* For now, cheat and set the error to zero in all cases.
   */

  *err = 0.0;

  /* Call system specific driver to get correction */
  (*drv_offset_convert)(&raw, &correction);
  UTI_AddDoubleToTimeval(&raw, correction, result);

  return;
}

/* ================================================== */

double
LCL_GetOffsetCorrection(struct timeval *raw)
{
  double correction;
  (*drv_offset_convert)(raw, &correction);
  return correction;
}

/* ================================================== */
/* This is just a simple passthrough of the system specific routine */

double
LCL_ReadAbsoluteFrequency(void)
{
  return (*drv_read_freq)();
}

/* ================================================== */
/* This involves both setting the absolute frequency with the
   system-specific driver, as well as calling all notify handlers */

void
LCL_SetAbsoluteFrequency(double afreq_ppm)
{
  ChangeListEntry *ptr;
  struct timeval raw, cooked;
  double correction;
  double dfreq;
  
  /* Call the system-specific driver for setting the frequency */
  
  (*drv_set_freq)(afreq_ppm);

  dfreq = 1.0e-6 * (afreq_ppm - current_freq_ppm) / (1.0 - 1.0e-6 * current_freq_ppm);

  LCL_ReadRawTime(&raw);
  (drv_offset_convert)(&raw, &correction);
  UTI_AddDoubleToTimeval(&raw, correction, &cooked);

  /* Dispatch to all handlers */
  for (ptr = change_list.next; ptr != &change_list; ptr = ptr->next) {
    (ptr->handler)(&raw, &cooked, dfreq, afreq_ppm, 0.0, 0, ptr->anything);
  }

  current_freq_ppm = afreq_ppm;

}

/* ================================================== */

void
LCL_AccumulateDeltaFrequency(double dfreq)
{
  ChangeListEntry *ptr;
  struct timeval raw, cooked;
  double correction;

  /* Work out new absolute frequency.  Note that absolute frequencies
   are handled in units of ppm, whereas the 'dfreq' argument is in
   terms of the gradient of the (offset) v (local time) function. */

  current_freq_ppm = (1.0 - dfreq) * current_freq_ppm +
    (1.0e6 * dfreq);

  /* Call the system-specific driver for setting the frequency */
  (*drv_set_freq)(current_freq_ppm);

  LCL_ReadRawTime(&raw);
  (drv_offset_convert)(&raw, &correction);
  UTI_AddDoubleToTimeval(&raw, correction, &cooked);

  /* Dispatch to all handlers */
  for (ptr = change_list.next; ptr != &change_list; ptr = ptr->next) {
    (ptr->handler)(&raw, &cooked, dfreq, current_freq_ppm, 0.0, 0, ptr->anything);
  }

}

/* ================================================== */

void
LCL_AccumulateOffset(double offset)
{
  ChangeListEntry *ptr;
  struct timeval raw, cooked;
  double correction;

  /* In this case, the cooked time to be passed to the notify clients
     has to be the cooked time BEFORE the change was made */

  LCL_ReadRawTime(&raw);
  (drv_offset_convert)(&raw, &correction);
  UTI_AddDoubleToTimeval(&raw, correction, &cooked);

  (*drv_accrue_offset)(offset);

  /* Dispatch to all handlers */
  for (ptr = change_list.next; ptr != &change_list; ptr = ptr->next) {
    (ptr->handler)(&raw, &cooked, 0.0, current_freq_ppm, offset, 0, ptr->anything);
  }

}

/* ================================================== */

void
LCL_ApplyStepOffset(double offset)
{
  ChangeListEntry *ptr;
  struct timeval raw, cooked;
  double correction;

  /* In this case, the cooked time to be passed to the notify clients
     has to be the cooked time BEFORE the change was made */

  LCL_ReadRawTime(&raw);
  (drv_offset_convert)(&raw, &correction);
  UTI_AddDoubleToTimeval(&raw, correction, &cooked);

  (*drv_apply_step_offset)(offset);

  /* Dispatch to all handlers */
  for (ptr = change_list.next; ptr != &change_list; ptr = ptr->next) {
    (ptr->handler)(&raw, &cooked, 0.0, current_freq_ppm, offset, 1, ptr->anything);
  }

}

/* ================================================== */

void
LCL_AccumulateFrequencyAndOffset(double dfreq, double doffset)
{
  ChangeListEntry *ptr;
  struct timeval raw, cooked;
  double correction;
  double old_freq_ppm;

  LCL_ReadRawTime(&raw);
  (drv_offset_convert)(&raw, &correction);
  /* Due to modifying the offset, this has to be the cooked time prior
     to the change we are about to make */
  UTI_AddDoubleToTimeval(&raw, correction, &cooked);

  old_freq_ppm = current_freq_ppm;

  /* Work out new absolute frequency.  Note that absolute frequencies
   are handled in units of ppm, whereas the 'dfreq' argument is in
   terms of the gradient of the (offset) v (local time) function. */
  current_freq_ppm = (1.0 - dfreq) * old_freq_ppm +
    (1.0e6 * dfreq);

#ifdef TRACEON
  LOG(LOGS_INFO, LOGF_Local, "old_freq=%.3fppm new_freq=%.3fppm offset=%.6fsec",
      old_freq_ppm, current_freq_ppm, doffset);
#endif

  /* Call the system-specific driver for setting the frequency */
  (*drv_set_freq)(current_freq_ppm);
  (*drv_accrue_offset)(doffset);

  /* Dispatch to all handlers */
  for (ptr = change_list.next; ptr != &change_list; ptr = ptr->next) {
    (ptr->handler)(&raw, &cooked, dfreq, current_freq_ppm, doffset, 0, ptr->anything);
  }


}

/* ================================================== */

void
lcl_InvokeDispersionNotifyHandlers(double dispersion)
{
  DispersionNotifyListEntry *ptr;

  for (ptr = dispersion_notify_list.next; ptr != &dispersion_notify_list; ptr = ptr->next) {
    (ptr->handler)(dispersion, ptr->anything);
  }

}

/* ================================================== */

void
lcl_RegisterSystemDrivers(lcl_ReadFrequencyDriver read_freq,
                          lcl_SetFrequencyDriver set_freq,
                          lcl_AccrueOffsetDriver accrue_offset,
                          lcl_ApplyStepOffsetDriver apply_step_offset,
                          lcl_OffsetCorrectionDriver offset_convert,
                          lcl_ImmediateStepDriver immediate_step)
{
  drv_read_freq = read_freq;
  drv_set_freq = set_freq;
  drv_accrue_offset = accrue_offset;
  drv_apply_step_offset = apply_step_offset;
  drv_offset_convert = offset_convert;
  drv_immediate_step = immediate_step;

  current_freq_ppm = (*drv_read_freq)();

#ifdef TRACEON
  LOG(LOGS_INFO, LOGF_Local, "Local freq=%.3fppm", current_freq_ppm);
#endif

  return;
}

/* ================================================== */
/* Look at the current difference between the system time and the NTP
   time, and make a step to cancel it. */

int
LCL_MakeStep(void)
{
  if (drv_immediate_step) {
    (drv_immediate_step)();
#ifdef TRACEON
    LOG(LOGS_INFO, LOGF_Local, "Made step to system time to apply remaining slew");
#endif
    return 1;
  }

  return 0;
}

/* ================================================== */
