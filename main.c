/*
  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Richard P. Curnow  1997-2003
 * Copyright (C) John G. Hasler  2009
 * Copyright (C) Miroslav Lichvar  2012
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

#include "config.h"

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
#include "rtc.h"
#include "refclock.h"
#include "clientlog.h"
#include "broadcast.h"
#include "nameserv.h"
#include "tempcomp.h"

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

  TMC_Finalise();
  MNL_Finalise();
  ACQ_Finalise();
  KEY_Finalise();
  CLG_Finalise();
  NSR_Finalise();
  NCR_Finalise();
  BRD_Finalise();
  SST_Finalise();
  REF_Finalise();
  RCL_Finalise();
  SRC_Finalise();
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
  if (!initialised) exit(0);
  SCH_QuitProgram();
}

/* ================================================== */

static void
post_acquire_hook(void *anything)
{
  /* Close the pipe to the foreground process so it can exit */
  LOG_CloseParentFd();

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

static void
go_daemon(void)
{
#ifdef WINNT


#else

  int pid, fd, pipefd[2];

  /* Create pipe which will the daemon use to notify the grandparent
     when it's initialised or send an error message */
  if (pipe(pipefd)) {
    LOG(LOGS_ERR, LOGF_Logging, "Could not detach, pipe failed : %s", strerror(errno));
  }

  /* Does this preserve existing signal handlers? */
  pid = fork();

  if (pid < 0) {
    LOG(LOGS_ERR, LOGF_Logging, "Could not detach, fork failed : %s", strerror(errno));
  } else if (pid > 0) {
    /* In the 'grandparent' */
    char message[1024];
    int r;

    close(pipefd[1]);
    r = read(pipefd[0], message, sizeof (message));
    if (r) {
      if (r > 0) {
        /* Print the error message from the child */
        fprintf(stderr, "%.1024s\n", message);
      }
      exit(1);
    } else
      exit(0);
  } else {
    close(pipefd[0]);

    setsid();

    /* Do 2nd fork, as-per recommended practice for launching daemons. */
    pid = fork();

    if (pid < 0) {
      LOG(LOGS_ERR, LOGF_Logging, "Could not detach, fork failed : %s", strerror(errno));
    } else if (pid > 0) {
      exit(0); /* In the 'parent' */
    } else {
      /* In the child we want to leave running as the daemon */

      /* Change current directory to / */
      if (chdir("/") < 0) {
        LOG(LOGS_ERR, LOGF_Logging, "Could not chdir to / : %s", strerror(errno));
      }

      /* Don't keep stdin/out/err from before. But don't close
         the parent pipe yet. */
      for (fd=0; fd<1024; fd++) {
        if (fd != pipefd[1])
          close(fd);
      }

      LOG_SetParentFd(pipefd[1]);
    }
  }

#endif
}

/* ================================================== */

int main
(int argc, char **argv)
{
  char *conf_file = NULL;
  char *user = NULL;
  int debug = 0, nofork = 0;
  int do_init_rtc = 0, restarted = 0;
  int other_pid;
  int lock_memory = 0, sched_priority = 0;

  LOG_Initialise();

  /* Parse command line options */
  while (++argv, (--argc)>0) {

    if (!strcmp("-f", *argv)) {
      ++argv, --argc;
      conf_file = *argv;
    } else if (!strcmp("-P", *argv)) {
      ++argv, --argc;
      if (argc == 0 || sscanf(*argv, "%d", &sched_priority) != 1) {
        LOG_FATAL(LOGF_Main, "Bad scheduler priority");
      }
    } else if (!strcmp("-m", *argv)) {
      lock_memory = 1;
    } else if (!strcmp("-r", *argv)) {
      reload = 1;
    } else if (!strcmp("-R", *argv)) {
      restarted = 1;
    } else if (!strcmp("-u", *argv)) {
      ++argv, --argc;
      if (argc == 0) {
        LOG_FATAL(LOGF_Main, "Missing user name");
      } else {
        user = *argv;
      }
    } else if (!strcmp("-s", *argv)) {
      do_init_rtc = 1;
    } else if (!strcmp("-v", *argv) || !strcmp("--version",*argv)) {
      /* This write to the terminal is OK, it comes before we turn into a daemon */
      printf("chronyd (chrony) version %s\n", CHRONY_VERSION);
      exit(0);
    } else if (!strcmp("-n", *argv)) {
      nofork = 1;
    } else if (!strcmp("-d", *argv)) {
      debug = 1;
      nofork = 1;
    } else if (!strcmp("-4", *argv)) {
      DNS_SetAddressFamily(IPADDR_INET4);
    } else if (!strcmp("-6", *argv)) {
      DNS_SetAddressFamily(IPADDR_INET6);
    } else {
      LOG_FATAL(LOGF_Main, "Unrecognized command line option [%s]", *argv);
    }
  }

  if (getuid() != 0) {
    /* This write to the terminal is OK, it comes before we turn into a daemon */
    fprintf(stderr,"Not superuser\n");
    exit(1);
  }

  /* Turn into a daemon */
  if (!nofork) {
    go_daemon();
  }

  if (!debug) {
    LOG_OpenSystemLog();
  }
  
  LOG(LOGS_INFO, LOGF_Main, "chronyd version %s starting", CHRONY_VERSION);

  CNF_SetRestarted(restarted);
  CNF_ReadFile(conf_file);

  /* Check whether another chronyd may already be running.  Do this after
   * forking, so that message logging goes to the right place (i.e. syslog), in
   * case this chronyd is being run from a boot script. */
  if (maybe_another_chronyd_running(&other_pid)) {
    LOG_FATAL(LOGF_Main, "Another chronyd may already be running (pid=%d), check lockfile (%s)",
              other_pid, CNF_GetPidFile());
  }

  /* Write our lockfile to prevent other chronyds running.  This has *GOT* to
   * be done *AFTER* the daemon-creation fork() */
  write_lockfile();

  if (do_init_rtc) {
    RTC_TimePreInit();
  }

  LCL_Initialise();
  SCH_Initialise();
  SYS_Initialise();
  NIO_Initialise();
  CAM_Initialise();
  RTC_Initialise();
  SRC_Initialise();
  RCL_Initialise();

  /* Command-line switch must have priority */
  if (!sched_priority) {
    sched_priority = CNF_GetSchedPriority();
  }
  if (sched_priority) {
    SYS_SetScheduler(sched_priority);
  }

  if (lock_memory || CNF_GetLockMemory()) {
    SYS_LockMemory();
  }

  if (user) {
    SYS_DropRoot(user);
  }

  LOG_CreateLogFileDir();

  REF_Initialise();
  SST_Initialise();
  BRD_Initialise();
  NCR_Initialise();
  NSR_Initialise();
  CLG_Initialise();
  KEY_Initialise();
  ACQ_Initialise();
  MNL_Initialise();
  TMC_Initialise();

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

  LOG(LOGS_INFO, LOGF_Main, "chronyd exiting");

  MAI_CleanupAndExit();

  return 0;
}

/* ================================================== */
