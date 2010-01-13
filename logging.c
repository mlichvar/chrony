/*
  $Header: /cvs/src/chrony/logging.c,v 1.15 2003/09/22 21:22:30 richard Exp $

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

  Module to handle logging of diagnostic information
  */

#include "sysincl.h"

#include "main.h"
#include "logging.h"
#include "version.h"

/* ================================================== */
/* Flag indicating we have initialised */
static int initialised = 0;

static int is_detached = 0;

static time_t last_limited = 0;

#ifdef WINNT
static FILE *logfile;
#endif

/* ================================================== */
/* Init function */

void
LOG_Initialise(void)
{
  initialised = 1;

#ifdef WINNT
  logfile = fopen("./chronyd.err", "a");
#endif

  return;
}

/* ================================================== */
/* Fini function */

void
LOG_Finalise(void)
{
#ifdef WINNT
  if (logfile) {
    fclose(logfile);
  }
#else
  if (is_detached) {
    closelog();
  }
#endif

  initialised = 0;
  return;
}

/* ================================================== */

void
LOG_Line_Function(LOG_Severity severity, LOG_Facility facility, const char *format, ...)
{
  char buf[2048];
  va_list other_args;
  va_start(other_args, format);
  vsnprintf(buf, sizeof(buf), format, other_args);
  va_end(other_args);
#ifdef WINNT
  if (logfile) {
    fprintf(logfile, "%s\n", buf);
  }
#else
  if (is_detached) {
    switch (severity) {
      case LOGS_INFO:
        syslog(LOG_INFO, "%s", buf);
        break;
      case LOGS_WARN:
        syslog(LOG_WARNING, "%s", buf);
        break;
      case LOGS_ERR:
      default:
        syslog(LOG_ERR, "%s", buf);
        break;
    }
  } else {
    fprintf(stderr, "%s\n", buf);
  }
#endif
  return;
}

/* ================================================== */

void
LOG_Fatal_Function(LOG_Facility facility, const char *format, ...)
{
  char buf[2048];
  va_list other_args;
  va_start(other_args, format);
  vsnprintf(buf, sizeof(buf), format, other_args);
  va_end(other_args);

#ifdef WINNT
  if (logfile) {
    fprintf(logfile, "Fatal error : %s\n", buf);
  }
#else
  if (is_detached) {
    syslog(LOG_CRIT, "Fatal error : %s", buf);
  } else {
    fprintf(stderr, "Fatal error : %s\n", buf);
  }
#endif

  MAI_CleanupAndExit();

  return;
}

/* ================================================== */

void
LOG_Position(const char *filename, int line_number, const char *function_name)
{
#ifdef WINNT
#else
  time_t t;
  struct tm stm;
  char buf[64];
  if (!is_detached) {
    /* Don't clutter up syslog with internal debugging info */
    time(&t);
    stm = *gmtime(&t);
    strftime(buf, sizeof(buf), "%d-%H:%M:%S", &stm);
    fprintf(stderr, "%s:%d:(%s)[%s] ", filename, line_number, function_name, buf);
  }
#endif
  return;
}

/* ================================================== */

void
LOG_GoDaemon(void)
{
#ifdef WINNT


#else

  int pid, fd;

  /* Does this preserve existing signal handlers? */
  pid = fork();

  if (pid < 0) {
    LOG(LOGS_ERR, LOGF_Logging, "Could not detach, fork failed : %s", strerror(errno));
  } else if (pid > 0) {
    exit(0); /* In the 'grandparent' */
  } else {

    setsid();

    /* Do 2nd fork, as-per recommended practice for launching daemons. */
    pid = fork();

    if (pid < 0) {
      LOG(LOGS_ERR, LOGF_Logging, "Could not detach, fork failed : %s", strerror(errno));
    } else if (pid > 0) {
      exit(0); /* In the 'parent' */
    } else {
      /* In the child we want to leave running as the daemon */

      /* Don't keep stdin/out/err from before. */
      for (fd=0; fd<1024; fd++) {
        close(fd);
      }

      is_detached = 1;

      openlog("chronyd", LOG_PID, LOG_DAEMON);

      LOG(LOGS_INFO, LOGF_Logging, "chronyd version %s starting", PROGRAM_VERSION_STRING);

    }
  }

#endif
}

/* ================================================== */

int
LOG_RateLimited(void)
{
  time_t now;

  now = time(NULL);

  if (last_limited + 10 > now && last_limited <= now)
    return 1;

  last_limited = now;
  return 0;
}

/* ================================================== */
