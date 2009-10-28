/*
  $Header: /cvs/src/chrony/main.c,v 1.31 2003/09/22 21:22:30 richard Exp $

  =======================================================================

  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Richard P. Curnow  1997-2003
 * Copyright (C) John G. Hasler  2009
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

  The main program
  */

#include "sysincl.h"

#include "main.h"
#include "sched.h"
#include "local.h"
#include "sys.h"
#include "ntp_io.h"
#include "ntp_sources.h"
#include "ntp_core.h"
#include "sources.h"
#include "sourcestats.h"
#include "reference.h"
#include "logging.h"
#include "conf.h"
#include "cmdmon.h"
#include "keys.h"
#include "acquire.h"
#include "manual.h"
#include "version.h"
#include "rtc.h"
#include "refclock.h"
#include "clientlog.h"
#include "broadcast.h"
#include "nameserv.h"

/* ================================================== */

/* Set when the initialisation chain has been completed.  Prevents finalisation
 * chain being run if a fatal error happened early. */

static int initialised = 0;

/* ================================================== */

static int reload = 0;

/* ================================================== */

static void
delete_pidfile(void)
{
  const char *pidfile = CNF_GetPidFile();
  /* Don't care if this fails, there's not a lot we can do */
  unlink(pidfile);
}

/* ================================================== */

void
MAI_CleanupAndExit(void)
{
  if (!initialised) exit(0);
  
  if (CNF_GetDumpOnExit()) {
    SRC_DumpSources();
  }

  MNL_Finalise();
  ACQ_Finalise();
  KEY_Finalise();
  CLG_Finalise();
  NSR_Finalise();
  NCR_Finalise();
  BRD_Finalise();
  SRC_Finalise();
  SST_Finalise();
  REF_Finalise();
  RCL_Finalise();
  RTC_Finalise();
  CAM_Finalise();
  NIO_Finalise();
  SYS_Finalise();
  SCH_Finalise();
  LCL_Finalise();

  delete_pidfile();
  
  LOG_Finalise();

  exit(0);
}

/* ================================================== */

static void
signal_cleanup(int x)
{
  LOG(LOGS_WARN, LOGF_Main, "chronyd exiting on signal");
  if (!initialised) exit(0);
  SCH_QuitProgram();
}

/* ================================================== */

static void
post_acquire_hook(void *anything)
{

  CNF_AddSources();
  CNF_AddBroadcasts();
  if (reload) {
    /* Note, we want reload to come well after the initialisation from
       the real time clock - this gives us a fighting chance that the
       system-clock scale for the reloaded samples still has a
       semblence of validity about it. */
    SRC_ReloadSources();
  }
  CNF_SetupAccessRestrictions();

  RTC_StartMeasurements();
  RCL_StartRefclocks();
}

/* ================================================== */

static void
post_init_rtc_hook(void *anything)
{
  CNF_ProcessInitStepSlew(post_acquire_hook, NULL);
}

/* ================================================== */
/* Return 1 if the process exists on the system. */

static int
does_process_exist(int pid)
{
  int status;
  status = getsid(pid);
  if (status >= 0) {
    return 1;
  } else {
    return 0;
  }
}

/* ================================================== */

static int
maybe_another_chronyd_running(int *other_pid)
{
  const char *pidfile = CNF_GetPidFile();
  FILE *in;
  int pid, count;
  
  *other_pid = 0;

  in = fopen(pidfile, "r");
  if (!in) return 0;

  count = fscanf(in, "%d", &pid);
  fclose(in);
  
  if (count != 1) return 0;

  *other_pid = pid;
  return does_process_exist(pid);
  
}

/* ================================================== */

static void
write_lockfile(void)
{
  const char *pidfile = CNF_GetPidFile();
  FILE *out;

  out = fopen(pidfile, "w");
  if (!out) {
    LOG(LOGS_ERR, LOGF_Main, "could not open lockfile %s for writing", pidfile);
  } else {
    fprintf(out, "%d\n", getpid());
    fclose(out);
  }
}

/* ================================================== */

