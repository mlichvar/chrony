/*
  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Richard P. Curnow  1997-2003
 * Copyright (C) Miroslav Lichvar  2011, 2014
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

  The routines in this file present a common local (system) clock
  interface to the rest of the software.

  They interface with the system specific driver files in sys_*.c
  */

#include "config.h"

#include "sysincl.h"

#include "conf.h"
#include "local.h"
#include "localp.h"
#include "memory.h"
#include "util.h"
#include "logging.h"

/* ================================================== */

/* Variable to store the current frequency, in ppm */
static double current_freq_ppm;

/* Temperature compensation, in ppm */
static double temp_comp_ppm;

/* ================================================== */
/* Store the system dependent drivers */

static lcl_ReadFrequencyDriver drv_read_freq;
static lcl_SetFrequencyDriver drv_set_freq;
static lcl_AccrueOffsetDriver drv_accrue_offset;
static lcl_ApplyStepOffsetDriver drv_apply_step_offset;
static lcl_OffsetCorrectionDriver drv_offset_convert;
static lcl_SetLeapDriver drv_set_leap;
static lcl_SetSyncStatusDriver drv_set_sync_status;

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

static double max_clock_error;

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
  struct timeval tv, old_tv;
  int dusec, best_dusec;
  int iters;

  gettimeofday(&old_tv, NULL);
  best_dusec = 1000000; /* Assume we must be better than a second */
  iters = 0;
  do {
    gettimeofday(&tv, NULL);
    dusec = 1000000*(tv.tv_sec - old_tv.tv_sec) + (tv.tv_usec - old_tv.tv_usec);
    old_tv = tv;
    if (dusec > 0)  {
      if (dusec < best_dusec) {
        best_dusec = dusec;
      }
      iters++;
    }
  } while (iters < NITERS);

  assert(best_dusec > 0);

  precision_quantum = best_dusec * 1.0e-6;

  /* Get rounded log2 value of the measured precision */
  precision_log = 0;
  while (best_dusec < 707107) {
    precision_log--;
    best_dusec *= 2;
  }

  DEBUG_LOG(LOGF_Local, "Clock precision %.9f (%d)", precision_quantum, precision_log);
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
  temp_comp_ppm = 0.0;

  calculate_sys_precision();

  max_clock_error = CNF_GetMaxClockError() * 1e-6;
}

/* ================================================== */

