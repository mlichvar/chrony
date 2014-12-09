/*
  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Richard P. Curnow  1997-2003
 * Copyright (C) John G. Hasler  2009
 * Copyright (C) Miroslav Lichvar  2009-2012, 2014
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

  This is the module specific to the Linux operating system.

  */

#include "config.h"

#include "sysincl.h"

#include <sys/utsname.h>

#if defined(HAVE_SCHED_SETSCHEDULER)
#  include <sched.h>
int SchedPriority = 0;
#endif

#if defined(HAVE_MLOCKALL)
#  include <sys/mman.h>
#include <sys/resource.h>
int LockAll = 0;
#endif

#ifdef FEAT_PRIVDROP
#include <sys/types.h>
#include <pwd.h>
#include <sys/prctl.h>
#include <sys/capability.h>
#include <grp.h>
#endif

#include "sys_generic.h"
#include "sys_linux.h"
#include "conf.h"
#include "logging.h"
#include "wrap_adjtimex.h"

/* This is the uncompensated system tick value */
static int nominal_tick;

/* Current tick value */
static int current_delta_tick;

/* The maximum amount by which 'tick' can be biased away from 'nominal_tick'
   (sys_adjtimex() in the kernel bounds this to 10%) */
static int max_tick_bias;

/* The kernel USER_HZ constant */
static int hz;
static double dhz; /* And dbl prec version of same for arithmetic */

/* Flag indicating whether adjtimex() can step the clock */
static int have_setoffset;

/* The assumed rate at which the effective frequency and tick values are
   updated in the kernel */
static int tick_update_hz;

/* ================================================== */

inline static long
our_round(double x)
{
  long y;

  if (x > 0.0)
    y = x + 0.5;
  else
    y = x - 0.5;

  return y;
}

/* ================================================== */
/* Positive means currently fast of true time, i.e. jump backwards */

static void
apply_step_offset(double offset)
{
  if (TMX_ApplyStepOffset(-offset) < 0) {
    LOG_FATAL(LOGF_SysLinux, "adjtimex() failed");
  }
}

/* ================================================== */
/* This call sets the Linux kernel frequency to a given value in parts
   per million relative to the nominal running frequency.  Nominal is taken to
   be tick=10000, freq=0 (for a USER_HZ==100 system, other values otherwise).
   The convention is that this is called with a positive argument if the local
   clock runs fast when uncompensated.  */

static double
set_frequency(double freq_ppm)
{
  long required_tick;
  double required_freq;
  int required_delta_tick;

  required_delta_tick = our_round(freq_ppm / dhz);

  /* Older kernels (pre-2.6.18) don't apply the frequency offset exactly as
     set by adjtimex() and a scaling constant (that depends on the internal
     kernel HZ constant) would be needed to compensate for the error. Because
     chronyd is closed loop it doesn't matter much if we don't scale the
     required frequency, but we want to prevent thrashing between two states
     when the system's frequency error is close to a multiple of USER_HZ.  With
     USER_HZ <= 250, the maximum frequency adjustment of 500 ppm overlaps at
     least two ticks and we can stick to the current tick if it's next to the
     required tick. */
  if (hz <= 250 && (required_delta_tick + 1 == current_delta_tick ||
                    required_delta_tick - 1 == current_delta_tick)) {
    required_delta_tick = current_delta_tick;
  }

  required_freq = -(freq_ppm - dhz * required_delta_tick);
  required_tick = nominal_tick - required_delta_tick;

  if (TMX_SetFrequency(&required_freq, required_tick) < 0) {
    LOG_FATAL(LOGF_SysLinux, "adjtimex failed for set_frequency, freq_ppm=%10.4e required_freq=%10.4e required_tick=%ld",
        freq_ppm, required_freq, required_tick);
  }

  current_delta_tick = required_delta_tick;

  return dhz * current_delta_tick - required_freq;
}

/* ================================================== */
/* Read the ppm frequency from the kernel */

static double
read_frequency(void)
{
  long tick;
  double freq;

  if (TMX_GetFrequency(&freq, &tick) < 0) {
    LOG_FATAL(LOGF_SysLinux, "adjtimex() failed");
  }

  current_delta_tick = nominal_tick - tick;
  
  return dhz * current_delta_tick - freq;
}

/* ================================================== */

static void
set_leap(int leap)
{
  if (TMX_SetLeap(leap) < 0) {
    LOG_FATAL(LOGF_SysLinux, "adjtimex() failed in set_leap");
  }

  LOG(LOGS_INFO, LOGF_SysLinux, "System clock status set to %s leap second",
     leap ? (leap > 0 ? "insert" : "delete") : "not insert/delete");
}

/* ================================================== */

