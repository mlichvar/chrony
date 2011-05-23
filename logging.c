/*
  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Richard P. Curnow  1997-2003
 * Copyright (C) Miroslav Lichvar  2011
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

  Module to handle logging of diagnostic information
  */

#include "config.h"

#include "sysincl.h"

#include "main.h"
#include "conf.h"
#include "logging.h"
#include "mkdirpp.h"
#include "util.h"

/* ================================================== */
/* Flag indicating we have initialised */
static int initialised = 0;

static int system_log = 0;

static time_t last_limited = 0;

#ifdef WINNT
static FILE *logfile;
#endif

struct LogFile {
  const char *name;
  const char *banner;
  FILE *file;
  unsigned long writes;
};

static int n_filelogs = 0;

/* Increase this when adding a new logfile */
#define MAX_FILELOGS 6

static struct LogFile logfiles[MAX_FILELOGS];

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
  if (system_log) {
    closelog();
  }
#endif

  LOG_CycleLogFiles();

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
  if (system_log) {
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
  if (system_log) {
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
  if (!system_log) {
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
LOG_OpenSystemLog(void)
{
#ifdef WINNT
#else
  system_log = 1;
  openlog("chronyd", LOG_PID, LOG_DAEMON);
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

LOG_FileID
LOG_FileOpen(const char *name, const char *banner)
{
  assert(n_filelogs < MAX_FILELOGS);

  logfiles[n_filelogs].name = name;
  logfiles[n_filelogs].banner = banner;
  logfiles[n_filelogs].file = NULL;
  logfiles[n_filelogs].writes = 0;

  return n_filelogs++;
}

/* ================================================== */

void
LOG_FileWrite(LOG_FileID id, const char *format, ...)
{
  va_list other_args;
  int banner;

  if (id < 0 || id >= n_filelogs || !logfiles[id].name)
    return;

  if (!logfiles[id].file) {
    char filename[512];

    if (snprintf(filename, sizeof(filename), "%s/%s.log",
          CNF_GetLogDir(), logfiles[id].name) >= sizeof(filename) ||
        !(logfiles[id].file = fopen(filename, "a"))) {
      LOG(LOGS_WARN, LOGF_Refclock, "Couldn't open logfile %s for update", filename);
      logfiles[id].name = NULL;
      return;
    }

    /* Close on exec */
    UTI_FdSetCloexec(fileno(logfiles[id].file));
  }

  banner = CNF_GetLogBanner();
  if (banner && logfiles[id].writes++ % banner == 0) {
    char bannerline[256];
    int i, bannerlen;

    bannerlen = strlen(logfiles[id].banner);

    for (i = 0; i < bannerlen; i++)
      bannerline[i] = '=';
    bannerline[i] = '\0';

    fprintf(logfiles[id].file, "%s\n", bannerline);
    fprintf(logfiles[id].file, "%s\n", logfiles[id].banner);
    fprintf(logfiles[id].file, "%s\n", bannerline);
  }

  va_start(other_args, format);
  vfprintf(logfiles[id].file, format, other_args);
  va_end(other_args);
  fprintf(logfiles[id].file, "\n");

  fflush(logfiles[id].file);
}

/* ================================================== */

void
LOG_CreateLogFileDir(void)
{
  const char *logdir;

  if (n_filelogs <= 0)
    return;

  logdir = CNF_GetLogDir();

  if (!mkdir_and_parents(logdir)) {
    LOG(LOGS_ERR, LOGF_Logging, "Could not create directory %s", logdir);
    n_filelogs = 0;
  }
}

/* ================================================== */

void
LOG_CycleLogFiles(void)
{
  LOG_FileID i;

  for (i = 0; i < n_filelogs; i++) {
    if (logfiles[i].file)
      fclose(logfiles[i].file);
    logfiles[i].file = NULL;
    logfiles[i].writes = 0;
  }
}

/* ================================================== */