void
LCL_Finalise(void)
{
  while (change_list.next != &change_list)
    LCL_RemoveParameterChangeHandler(change_list.next->handler,
                                     change_list.next->anything);

  while (dispersion_notify_list.next != &dispersion_notify_list)
    LCL_RemoveDispersionNotifyHandler(dispersion_notify_list.next->handler,
                                      dispersion_notify_list.next->anything);
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

double
LCL_GetMaxClockError(void)
{
  return max_clock_error;
}

/* ================================================== */

void
LCL_AddParameterChangeHandler(LCL_ParameterChangeHandler handler, void *anything)
{
  ChangeListEntry *ptr, *new_entry;

  /* Check that the handler is not already registered */
  for (ptr = change_list.next; ptr != &change_list; ptr = ptr->next) {
    if (!(ptr->handler != handler || ptr->anything != anything)) {
      assert(0);
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
}

/* ================================================== */

/* Remove a handler */
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

  assert(ok);

  /* Unlink entry from the list */
  ptr->next->prev = ptr->prev;
  ptr->prev->next = ptr->next;

  Free(ptr);
}

/* ================================================== */

int
LCL_IsFirstParameterChangeHandler(LCL_ParameterChangeHandler handler)
{
  return change_list.next->handler == handler;
}

/* ================================================== */

static void
invoke_parameter_change_handlers(struct timeval *raw, struct timeval *cooked,
                                 double dfreq, double doffset,
                                 LCL_ChangeType change_type)
{
  ChangeListEntry *ptr;

  for (ptr = change_list.next; ptr != &change_list; ptr = ptr->next) {
    (ptr->handler)(raw, cooked, dfreq, doffset, change_type, ptr->anything);
  }
}

/* ================================================== */

void
LCL_AddDispersionNotifyHandler(LCL_DispersionNotifyHandler handler, void *anything)
{
  DispersionNotifyListEntry *ptr, *new_entry;

  /* Check that the handler is not already registered */
  for (ptr = dispersion_notify_list.next; ptr != &dispersion_notify_list; ptr = ptr->next) {
    if (!(ptr->handler != handler || ptr->anything != anything)) {
      assert(0);
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

  assert(ok);

  /* Unlink entry from the list */
  ptr->next->prev = ptr->prev;
  ptr->prev->next = ptr->next;

  Free(ptr);
}

/* ================================================== */
/* At the moment, this is just gettimeofday(), because
   I can't think of a Unix system where it would not be */

void
LCL_ReadRawTime(struct timeval *result)
{
  if (gettimeofday(result, NULL) < 0) {
    LOG_FATAL(LOGF_Local, "gettimeofday() failed");
  }
}

/* ================================================== */

void
LCL_ReadCookedTime(struct timeval *result, double *err)
{
  struct timeval raw;

  LCL_ReadRawTime(&raw);
  LCL_CookTime(&raw, result, err);
}

/* ================================================== */

void
LCL_CookTime(struct timeval *raw, struct timeval *cooked, double *err)
{
  double correction;

  LCL_GetOffsetCorrection(raw, &correction, err);
  UTI_AddDoubleToTimeval(raw, correction, cooked);
}

/* ================================================== */

void
LCL_GetOffsetCorrection(struct timeval *raw, double *correction, double *err)
{
  /* Call system specific driver to get correction */
  (*drv_offset_convert)(raw, correction, err);
}

/* ================================================== */
/* Return current frequency */

double
LCL_ReadAbsoluteFrequency(void)
{
  double freq;

  freq = current_freq_ppm; 

  /* Undo temperature compensation */
  if (temp_comp_ppm != 0.0) {
    freq = (freq + temp_comp_ppm) / (1.0 - 1.0e-6 * temp_comp_ppm);
  }

  return freq;
}

/* ================================================== */
/* This involves both setting the absolute frequency with the
   system-specific driver, as well as calling all notify handlers */

void
LCL_SetAbsoluteFrequency(double afreq_ppm)
{
  struct timeval raw, cooked;
  double dfreq;
  
  /* Apply temperature compensation */
  if (temp_comp_ppm != 0.0) {
    afreq_ppm = afreq_ppm * (1.0 - 1.0e-6 * temp_comp_ppm) - temp_comp_ppm;
  }

  /* Call the system-specific driver for setting the frequency */
  
  afreq_ppm = (*drv_set_freq)(afreq_ppm);

  dfreq = (afreq_ppm - current_freq_ppm) / (1.0e6 - current_freq_ppm);

  LCL_ReadRawTime(&raw);
  LCL_CookTime(&raw, &cooked, NULL);

  /* Dispatch to all handlers */
  invoke_parameter_change_handlers(&raw, &cooked, dfreq, 0.0, LCL_ChangeAdjust);

  current_freq_ppm = afreq_ppm;

}

/* ================================================== */

void
LCL_AccumulateDeltaFrequency(double dfreq)
{
  struct timeval raw, cooked;
  double old_freq_ppm;

  old_freq_ppm = current_freq_ppm;

  /* Work out new absolute frequency.  Note that absolute frequencies
   are handled in units of ppm, whereas the 'dfreq' argument is in
   terms of the gradient of the (offset) v (local time) function. */

  current_freq_ppm += dfreq * (1.0e6 - current_freq_ppm);

  /* Call the system-specific driver for setting the frequency */
  current_freq_ppm = (*drv_set_freq)(current_freq_ppm);
  dfreq = (current_freq_ppm - old_freq_ppm) / (1.0e6 - old_freq_ppm);

  LCL_ReadRawTime(&raw);
  LCL_CookTime(&raw, &cooked, NULL);

  /* Dispatch to all handlers */
  invoke_parameter_change_handlers(&raw, &cooked, dfreq, 0.0, LCL_ChangeAdjust);
}

/* ================================================== */

void
LCL_AccumulateOffset(double offset, double corr_rate)
{
  struct timeval raw, cooked;

  /* In this case, the cooked time to be passed to the notify clients
     has to be the cooked time BEFORE the change was made */

  LCL_ReadRawTime(&raw);
  LCL_CookTime(&raw, &cooked, NULL);

  (*drv_accrue_offset)(offset, corr_rate);

  /* Dispatch to all handlers */
  invoke_parameter_change_handlers(&raw, &cooked, 0.0, offset, LCL_ChangeAdjust);
}

/* ================================================== */

void
LCL_ApplyStepOffset(double offset)
{
  struct timeval raw, cooked;

  /* In this case, the cooked time to be passed to the notify clients
     has to be the cooked time BEFORE the change was made */

  LCL_ReadRawTime(&raw);
  LCL_CookTime(&raw, &cooked, NULL);

  (*drv_apply_step_offset)(offset);

  /* Dispatch to all handlers */
  invoke_parameter_change_handlers(&raw, &cooked, 0.0, offset, LCL_ChangeStep);
}

/* ================================================== */

void
LCL_NotifyExternalTimeStep(struct timeval *raw, struct timeval *cooked,
    double offset, double dispersion)
{
  /* Dispatch to all handlers */
  invoke_parameter_change_handlers(raw, cooked, 0.0, offset, LCL_ChangeUnknownStep);

  lcl_InvokeDispersionNotifyHandlers(dispersion);
}

/* ================================================== */

void
LCL_NotifyLeap(int leap)
{
  struct timeval raw, cooked;

  LCL_ReadRawTime(&raw);
  LCL_CookTime(&raw, &cooked, NULL);

  /* Dispatch to all handlers as if the clock was stepped */
  invoke_parameter_change_handlers(&raw, &cooked, 0.0, -leap, LCL_ChangeStep);
}

/* ================================================== */

void
LCL_AccumulateFrequencyAndOffset(double dfreq, double doffset, double corr_rate)
{
  struct timeval raw, cooked;
  double old_freq_ppm;

  LCL_ReadRawTime(&raw);
  /* Due to modifying the offset, this has to be the cooked time prior
     to the change we are about to make */
  LCL_CookTime(&raw, &cooked, NULL);

  old_freq_ppm = current_freq_ppm;

  /* Work out new absolute frequency.  Note that absolute frequencies
   are handled in units of ppm, whereas the 'dfreq' argument is in
   terms of the gradient of the (offset) v (local time) function. */
  current_freq_ppm += dfreq * (1.0e6 - current_freq_ppm);

  DEBUG_LOG(LOGF_Local, "old_freq=%.3fppm new_freq=%.3fppm offset=%.6fsec",
      old_freq_ppm, current_freq_ppm, doffset);

  /* Call the system-specific driver for setting the frequency */
  current_freq_ppm = (*drv_set_freq)(current_freq_ppm);
  dfreq = (current_freq_ppm - old_freq_ppm) / (1.0e6 - old_freq_ppm);

  (*drv_accrue_offset)(doffset, corr_rate);

  /* Dispatch to all handlers */
  invoke_parameter_change_handlers(&raw, &cooked, dfreq, doffset, LCL_ChangeAdjust);
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
                          lcl_SetLeapDriver set_leap,
                          lcl_SetSyncStatusDriver set_sync_status)
{
  drv_read_freq = read_freq;
  drv_set_freq = set_freq;
  drv_accrue_offset = accrue_offset;
  drv_apply_step_offset = apply_step_offset;
  drv_offset_convert = offset_convert;
  drv_set_leap = set_leap;
  drv_set_sync_status = set_sync_status;

  current_freq_ppm = (*drv_read_freq)();

  DEBUG_LOG(LOGF_Local, "Local freq=%.3fppm", current_freq_ppm);
}

/* ================================================== */
/* Look at the current difference between the system time and the NTP
   time, and make a step to cancel it. */

int
LCL_MakeStep(void)
{
  struct timeval raw;
  double correction;

  LCL_ReadRawTime(&raw);
  LCL_GetOffsetCorrection(&raw, &correction, NULL);

  /* Cancel remaining slew and make the step */
  LCL_AccumulateOffset(correction, 0.0);
  LCL_ApplyStepOffset(-correction);

  LOG(LOGS_WARN, LOGF_Local, "System clock was stepped by %.6f seconds", correction);

  return 1;
}

/* ================================================== */

void
LCL_SetSystemLeap(int leap)
{
  if (drv_set_leap) {
    (drv_set_leap)(leap);
  }
}

/* ================================================== */

double
LCL_SetTempComp(double comp)
{
  double uncomp_freq_ppm;

  if (temp_comp_ppm == comp)
    return comp;

  /* Undo previous compensation */
  current_freq_ppm = (current_freq_ppm + temp_comp_ppm) /
    (1.0 - 1.0e-6 * temp_comp_ppm);

  uncomp_freq_ppm = current_freq_ppm;

  /* Apply new compensation */
  current_freq_ppm = current_freq_ppm * (1.0 - 1.0e-6 * comp) - comp;

  /* Call the system-specific driver for setting the frequency */
  current_freq_ppm = (*drv_set_freq)(current_freq_ppm);

  temp_comp_ppm = (uncomp_freq_ppm - current_freq_ppm) /
    (1.0e-6 * uncomp_freq_ppm + 1.0);

  return temp_comp_ppm;
}

/* ================================================== */

void
LCL_SetSyncStatus(int synchronised, double est_error, double max_error)
{
  if (drv_set_sync_status) {
    (drv_set_sync_status)(synchronised, est_error, max_error);
  }
}

/* ================================================== */