/* Estimate the value of USER_HZ given the value of txc.tick that chronyd finds when
 * it starts.  The only credible values are 100 (Linux/x86) or powers of 2.
 * Also, the bounds checking inside the kernel's adjtimex system call enforces
 * a +/- 10% movement of tick away from the nominal value 1e6/USER_HZ. */

static void
guess_hz_and_shift_hz(int tick, int *hz, int *shift_hz)
{
  int i, tick_lo, tick_hi, ihz;
  double tick_nominal;
  /* Pick off the hz=100 case first */
  if (tick >= 9000 && tick <= 11000) {
    *hz = 100;
    *shift_hz = 7;
    return;
  }

  for (i=4; i<16; i++) { /* surely 16 .. 32768 is a wide enough range? */
    ihz = 1 << i;
    tick_nominal = 1.0e6 / (double) ihz;
    tick_lo = (int)(0.5 + tick_nominal*2.0/3.0);
    tick_hi = (int)(0.5 + tick_nominal*4.0/3.0);
    
    if (tick_lo < tick && tick <= tick_hi) {
      *hz = ihz;
      *shift_hz = i;
      return;
    }
  }

  /* oh dear.  doomed. */
  *hz = 0;
  *shift_hz = 0;
}

/* ================================================== */

static int
get_hz_and_shift_hz(int *hz, int *shift_hz)
{
#ifdef _SC_CLK_TCK
  if ((*hz = sysconf(_SC_CLK_TCK)) < 1) {
    return 0;
  }

  if (*hz == 100) {
    *shift_hz = 7;
    return 1;
  }

  for (*shift_hz = 1; (*hz >> *shift_hz) > 1; (*shift_hz)++)
    ;

  return 1;
#else
  return 0;
#endif
}

/* ================================================== */

static int
kernelvercmp(int major1, int minor1, int patch1,
    int major2, int minor2, int patch2)
{
  if (major1 != major2)
    return major1 - major2;
  if (minor1 != minor2)
    return minor1 - minor2;
  return patch1 - patch2;
}

/* ================================================== */
/* Compute the scaling to use on any frequency we set, according to
   the vintage of the Linux kernel being used. */

static void
get_version_specific_details(void)
{
  int major, minor, patch;
  int shift_hz;
  struct tmx_params tmx_params;
  struct utsname uts;
  
  if (!get_hz_and_shift_hz(&hz, &shift_hz)) {
    TMX_ReadCurrentParams(&tmx_params);

    guess_hz_and_shift_hz(tmx_params.tick, &hz, &shift_hz);

    if (!shift_hz) {
      LOG_FATAL(LOGF_SysLinux, "Can't determine hz (txc.tick=%ld txc.freq=%ld (%.8f) txc.offset=%ld)",
          tmx_params.tick, tmx_params.freq, tmx_params.dfreq, tmx_params.offset);
    }
  }

  dhz = (double) hz;
  nominal_tick = (1000000L + (hz/2))/hz; /* Mirror declaration in kernel */
  max_tick_bias = nominal_tick / 10;

  /* We can't reliably detect the internal kernel HZ, it may not even be fixed
     (CONFIG_NO_HZ aka tickless), assume the lowest commonly used fixed rate */
  tick_update_hz = 100;

  if (uname(&uts) < 0) {
    LOG_FATAL(LOGF_SysLinux, "Cannot uname(2) to get kernel version, sorry.");
  }

  patch = 0;
  if (sscanf(uts.release, "%d.%d.%d", &major, &minor, &patch) < 2) {
    LOG_FATAL(LOGF_SysLinux, "Cannot read information from uname, sorry");
  }

  DEBUG_LOG(LOGF_SysLinux, "Linux kernel major=%d minor=%d patch=%d", major, minor, patch);

  if (kernelvercmp(major, minor, patch, 2, 2, 0) < 0) {
    LOG_FATAL(LOGF_SysLinux, "Kernel version not supported, sorry.");
  }

  if (kernelvercmp(major, minor, patch, 2, 6, 27) >= 0 &&
      kernelvercmp(major, minor, patch, 2, 6, 33) < 0) {
    /* Tickless kernels before 2.6.33 accumulated ticks only in
       half-second intervals */
    tick_update_hz = 2;
  }

  /* ADJ_SETOFFSET support */
  if (kernelvercmp(major, minor, patch, 2, 6, 39) < 0) {
    have_setoffset = 0;
  } else {
    have_setoffset = 1;
  }

  DEBUG_LOG(LOGF_SysLinux, "hz=%d nominal_tick=%d max_tick_bias=%d",
      hz, nominal_tick, max_tick_bias);
}

/* ================================================== */
/* Initialisation code for this module */

