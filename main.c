/*
  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Richard P. Curnow  1997-2003
 * Copyright (C) John G. Hasler  2009
 * Copyright (C) Miroslav Lichvar  2012-2014
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
#include "manual.h"
#include "rtc.h"
#include "refclock.h"
#include "clientlog.h"
#include "nameserv.h"
#include "smooth.h"
#include "tempcomp.h"
#include "util.h"

/* ================================================== */

/* Set when the initialisation chain has been completed.  Prevents finalisation
 * chain being run if a fatal error happened early. */

static int initialised = 0;

static int exit_status = 0;

static int reload = 0;

static REF_Mode ref_mode = REF_ModeNormal;

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
  if (!initialised) exit(exit_status);
  
  if (CNF_GetDumpOnExit()) {
    SRC_DumpSources();
  }

  /* Don't update clock when removing sources */
  REF_SetMode(REF_ModeIgnore);

  SMT_Finalise();
  TMC_Finalise();
  MNL_Finalise();
  CLG_Finalise();
  NSR_Finalise();
  NCR_Finalise();
  CAM_Finalise();
  NIO_Finalise();
  SST_Finalise();
  KEY_Finalise();
  RCL_Finalise();
  SRC_Finalise();
  REF_Finalise();
  RTC_Finalise();
  SYS_Finalise();
  SCH_Finalise();
  LCL_Finalise();

  delete_pidfile();
  
  CNF_Finalise();
  LOG_Finalise();

  HSH_Finalise();

  exit(exit_status);
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
ntp_source_resolving_end(void)
{
  NSR_SetSourceResolvingEndHandler(NULL);

  if (reload) {
    /* Note, we want reload to come well after the initialisation from
       the real time clock - this gives us a fighting chance that the
       system-clock scale for the reloaded samples still has a
       semblence of validity about it. */
    SRC_ReloadSources();
  }

  RTC_StartMeasurements();
  RCL_StartRefclocks();
  NSR_StartSources();
  NSR_AutoStartSources();

  /* Special modes can end only when sources update their reachability.
     Give up immediatelly if there are no active sources. */
  if (ref_mode != REF_ModeNormal && !SRC_ActiveSources()) {
    REF_SetUnsynchronised();
  }
}

/* ================================================== */

static void
post_init_ntp_hook(void *anything)
{
  if (ref_mode == REF_ModeInitStepSlew) {
    /* Remove the initstepslew sources and set normal mode */
    NSR_RemoveAllSources();
    ref_mode = REF_ModeNormal;
    REF_SetMode(ref_mode);
  }

  /* Close the pipe to the foreground process so it can exit */
  LOG_CloseParentFd();

  CNF_AddSources();
  CNF_AddBroadcasts();

  NSR_SetSourceResolvingEndHandler(ntp_source_resolving_end);
  NSR_ResolveSources();
}

/* ================================================== */

static void
reference_mode_end(int result)
{
  switch (ref_mode) {
    case REF_ModeNormal:
    case REF_ModeUpdateOnce:
    case REF_ModePrintOnce:
      exit_status = !result;
      SCH_QuitProgram();
      break;
    case REF_ModeInitStepSlew:
      /* Switch to the normal mode, the delay is used to prevent polling
         interval shorter than the burst interval if some configured servers
         were used also for initstepslew */
      SCH_AddTimeoutByDelay(2.0, post_init_ntp_hook, NULL);
      break;
    default:
      assert(0);
  }
}

/* ================================================== */

