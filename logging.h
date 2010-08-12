/*
  $Header: /cvs/src/chrony/logging.h,v 1.15 2002/02/28 23:27:10 richard Exp $

  =======================================================================

  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Richard P. Curnow  1997-2002
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

  Header file for diagnostic logging module

  */

#ifndef GOT_LOGGING_H
#define GOT_LOGGING_H

/* Definition of severity */
typedef enum {
  LOGS_INFO,
  LOGS_WARN,
  LOGS_ERR
} LOG_Severity;

/* Definition of facility.  Each message is tagged with who generated
   it, so that the user can customise what level of reporting he gets
   for each area of the software */
typedef enum {
  LOGF_Reference,
  LOGF_NtpIO,
  LOGF_NtpCore,
  LOGF_NtpSources,
  LOGF_Scheduler,
  LOGF_SourceStats,
  LOGF_Sources,
  LOGF_Local,
  LOGF_Util,
  LOGF_Main,
  LOGF_ClientLog,
  LOGF_Configure,
  LOGF_CmdMon,
  LOGF_Acquire,
  LOGF_Manual,
  LOGF_Logging,
  LOGF_Rtc,
  LOGF_Regress,
  LOGF_Sys,
  LOGF_SysLinux,
  LOGF_SysNetBSD,
  LOGF_SysSolaris,
  LOGF_SysSunOS,
  LOGF_SysWinnt,
  LOGF_RtcLinux,
  LOGF_Refclock
} LOG_Facility;

/* Init function */
extern void LOG_Initialise(void);

/* Fini function */
extern void LOG_Finalise(void);

/* Line logging function */
extern void LOG_Line_Function(LOG_Severity severity, LOG_Facility facility, const char *format, ...);

/* Logging function for fatal errors */
extern void LOG_Fatal_Function(LOG_Facility facility, const char *format, ...);

/* Position in code reporting function */
extern void LOG_Position(const char *filename, int line_number, const char *function_name);

extern void LOG_GoDaemon(void);

/* Return zero once per 10 seconds */
extern int LOG_RateLimited(void);

/* Line logging macro.  If the compiler is GNU C, we take advantage of
   being able to get the function name also. */
#if defined(__GNUC__)
#define LOG LOG_Position(__FILE__, __LINE__, __FUNCTION__); LOG_Line_Function
#define LOG_FATAL LOG_Position(__FILE__, __LINE__, __FUNCTION__); LOG_Fatal_Function
#else
#define LOG LOG_Position(__FILE__, __LINE__, ""); LOG_Line_Function
#define LOG_FATAL LOG_Position(__FILE__, __LINE__, ""); LOG_Fatal_Function
#endif /* defined (__GNUC__) */

/* File logging functions */

typedef int LOG_FileID;

extern LOG_FileID LOG_FileOpen(const char *name, const char *banner);
extern void LOG_FileWrite(LOG_FileID id, const char *format, ...);

extern void LOG_CreateLogFileDir(void);
extern void LOG_CycleLogFiles(void);

#endif /* GOT_LOGGING_H */