void
SYS_Linux_Initialise(void)
{
  get_version_specific_details();

  if (TMX_ResetOffset() < 0) {
    LOG_FATAL(LOGF_SysLinux, "adjtimex() failed");
  }

  if (have_setoffset && TMX_TestStepOffset() < 0) {
    LOG(LOGS_INFO, LOGF_SysLinux, "adjtimex() doesn't support ADJ_SETOFFSET");
    have_setoffset = 0;
  }

  TMX_SetSync(CNF_GetRtcSync());

  SYS_Generic_CompleteFreqDriver(1.0e6 * max_tick_bias / nominal_tick,
                                 1.0 / tick_update_hz,
                                 read_frequency, set_frequency,
                                 have_setoffset ? apply_step_offset : NULL,
                                 set_leap, NULL);
}

/* ================================================== */
/* Finalisation code for this module */

void
SYS_Linux_Finalise(void)
{
  SYS_Generic_Finalise();
}

/* ================================================== */

#ifdef FEAT_PRIVDROP
void
SYS_Linux_DropRoot(char *user)
{
  struct passwd *pw;
  cap_t cap;

  if (user == NULL)
    return;

  if ((pw = getpwnam(user)) == NULL) {
    LOG_FATAL(LOGF_SysLinux, "getpwnam(%s) failed", user);
  }

  if (prctl(PR_SET_KEEPCAPS, 1)) {
    LOG_FATAL(LOGF_SysLinux, "prctl() failed");
  }
  
  if (setgroups(0, NULL)) {
    LOG_FATAL(LOGF_SysLinux, "setgroups() failed");
  }

  if (setgid(pw->pw_gid)) {
    LOG_FATAL(LOGF_SysLinux, "setgid(%d) failed", pw->pw_gid);
  }

  if (setuid(pw->pw_uid)) {
    LOG_FATAL(LOGF_SysLinux, "setuid(%d) failed", pw->pw_uid);
  }

  if ((cap = cap_from_text("cap_sys_time=ep")) == NULL) {
    LOG_FATAL(LOGF_SysLinux, "cap_from_text() failed");
  }

  if (cap_set_proc(cap)) {
    LOG_FATAL(LOGF_SysLinux, "cap_set_proc() failed");
  }

  cap_free(cap);

  DEBUG_LOG(LOGF_SysLinux, "Privileges dropped to user %s", user);
}
#endif

/* ================================================== */

#if defined(HAVE_SCHED_SETSCHEDULER)
  /* Install SCHED_FIFO real-time scheduler with specified priority */
void SYS_Linux_SetScheduler(int SchedPriority)
{
  int pmax, pmin;
  struct sched_param sched;

  if (SchedPriority < 1 || SchedPriority > 99) {
    LOG_FATAL(LOGF_SysLinux, "Bad scheduler priority: %d", SchedPriority);
  } else {
    sched.sched_priority = SchedPriority;
    pmax = sched_get_priority_max(SCHED_FIFO);
    pmin = sched_get_priority_min(SCHED_FIFO);
    if ( SchedPriority > pmax ) {
      sched.sched_priority = pmax;
    }
    else if ( SchedPriority < pmin ) {
      sched.sched_priority = pmin;
    }
    if ( sched_setscheduler(0, SCHED_FIFO, &sched) == -1 ) {
      LOG(LOGS_ERR, LOGF_SysLinux, "sched_setscheduler() failed");
    }
    else {
      DEBUG_LOG(LOGF_SysLinux, "Enabled SCHED_FIFO with priority %d",
          sched.sched_priority);
    }
  }
}
#endif /* HAVE_SCHED_SETSCHEDULER */

#if defined(HAVE_MLOCKALL)
/* Lock the process into RAM so that it will never be swapped out */ 
void SYS_Linux_MemLockAll(int LockAll)
{
  struct rlimit rlim;
  if (LockAll == 1 ) {
    /* Make sure that we will be able to lock all the memory we need */
    /* even after dropping privileges.  This does not actually reaerve any memory */
    rlim.rlim_max = RLIM_INFINITY;
    rlim.rlim_cur = RLIM_INFINITY;
    if (setrlimit(RLIMIT_MEMLOCK, &rlim) < 0) {
      LOG(LOGS_ERR, LOGF_SysLinux, "setrlimit() failed: not locking into RAM");
    }
    else {
      if (mlockall(MCL_CURRENT|MCL_FUTURE) < 0) {
	LOG(LOGS_ERR, LOGF_SysLinux, "mlockall() failed");
      }
      else {
	DEBUG_LOG(LOGF_SysLinux, "Successfully locked into RAM");
      }
    }
  }
}
#endif /* HAVE_MLOCKALL */