static void
post_init_rtc_hook(void *anything)
{
  if (CNF_GetInitSources() > 0) {
    CNF_AddInitSources();
    NSR_StartSources();
    assert(REF_GetMode() != REF_ModeNormal);
    /* Wait for mode end notification */
  } else {
    (post_init_ntp_hook)(NULL);
  }
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
    LOG_FATAL(LOGF_Main, "could not open lockfile %s for writing", pidfile);
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
    LOG_FATAL(LOGF_Logging, "Could not detach, pipe failed : %s", strerror(errno));
  }

  /* Does this preserve existing signal handlers? */
  pid = fork();

  if (pid < 0) {
    LOG_FATAL(LOGF_Logging, "Could not detach, fork failed : %s", strerror(errno));
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
      LOG_FATAL(LOGF_Logging, "Could not detach, fork failed : %s", strerror(errno));
    } else if (pid > 0) {
      exit(0); /* In the 'parent' */
    } else {
      /* In the child we want to leave running as the daemon */

      /* Change current directory to / */
      if (chdir("/") < 0) {
        LOG_FATAL(LOGF_Logging, "Could not chdir to / : %s", strerror(errno));
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
  const char *conf_file = DEFAULT_CONF_FILE;
  const char *progname = argv[0];
  char *user = NULL;
  struct passwd *pw;
  int debug = 0, nofork = 0, address_family = IPADDR_UNSPEC;
  int do_init_rtc = 0, restarted = 0;
  int other_pid;
  int lock_memory = 0, sched_priority = 0;
  int system_log = 1;
  int config_args = 0;

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
      printf("chronyd (chrony) version %s (%s)\n", CHRONY_VERSION, CHRONYD_FEATURES);
      return 0;
    } else if (!strcmp("-n", *argv)) {
      nofork = 1;
    } else if (!strcmp("-d", *argv)) {
      debug++;
      nofork = 1;
      system_log = 0;
    } else if (!strcmp("-q", *argv)) {
      ref_mode = REF_ModeUpdateOnce;
      nofork = 1;
      system_log = 0;
    } else if (!strcmp("-Q", *argv)) {
      ref_mode = REF_ModePrintOnce;
      nofork = 1;
      system_log = 0;
    } else if (!strcmp("-4", *argv)) {
      address_family = IPADDR_INET4;
    } else if (!strcmp("-6", *argv)) {
      address_family = IPADDR_INET6;
    } else if (!strcmp("-h", *argv) || !strcmp("--help", *argv)) {
      printf("Usage: %s [-4|-6] [-n|-d] [-q|-Q] [-r] [-R] [-s] [-f FILE|COMMAND...]\n",
             progname);
      return 0;
    } else if (*argv[0] == '-') {
      LOG_FATAL(LOGF_Main, "Unrecognized command line option [%s]", *argv);
    } else {
      /* Process remaining arguments and configuration lines */
      config_args = argc;
      break;
    }
  }

  if (getuid() != 0) {
    /* This write to the terminal is OK, it comes before we turn into a daemon */
    fprintf(stderr,"Not superuser\n");
    return 1;
  }

  /* Turn into a daemon */
  if (!nofork) {
    go_daemon();
  }

  if (system_log) {
    LOG_OpenSystemLog();
  }
  
  LOG_SetDebugLevel(debug);
  
  LOG(LOGS_INFO, LOGF_Main, "chronyd version %s starting (%s)",
      CHRONY_VERSION, CHRONYD_FEATURES);

  DNS_SetAddressFamily(address_family);

  CNF_Initialise(restarted);

  /* Parse the config file or the remaining command line arguments */
  if (!config_args) {
    CNF_ReadFile(conf_file);
  } else {
    do {
      CNF_ParseLine(NULL, config_args - argc + 1, *argv);
    } while (++argv, --argc);
  }

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

  LCL_Initialise();
  SCH_Initialise();
  SYS_Initialise();
  RTC_Initialise(do_init_rtc);
  SRC_Initialise();
  RCL_Initialise();
  KEY_Initialise();

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

  if (!user) {
    user = CNF_GetUser();
  }

  if ((pw = getpwnam(user)) == NULL)
    LOG_FATAL(LOGF_Main, "Could not get %s uid/gid", user);

  /* Create all directories before dropping root */
  CNF_CreateDirs(pw->pw_uid, pw->pw_gid);

  /* Drop root privileges if the user has non-zero uid or gid */
  if (pw->pw_uid || pw->pw_gid)
    SYS_DropRoot(pw->pw_uid, pw->pw_gid);

  REF_Initialise();
  SST_Initialise();
  NIO_Initialise(address_family);
  CAM_Initialise(address_family);
  NCR_Initialise();
  NSR_Initialise();
  CLG_Initialise();
  MNL_Initialise();
  TMC_Initialise();
  SMT_Initialise();

  /* From now on, it is safe to do finalisation on exit */
  initialised = 1;

  CNF_SetupAccessRestrictions();

  if (ref_mode == REF_ModeNormal && CNF_GetInitSources() > 0) {
    ref_mode = REF_ModeInitStepSlew;
  }

  REF_SetModeEndHandler(reference_mode_end);
  REF_SetMode(ref_mode);

  if (do_init_rtc) {
    RTC_TimeInit(post_init_rtc_hook, NULL);
  } else {
    post_init_rtc_hook(NULL);
  }

  UTI_SetQuitSignalsHandler(signal_cleanup);

  /* The program normally runs under control of the main loop in
     the scheduler. */
  SCH_MainLoop();

  LOG(LOGS_INFO, LOGF_Main, "chronyd exiting");

  MAI_CleanupAndExit();

  return 0;
}

/* ================================================== */