int main
(int argc, char **argv)
{
  char *conf_file = NULL;
  char *user = NULL;
  int debug = 0;
  int do_init_rtc = 0;
  int other_pid;

#if defined(HAVE_SCHED_SETSCHEDULER)
  int return_value = 0;
#endif

  LOG_Initialise();

  /* Parse command line options */
  while (++argv, (--argc)>0) {

    if (!strcmp("-f", *argv)) {
      ++argv, --argc;
      conf_file = *argv;

#if defined(HAVE_SCHED_SETSCHEDULER)
      /* Get real-time scheduler priority */
    } else if (!strcmp("-P", *argv)) {
      ++argv, --argc;
      return_value = sscanf(*argv, "%d", &SchedPriority);
      if (return_value != 1 || SchedPriority < 1 || SchedPriority > 99) { 
	SchedPriority = 0;
	LOG(LOGS_WARN, LOGF_Main, "Bad scheduler priority: [%s]", *argv);
      }
#endif /* HAVE_SCHED_SETCHEDULER */

#if defined(HAVE_MLOCKALL)
      /* Detect lockall switch */
    } else if (!strcmp("-m", *argv)) {
      LockAll = 1;
#endif /* HAVE_MLOCKALL */

    } else if (!strcmp("-r", *argv)) {
      reload = 1;
    } else if (!strcmp("-u", *argv)) {
      ++argv, --argc;
      user = *argv;
    } else if (!strcmp("-s", *argv)) {
      do_init_rtc = 1;
    } else if (!strcmp("-v", *argv) || !strcmp("--version",*argv)) {
      /* This write to the terminal is OK, it comes before we turn into a daemon */
      printf("chronyd (chrony) version %s\n", PROGRAM_VERSION_STRING);
      exit(0);
    } else if (!strcmp("-d", *argv)) {
      debug = 1;
    } else if (!strcmp("-4", *argv)) {
      DNS_SetAddressFamily(IPADDR_INET4);
    } else if (!strcmp("-6", *argv)) {
      DNS_SetAddressFamily(IPADDR_INET6);
    } else {
      LOG(LOGS_WARN, LOGF_Main, "Unrecognized command line option [%s]", *argv);
    }
  }

#ifndef SYS_WINNT
  if (getuid() != 0) {
    /* This write to the terminal is OK, it comes before we turn into a daemon */
    fprintf(stderr,"Not superuser\n");
    exit(1);
  }


  /* Turn into a daemon */
  if (!debug) {
    LOG_GoDaemon();
  }
  
  /* Check whether another chronyd may already be running.  Do this after
   * forking, so that message logging goes to the right place (i.e. syslog), in
   * case this chronyd is being run from a boot script. */
  if (maybe_another_chronyd_running(&other_pid)) {
    LOG_FATAL(LOGF_Main, "Another chronyd may already be running (pid=%d), check lockfile (%s)",
              other_pid, CNF_GetPidFile());
    exit(1);
  }

  /* Write our lockfile to prevent other chronyds running.  This has *GOT* to
   * be done *AFTER* the daemon-creation fork() */
  write_lockfile();
#endif

  CNF_ReadFile(conf_file);

  if (do_init_rtc) {
    RTC_TimePreInit();
  }

  LCL_Initialise();
  SCH_Initialise();
  SYS_Initialise();
  NIO_Initialise();
  CAM_Initialise();
  RTC_Initialise();
  RCL_Initialise();

  if (SchedPriority > 0) {
    SYS_SetScheduler(SchedPriority);
  }

  if (LockAll == 1 ) {
    SYS_MemLockAll(LockAll);
  }

  if (user)
    SYS_DropRoot(user);

  REF_Initialise();
  SST_Initialise();
  SRC_Initialise();
  BRD_Initialise();
  NCR_Initialise();
  NSR_Initialise();
  CLG_Initialise();
  KEY_Initialise();
  ACQ_Initialise();
  MNL_Initialise();

  /* From now on, it is safe to do finalisation on exit */
  initialised = 1;

  if (do_init_rtc) {
    RTC_TimeInit(post_init_rtc_hook, NULL);
  } else {
    post_init_rtc_hook(NULL);
  }

  signal(SIGINT, signal_cleanup);
  signal(SIGTERM, signal_cleanup);
#if !defined(WINNT)
  signal(SIGQUIT, signal_cleanup);
  signal(SIGHUP, signal_cleanup);
#endif /* WINNT */

  /* The program normally runs under control of the main loop in
     the scheduler. */
  SCH_MainLoop();

  MAI_CleanupAndExit();

  return 0;
}

/* ================================================== */
