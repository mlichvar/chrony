/*
  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Richard P. Curnow  1997-2003
 * Copyright (C) Miroslav Lichvar  2009-2012, 2014-2015
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

  Driver for systems that implement the adjtimex()/ntp_adjtime() system call
  */

#include "config.h"

#include "sysincl.h"

#include "conf.h"
#include "sys_generic.h"
#include "sys_timex.h"
#include "logging.h"

#ifdef LINUX
#define NTP_ADJTIME adjtimex
#define NTP_ADJTIME_NAME "adjtimex"
#else
#define NTP_ADJTIME ntp_adjtime
#define NTP_ADJTIME_NAME "ntp_adjtime"
#endif

/* Maximum frequency offset accepted by the kernel (in ppm) */
#define MAX_FREQ 500.0

/* Frequency scale to convert from ppm to the timex freq */
#define FREQ_SCALE (double)(1 << 16)

/* Threshold for the timex maxerror when the kernel sets the UNSYNC flag */
#define MAX_SYNC_ERROR 16.0

/* Minimum assumed rate at which the kernel updates the clock frequency */
#define MIN_TICK_RATE 100

/* Saved timex status */
static int status;

/* ================================================== */

static double
read_frequency(void)
{
  struct timex txc;

  txc.modes = 0;

  SYS_Timex_Adjust(&txc, 0);

  return txc.freq / -FREQ_SCALE;
}

/* ================================================== */

static double
set_frequency(double freq_ppm)
{
  struct timex txc;

  txc.modes = MOD_FREQUENCY;
  txc.freq = freq_ppm * -FREQ_SCALE;

  SYS_Timex_Adjust(&txc, 0);

  return txc.freq / -FREQ_SCALE;
}

/* ================================================== */

static void
set_leap(int leap)
{
  struct timex txc;
  int applied;

  applied = 0;
  if (!leap) {
    txc.modes = 0;
    if (SYS_Timex_Adjust(&txc, 1) == TIME_WAIT)
      applied = 1;
  }

  status &= ~(STA_INS | STA_DEL);

  if (leap > 0)
    status |= STA_INS;
  else if (leap < 0)
    status |= STA_DEL;

  txc.modes = MOD_STATUS;
  txc.status = status;

  SYS_Timex_Adjust(&txc, 0);

  LOG(LOGS_INFO, LOGF_SysTimex, "System clock status %s leap second",
      leap ? (leap > 0 ? "set to insert" : "set to delete") :
      (applied ? "reset after" : "set to not insert/delete"));
}

/* ================================================== */

static void
set_sync_status(int synchronised, double est_error, double max_error)
{
  struct timex txc;

  if (synchronised) {
    if (est_error > MAX_SYNC_ERROR)
      est_error = MAX_SYNC_ERROR;
    if (max_error >= MAX_SYNC_ERROR) {
      max_error = MAX_SYNC_ERROR;
      synchronised = 0;
    }
  } else {
    est_error = max_error = MAX_SYNC_ERROR;
  }

#ifdef LINUX
  /* On Linux clear the UNSYNC flag only if rtcsync is enabled */
  if (!CNF_GetRtcSync())
    synchronised = 0;
#endif

  if (synchronised)
    status &= ~STA_UNSYNC;
  else
    status |= STA_UNSYNC;

  txc.modes = MOD_STATUS | MOD_ESTERROR | MOD_MAXERROR;
  txc.status = status;
  txc.esterror = est_error * 1.0e6;
  txc.maxerror = max_error * 1.0e6;

  SYS_Timex_Adjust(&txc, 1);
}

/* ================================================== */

static void
initialise_timex(void)
{
  struct timex txc;

  status = STA_UNSYNC;

  /* Reset PLL offset */
  txc.modes = MOD_OFFSET | MOD_STATUS;
  txc.status = STA_PLL | status;
  txc.offset = 0;
  SYS_Timex_Adjust(&txc, 0);

  /* Turn PLL off */
  txc.modes = MOD_STATUS;
  txc.status = status;
  SYS_Timex_Adjust(&txc, 0);
}

/* ================================================== */

void
SYS_Timex_Initialise(void)
{
  SYS_Timex_InitialiseWithFunctions(MAX_FREQ, 1.0 / MIN_TICK_RATE, NULL, NULL, NULL);
}

/* ================================================== */

void
SYS_Timex_InitialiseWithFunctions(double max_set_freq_ppm, double max_set_freq_delay,
                                  lcl_ReadFrequencyDriver sys_read_freq,
                                  lcl_SetFrequencyDriver sys_set_freq,
                                  lcl_ApplyStepOffsetDriver sys_apply_step_offset)
{
  initialise_timex();

  SYS_Generic_CompleteFreqDriver(max_set_freq_ppm, max_set_freq_delay,
                                 sys_read_freq ? sys_read_freq : read_frequency,
                                 sys_set_freq ? sys_set_freq : set_frequency,
                                 sys_apply_step_offset, set_leap, set_sync_status);
}

/* ================================================== */

void
SYS_Timex_Finalise(void)
{
  SYS_Generic_Finalise();
}

/* ================================================== */

int
SYS_Timex_Adjust(struct timex *txc, int ignore_error)
{
  int state;

  state = NTP_ADJTIME(txc);

  if (state < 0) {
    if (!ignore_error)
      LOG_FATAL(LOGF_SysTimex, NTP_ADJTIME_NAME"(0x%x) failed : %s",
                txc->modes, strerror(errno));
    else
      DEBUG_LOG(LOGF_SysTimex, NTP_ADJTIME_NAME"(0x%x) failed : %s",
                txc->modes, strerror(errno));
  }

  return state;
}
