/*
  $Header: /cvs/src/chrony/reports.h,v 1.17 2002/02/28 23:27:13 richard Exp $

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

  Data structure definitions within the daemon for various reports that
  can be generated */

#ifndef GOT_REPORTS_H
#define GOT_REPORTS_H

#include "sysincl.h"
#include "addressing.h"

#define REPORT_INVALID_OFFSET 0x80000000

typedef struct {
  IPAddr ip_addr;
  int stratum;
  int poll;
  enum {RPT_NTP_CLIENT, RPT_NTP_PEER, RPT_LOCAL_REFERENCE} mode;
  enum {RPT_SYNC, RPT_UNREACH, RPT_FALSETICKER, RPT_JITTERY, RPT_OTHER} state;

  unsigned long latest_meas_ago; /* seconds */
  double orig_latest_meas; /* seconds */
  double latest_meas; /* seconds */
  double latest_meas_err; /* seconds */
  double est_offset; /* seconds */
  double est_offset_err; /* seconds */
} RPT_SourceReport ;

typedef struct {
  unsigned long ref_id;
  IPAddr ip_addr;
  unsigned long stratum;
  struct timeval ref_time;
  double current_correction;
  double freq_ppm;
  double resid_freq_ppm;
  double skew_ppm;
  double root_delay;
  double root_dispersion;
} RPT_TrackingReport;

typedef struct {
  unsigned long ref_id;
  IPAddr ip_addr;
  unsigned long n_samples;
  unsigned long n_runs;
  unsigned long span_seconds;
  double resid_freq_ppm;
  double skew_ppm;
  double sd;
} RPT_SourcestatsReport;

typedef struct {
  struct timeval ref_time;
  unsigned short n_samples;
  unsigned short n_runs;
  unsigned long span_seconds;
  double rtc_seconds_fast;
  double rtc_gain_rate_ppm;
} RPT_RTC_Report;

typedef struct {
  unsigned long client_hits;
  unsigned long peer_hits;
  unsigned long cmd_hits_auth;
  unsigned long cmd_hits_normal;
  unsigned long cmd_hits_bad;
  unsigned long last_ntp_hit_ago;
  unsigned long last_cmd_hit_ago;
} RPT_ClientAccess_Report;

typedef struct {
  IPAddr ip_addr;
  unsigned long client_hits;
  unsigned long peer_hits;
  unsigned long cmd_hits_auth;
  unsigned long cmd_hits_normal;
  unsigned long cmd_hits_bad;
  unsigned long last_ntp_hit_ago;
  unsigned long last_cmd_hit_ago;
} RPT_ClientAccessByIndex_Report;

typedef struct {
  struct timeval when;
  double slewed_offset;
  double orig_offset;
  double residual;
} RPT_ManualSamplesReport;

typedef struct {
  int online;
  int offline;
  int burst_online;
  int burst_offline;
} RPT_ActivityReport;

#endif /* GOT_REPORTS_H */
