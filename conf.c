/*
  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Richard P. Curnow  1997-2003
 * Copyright (C) Miroslav Lichvar  2009-2013
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

  Module that reads and processes the configuration file.
  */

#include "config.h"

#include "sysincl.h"

#include "conf.h"
#include "ntp_sources.h"
#include "ntp_core.h"
#include "refclock.h"
#include "cmdmon.h"
#include "srcparams.h"
#include "logging.h"
#include "nameserv.h"
#include "memory.h"
#include "acquire.h"
#include "cmdparse.h"
#include "broadcast.h"
#include "util.h"

/* ================================================== */
/* Forward prototypes */

static int parse_string(char *line, char **result);
static int parse_int(char *line, int *result);
static int parse_unsignedlong(char *, unsigned long *result);
static int parse_double(char *line, double *result);
static int parse_null(char *line);

static void parse_allow(char *);
static void parse_bindacqaddress(char *);
static void parse_bindaddress(char *);
static void parse_bindcmdaddress(char *);
static void parse_broadcast(char *);
static void parse_clientloglimit(char *);
static void parse_cmdallow(char *);
static void parse_cmddeny(char *);
static void parse_deny(char *);
static void parse_fallbackdrift(char *);
static void parse_include(char *);
static void parse_initstepslew(char *);
static void parse_local(char *);
static void parse_log(char *);
static void parse_mailonchange(char *);
static void parse_makestep(char *);
static void parse_maxchange(char *);
static void parse_peer(char *);
static void parse_refclock(char *);
static void parse_server(char *);
static void parse_tempcomp(char *);

/* ================================================== */
/* Configuration variables */

static int restarted = 0;
static int generate_command_key = 0;
static char *rtc_device = "/dev/rtc";
static int acquisition_port = 0; /* 0 means let kernel choose port */
static int ntp_port = 123;
static char *keys_file = NULL;
static char *drift_file = NULL;
static char *rtc_file = NULL;
static unsigned long command_key_id;
static double max_update_skew = 1000.0;
static double correction_time_ratio = 1.0;
static double max_clock_error = 1.0; /* in ppm */

static double reselect_distance = 1e-4;
static double stratum_weight = 1.0;
static double combine_limit = 3.0;

static int cmd_port = DEFAULT_CANDM_PORT;

static int do_log_measurements = 0;
static int do_log_statistics = 0;
static int do_log_tracking = 0;
static int do_log_rtc = 0;
static int do_log_refclocks = 0;
static int do_log_tempcomp = 0;
static int do_dump_on_exit = 0;
static int log_banner = 32;
static char *logdir = ".";
static char *dumpdir = ".";

static int enable_local=0;
static int local_stratum;

static int do_init_stepslew = 0;
static int n_init_srcs;

/* Threshold (in seconds) - if absolute value of initial error is less
   than this, slew instead of stepping */
static double init_slew_threshold;
#define MAX_INIT_SRCS 8
static IPAddr init_srcs_ip[MAX_INIT_SRCS];

static int enable_manual=0;

/* Flag set if the RTC runs UTC (default is it runs local time
   incl. daylight saving). */
static int rtc_on_utc = 0;

/* Filename used to read the hwclock(8) LOCAL/UTC setting */
static char *hwclock_file = NULL;

/* Flag set if the RTC should be automatically synchronised by kernel */
static int rtc_sync = 0;

/* Limit and threshold for clock stepping */
static int make_step_limit = 0;
static double make_step_threshold = 0.0;

/* Threshold for automatic RTC trimming */
static double rtc_autotrim_threshold = 0.0;

/* Number of updates before offset checking, number of ignored updates
   before exiting and the maximum allowed offset */
static int max_offset_delay = -1;
static int max_offset_ignore;
static double max_offset;

/* Maximum and minimum number of samples per source */
static int max_samples = 0; /* no limit */
static int min_samples = 0;

/* Flag set if we should log to syslog when a time adjustment
   exceeding the threshold is initiated */
static int do_log_change = 0;
static double log_change_threshold = 0.0;

static char *mail_user_on_change = NULL;
static double mail_change_threshold = 0.0;

/* Flag indicating that we don't want to log clients, e.g. to save
   memory */
static int no_client_log = 0;

/* Limit memory allocated for the clients log */
static unsigned long client_log_limit = 524288;

/* Minimum and maximum fallback drift intervals */
static int fb_drift_min = 0;
static int fb_drift_max = 0;

/* IP addresses for binding the NTP server sockets to.  UNSPEC family means
   INADDR_ANY will be used */
static IPAddr bind_address4, bind_address6;

/* IP addresses for binding the NTP client sockets to.  UNSPEC family means
   INADDR_ANY will be used */
static IPAddr bind_acq_address4, bind_acq_address6;

/* IP addresses for binding the command socket to.  UNSPEC family means
   use the value of bind_address */
static IPAddr bind_cmd_address4, bind_cmd_address6;

/* Filename to use for storing pid of running chronyd, to prevent multiple
 * chronyds being started. */
static char *pidfile = "/var/run/chronyd.pid";

/* Temperature sensor, update interval and compensation coefficients */
static char *tempcomp_file = NULL;
static double tempcomp_interval;
static double tempcomp_T0, tempcomp_k0, tempcomp_k1, tempcomp_k2;

/* Boolean for whether the Linux HZ value has been overridden, and the
 * new value. */
static int set_linux_hz = 0;
static int linux_hz;

/* Boolean for whether the Linux frequency scaling value (i.e. the one that's
 * approx (1<<SHIFT_HZ)/HZ) has been overridden, and the new value. */
static int set_linux_freq_scale = 0;
static double linux_freq_scale;

static int sched_priority = 0;
static int lock_memory = 0;

/* Name of a system timezone containing leap seconds occuring at midnight */
static char *leapsec_tz = NULL;

/* Name of the user to which will be dropped root privileges. */
static char *user = NULL;

typedef struct {
  NTP_Source_Type type;
  CPS_NTP_Source params;
} NTP_Source;

#define MAX_NTP_SOURCES 64

static NTP_Source ntp_sources[MAX_NTP_SOURCES];
static int n_ntp_sources = 0;

#define MAX_RCL_SOURCES 8

static RefclockParameters refclock_sources[MAX_RCL_SOURCES];
static int n_refclock_sources = 0;

typedef struct _AllowDeny {
  struct _AllowDeny *next;
  struct _AllowDeny *prev;
  IPAddr ip;
  int subnet_bits;
  int all; /* 1 to override existing more specific defns */
  int allow; /* 0 for deny, 1 for allow */
} AllowDeny;

static AllowDeny ntp_auth_list = {&ntp_auth_list, &ntp_auth_list};
static AllowDeny cmd_auth_list = {&cmd_auth_list, &cmd_auth_list};

typedef struct {
  /* Both in host (not necessarily network) order */
  IPAddr addr;
  unsigned short port;
  int interval;
} NTP_Broadcast_Destination;

static NTP_Broadcast_Destination *broadcasts = NULL;
static int max_broadcasts = 0;
static int n_broadcasts = 0;

/* ================================================== */

/* The line number in the configuration file being processed */
static int line_number;
static const char *processed_file;
static const char *processed_command;

/* ================================================== */

static void
command_parse_error(void)
{
    LOG_FATAL(LOGF_Configure, "Could not parse %s directive at line %d in file %s",
        processed_command, line_number, processed_file);
}

/* ================================================== */

static void
other_parse_error(const char *message)
{
    LOG_FATAL(LOGF_Configure, "%s at line %d in file %s",
        message, line_number, processed_file);
}

/* ================================================== */

static void
check_number_of_args(char *line, int num)
{
  /* The line is normalized, between arguments is just one space */
  if (*line == ' ')
    line++;
  if (*line)
    num--;
  for (; *line; line++) {
    if (*line == ' ')
      num--;
  }
  if (num) {
    LOG_FATAL(LOGF_Configure, "%s arguments for %s directive at line %d in file %s",
        num > 0 ? "Missing" : "Too many",
        processed_command, line_number, processed_file);
  }
}

/* ================================================== */

void
CNF_SetRestarted(int r)
{
  restarted = r;
}

/* ================================================== */

/* Read the configuration file */
void
CNF_ReadFile(const char *filename)
{
  FILE *in;
  char line[2048];
  char *p, *command;
  const char *prev_processed_file;
  int prev_line_number;

  in = fopen(filename, "r");
  if (!in) {
    LOG_FATAL(LOGF_Configure, "Could not open configuration file %s", filename);
  } else {
    /* Save current line number in case this is an included file */
    prev_line_number = line_number;
    prev_processed_file = processed_file;

    line_number = 0;
    processed_file = filename;

    /* Success */
    while (fgets(line, sizeof(line), in)) {
      line_number++;

      /* Remove extra white-space and comments */
      CPS_NormalizeLine(line);

      /* Skip blank lines */
      if (!*line)
        continue;

      /* We have a real line, now try to match commands */
      processed_command = command = line;
      p = CPS_SplitWord(line);

      if (!strcasecmp(command, "acquisitionport")) {
        parse_int(p, &acquisition_port);
      } else if (!strcasecmp(command, "allow")) {
        parse_allow(p);
      } else if (!strcasecmp(command, "bindacqaddress")) {
        parse_bindacqaddress(p);
      } else if (!strcasecmp(command, "bindaddress")) {
        parse_bindaddress(p);
      } else if (!strcasecmp(command, "bindcmdaddress")) {
        parse_bindcmdaddress(p);
      } else if (!strcasecmp(command, "broadcast")) {
        parse_broadcast(p);
      } else if (!strcasecmp(command, "clientloglimit")) {
        parse_clientloglimit(p);
      } else if (!strcasecmp(command, "cmdallow")) {
        parse_cmdallow(p);
      } else if (!strcasecmp(command, "cmddeny")) {
        parse_cmddeny(p);
      } else if (!strcasecmp(command, "cmdport")) {
        parse_int(p, &cmd_port);
      } else if (!strcasecmp(command, "combinelimit")) {
        parse_double(p, &combine_limit);
      } else if (!strcasecmp(command, "commandkey")) {
        parse_unsignedlong(p, &command_key_id);
      } else if (!strcasecmp(command, "corrtimeratio")) {
        parse_double(p, &correction_time_ratio);
      } else if (!strcasecmp(command, "deny")) {
        parse_deny(p);
      } else if (!strcasecmp(command, "driftfile")) {
        parse_string(p, &drift_file);
      } else if (!strcasecmp(command, "dumpdir")) {
        parse_string(p, &dumpdir);
      } else if (!strcasecmp(command, "dumponexit")) {
        do_dump_on_exit = parse_null(p);
      } else if (!strcasecmp(command, "fallbackdrift")) {
        parse_fallbackdrift(p);
      } else if (!strcasecmp(command, "generatecommandkey")) {
        generate_command_key = parse_null(p);
      } else if (!strcasecmp(command, "hwclockfile")) {
        parse_string(p, &hwclock_file);
      } else if (!strcasecmp(command, "include")) {
        parse_include(p);
      } else if (!strcasecmp(command, "initstepslew")) {
        parse_initstepslew(p);
      } else if (!strcasecmp(command, "keyfile")) {
        parse_string(p, &keys_file);
      } else if (!strcasecmp(command, "leapsectz")) {
        parse_string(p, &leapsec_tz);
      } else if (!strcasecmp(command, "linux_freq_scale")) {
        set_linux_freq_scale = parse_double(p, &linux_freq_scale);
      } else if (!strcasecmp(command, "linux_hz")) {
        set_linux_hz = parse_int(p, &linux_hz);
      } else if (!strcasecmp(command, "local")) {
        parse_local(p);
      } else if (!strcasecmp(command, "lock_all")) {
        lock_memory = parse_null(p);
      } else if (!strcasecmp(command, "log")) {
        parse_log(p);
      } else if (!strcasecmp(command, "logbanner")) {
        parse_int(p, &log_banner);
      } else if (!strcasecmp(command, "logchange")) {
        do_log_change = parse_double(p, &log_change_threshold);
      } else if (!strcasecmp(command, "logdir")) {
        parse_string(p, &logdir);
      } else if (!strcasecmp(command, "mailonchange")) {
        parse_mailonchange(p);
      } else if (!strcasecmp(command, "makestep")) {
        parse_makestep(p);
      } else if (!strcasecmp(command, "manual")) {
        enable_manual = parse_null(p);
      } else if (!strcasecmp(command, "maxchange")) {
        parse_maxchange(p);
      } else if (!strcasecmp(command, "maxclockerror")) {
        parse_double(p, &max_clock_error);
      } else if (!strcasecmp(command, "maxsamples")) {
        parse_int(p, &max_samples);
      } else if (!strcasecmp(command, "maxupdateskew")) {
        parse_double(p, &max_update_skew);
      } else if (!strcasecmp(command, "minsamples")) {
        parse_int(p, &min_samples);
      } else if (!strcasecmp(command, "noclientlog")) {
        no_client_log = parse_null(p);
      } else if (!strcasecmp(command, "peer")) {
        parse_peer(p);
      } else if (!strcasecmp(command, "pidfile")) {
        parse_string(p, &pidfile);
      } else if (!strcasecmp(command, "port")) {
        parse_int(p, &ntp_port);
      } else if (!strcasecmp(command, "refclock")) {
        parse_refclock(p);
      } else if (!strcasecmp(command, "reselectdist")) {
        parse_double(p, &reselect_distance);
      } else if (!strcasecmp(command, "rtcautotrim")) {
        parse_double(p, &rtc_autotrim_threshold);
      } else if (!strcasecmp(command, "rtcdevice")) {
        parse_string(p, &rtc_device);
      } else if (!strcasecmp(command, "rtcfile")) {
        parse_string(p, &rtc_file);
      } else if (!strcasecmp(command, "rtconutc")) {
        rtc_on_utc = parse_null(p);
      } else if (!strcasecmp(command, "rtcsync")) {
        rtc_sync = parse_null(p);
      } else if (!strcasecmp(command, "sched_priority")) {
        parse_int(p, &sched_priority);
      } else if (!strcasecmp(command, "server")) {
        parse_server(p);
      } else if (!strcasecmp(command, "stratumweight")) {
        parse_double(p, &stratum_weight);
      } else if (!strcasecmp(command, "tempcomp")) {
        parse_tempcomp(p);
      } else if (!strcasecmp(command, "user")) {
        parse_string(p, &user);
      } else {
        other_parse_error("Invalid command");
      }
    }

    line_number = prev_line_number;
    processed_file = prev_processed_file;
    fclose(in);
  }
}

/* ================================================== */

static int
parse_string(char *line, char **result)
{
  check_number_of_args(line, 1);
  *result = strdup(line);
  return 1;
}

/* ================================================== */

static int
parse_int(char *line, int *result)
{
  check_number_of_args(line, 1);
  if (sscanf(line, "%d", result) != 1) {
    command_parse_error();
    return 0;
  }
  return 1;
}

/* ================================================== */

static int
parse_unsignedlong(char *line, unsigned long *result)
{
  check_number_of_args(line, 1);
  if (sscanf(line, "%lu", result) != 1) {
    command_parse_error();
    return 0;
  }
  return 1;
}

/* ================================================== */

static int
parse_double(char *line, double *result)
{
  check_number_of_args(line, 1);
  if (sscanf(line, "%lf", result) != 1) {
    command_parse_error();
    return 0;
  }
  return 1;
}

/* ================================================== */

static int
parse_null(char *line)
{
  check_number_of_args(line, 0);
  return 1;
}

/* ================================================== */

static void
parse_source(char *line, NTP_Source_Type type)
{
  CPS_Status status;

  if (n_ntp_sources >= MAX_NTP_SOURCES)
    return;

  ntp_sources[n_ntp_sources].type = type;
  status = CPS_ParseNTPSourceAdd(line, &ntp_sources[n_ntp_sources].params);

  switch (status) {
    case CPS_Success:
      n_ntp_sources++;
      break;
    case CPS_BadOption:
      other_parse_error("Invalid server/peer parameter");
      break;
    case CPS_BadHost:
      other_parse_error("Invalid host/IP address");
      break;
    case CPS_BadPort:
      other_parse_error("Unreadable port");
      break;
    case CPS_BadMinpoll:
      other_parse_error("Unreadable minpoll");
      break;
    case CPS_BadMaxpoll:
      other_parse_error("Unreadable maxpoll");
      break;
    case CPS_BadPresend:
      other_parse_error("Unreadable presend");
      break;
    case CPS_BadMaxdelaydevratio:
      other_parse_error("Unreadable maxdelaydevratio");
      break;
    case CPS_BadMaxdelayratio:
      other_parse_error("Unreadable maxdelayratio");
      break;
    case CPS_BadMaxdelay:
      other_parse_error("Unreadable maxdelay");
      break;
    case CPS_BadKey:
      other_parse_error("Unreadable key");
      break;
    case CPS_BadMinstratum:
      other_parse_error("Unreadable minstratum");
      break;
    case CPS_BadPolltarget:
      other_parse_error("Unreadable polltarget");
      break;
  }
}

/* ================================================== */

static void
parse_server(char *line)
{
  parse_source(line, NTP_SERVER);
}

/* ================================================== */

static void
parse_peer(char *line)
{
  parse_source(line, NTP_PEER);
}

/* ================================================== */

static void
parse_refclock(char *line)
{
  int i, n, poll, dpoll, filter_length, pps_rate;
  uint32_t ref_id, lock_ref_id;
  double offset, delay, precision;
  char *p, *cmd, *name, *param;
  unsigned char ref[5];
  SRC_SelectOption sel_option;

  i = n_refclock_sources;
  if (i >= MAX_RCL_SOURCES)
    return;

  poll = 4;
  dpoll = 0;
  filter_length = 64;
  pps_rate = 0;
  offset = 0.0;
  delay = 1e-9;
  precision = 0.0;
  ref_id = 0;
  lock_ref_id = 0;
  sel_option = SRC_SelectNormal;

  if (!*line) {
    command_parse_error();
    return;
  }

  p = line;
  line = CPS_SplitWord(line);

  if (!*line) {
    command_parse_error();
    return;
  }

  name = strdup(p);

  p = line;
  line = CPS_SplitWord(line);
  param = strdup(p);

  while (*line) {
    cmd = line;
    line = CPS_SplitWord(line);
    if (!strcasecmp(cmd, "refid")) {
      if (sscanf(line, "%4s%n", (char *)ref, &n) != 1)
        break;
      ref_id = ref[0] << 24 | ref[1] << 16 | ref[2] << 8 | ref[3];
    } else if (!strcasecmp(cmd, "lock")) {
      if (sscanf(line, "%4s%n", (char *)ref, &n) != 1)
        break;
      lock_ref_id = ref[0] << 24 | ref[1] << 16 | ref[2] << 8 | ref[3];
    } else if (!strcasecmp(cmd, "poll")) {
      if (sscanf(line, "%d%n", &poll, &n) != 1) {
        break;
      }
    } else if (!strcasecmp(cmd, "dpoll")) {
      if (sscanf(line, "%d%n", &dpoll, &n) != 1) {
        break;
      }
    } else if (!strcasecmp(cmd, "filter")) {
      if (sscanf(line, "%d%n", &filter_length, &n) != 1) {
        break;
      }
    } else if (!strcasecmp(cmd, "rate")) {
      if (sscanf(line, "%d%n", &pps_rate, &n) != 1)
        break;
    } else if (!strcasecmp(cmd, "offset")) {
      if (sscanf(line, "%lf%n", &offset, &n) != 1)
        break;
    } else if (!strcasecmp(cmd, "delay")) {
      if (sscanf(line, "%lf%n", &delay, &n) != 1)
        break;
    } else if (!strcasecmp(cmd, "precision")) {
      if (sscanf(line, "%lf%n", &precision, &n) != 1)
        break;
    } else if (!strcasecmp(cmd, "noselect")) {
      n = 0;
      sel_option = SRC_SelectNoselect;
    } else if (!strcasecmp(cmd, "prefer")) {
      n = 0;
      sel_option = SRC_SelectPrefer;
    } else {
      break;
    }
    line += n;
  }

  if (*line) {
    other_parse_error("Invalid/unreadable refclock parameter");
    return;
  }

  refclock_sources[i].driver_name = name;
  refclock_sources[i].driver_parameter = param;
  refclock_sources[i].driver_poll = dpoll;
  refclock_sources[i].poll = poll;
  refclock_sources[i].filter_length = filter_length;
  refclock_sources[i].pps_rate = pps_rate;
  refclock_sources[i].offset = offset;
  refclock_sources[i].delay = delay;
  refclock_sources[i].precision = precision;
  refclock_sources[i].sel_option = sel_option;
  refclock_sources[i].ref_id = ref_id;
  refclock_sources[i].lock_ref_id = lock_ref_id;

  n_refclock_sources++;
}

/* ================================================== */

static void
parse_log(char *line)
{
  char *log_name;
  do {
    log_name = line;
    line = CPS_SplitWord(line);
    if (*log_name) {
      if (!strcmp(log_name, "measurements")) {
        do_log_measurements = 1;
      } else if (!strcmp(log_name, "statistics")) {
        do_log_statistics = 1;
      } else if (!strcmp(log_name, "tracking")) {
        do_log_tracking = 1;
      } else if (!strcmp(log_name, "rtc")) {
        do_log_rtc = 1;
      } else if (!strcmp(log_name, "refclocks")) {
        do_log_refclocks = 1;
      } else if (!strcmp(log_name, "tempcomp")) {
        do_log_tempcomp = 1;
      } else {
        other_parse_error("Invalid log parameter");
        break;
      }
    } else {
      break;
    }
  } while (1);
}

/* ================================================== */

static void
parse_local(char *line)
{
  int stratum;
  if (sscanf(line, "stratum%d", &stratum) == 1) {
    local_stratum = stratum;
    enable_local = 1;
  } else {
    command_parse_error();
  }
}

/* ================================================== */

static void
parse_initstepslew(char *line)
{
  char *p, *hostname;
  IPAddr ip_addr;

  /* Ignore the line if chronyd was started with -R. */
  if (restarted) {
    return;
  }

  n_init_srcs = 0;
  p = CPS_SplitWord(line);

  if (sscanf(line, "%lf", &init_slew_threshold) != 1) {
    command_parse_error();
    return;
  }

  while (*p) {
    hostname = p;
    p = CPS_SplitWord(p);
    if (*hostname) {
      if (DNS_Name2IPAddress(hostname, &ip_addr) == DNS_Success) {
        init_srcs_ip[n_init_srcs] = ip_addr;
        ++n_init_srcs;
      } else {
        LOG(LOGS_WARN, LOGF_Configure, "Could not resolve address of initstepslew server %s", hostname);
      }
      
      if (n_init_srcs >= MAX_INIT_SRCS) {
        other_parse_error("Too many initstepslew servers");
      }
    }
  }
  if (n_init_srcs > 0) {
    do_init_stepslew = 1;
  }
}

/* ================================================== */

static void
parse_clientloglimit(char *line)
{
  check_number_of_args(line, 1);
  if (sscanf(line, "%lu", &client_log_limit) != 1) {
    command_parse_error();
  }

  if (client_log_limit == 0) {
    /* unlimited */
    client_log_limit = (unsigned long)-1;
  }
}

/* ================================================== */

static void
parse_fallbackdrift(char *line)
{
  check_number_of_args(line, 2);
  if (sscanf(line, "%d %d", &fb_drift_min, &fb_drift_max) != 2) {
    command_parse_error();
  }
}

/* ================================================== */

static void
parse_makestep(char *line)
{
  check_number_of_args(line, 2);
  if (sscanf(line, "%lf %d", &make_step_threshold, &make_step_limit) != 2) {
    make_step_limit = 0;
    command_parse_error();
  }

  /* Disable limited makestep if chronyd was started with -R. */
  if (restarted && make_step_limit > 0) {
    make_step_limit = 0;
  }
}

/* ================================================== */

static void
parse_maxchange(char *line)
{
  check_number_of_args(line, 3);
  if (sscanf(line, "%lf %d %d", &max_offset, &max_offset_delay, &max_offset_ignore) != 3) {
    max_offset_delay = -1;
    command_parse_error();
  }
}

/* ================================================== */

static void
parse_mailonchange(char *line)
{
  char *address;
  check_number_of_args(line, 2);
  address = line;
  line = CPS_SplitWord(line);
  if (sscanf(line, "%lf", &mail_change_threshold) == 1) {
    mail_user_on_change = strdup(address);
  } else {
    mail_user_on_change = NULL;
    command_parse_error();
  }
}

/* ================================================== */

static void
parse_allow_deny(char *line, AllowDeny *list, int allow)
{
  char *p;
  unsigned long a, b, c, d, n;
  int all = 0;
  AllowDeny *new_node = NULL;
  IPAddr ip_addr;

  p = line;

  if (!strncmp(p, "all", 3)) {
    all = 1;
    p = CPS_SplitWord(line);
  }

  if (!*p) {
    /* Empty line applies to all addresses */
    new_node = MallocNew(AllowDeny);
    new_node->allow = allow;
    new_node->all = all;
    new_node->ip.family = IPADDR_UNSPEC;
    new_node->subnet_bits = 0;
  } else {
    char *slashpos;
    slashpos = strchr(p, '/');
    if (slashpos) *slashpos = 0;

    check_number_of_args(p, 1);
    n = 0;
    if (UTI_StringToIP(p, &ip_addr) ||
        (n = sscanf(p, "%lu.%lu.%lu.%lu", &a, &b, &c, &d)) >= 1) {
      new_node = MallocNew(AllowDeny);
      new_node->allow = allow;
      new_node->all = all;

      if (n == 0) {
        new_node->ip = ip_addr;
        if (ip_addr.family == IPADDR_INET6)
          new_node->subnet_bits = 128;
        else
          new_node->subnet_bits = 32;
      } else {
        new_node->ip.family = IPADDR_INET4;

        a &= 0xff;
        b &= 0xff;
        c &= 0xff;
        d &= 0xff;
        
        switch (n) {
          case 1:
            new_node->ip.addr.in4 = (a<<24);
            new_node->subnet_bits = 8;
            break;
          case 2:
            new_node->ip.addr.in4 = (a<<24) | (b<<16);
            new_node->subnet_bits = 16;
            break;
          case 3:
            new_node->ip.addr.in4 = (a<<24) | (b<<16) | (c<<8);
            new_node->subnet_bits = 24;
            break;
          case 4:
            new_node->ip.addr.in4 = (a<<24) | (b<<16) | (c<<8) | d;
            new_node->subnet_bits = 32;
            break;
          default:
            assert(0);
        }
      }
      
      if (slashpos) {
        int specified_subnet_bits, n;
        n = sscanf(slashpos+1, "%d", &specified_subnet_bits);
        if (n == 1) {
          new_node->subnet_bits = specified_subnet_bits;
        } else {
          command_parse_error();
        }
      }

    } else {
      if (DNS_Name2IPAddress(p, &ip_addr) == DNS_Success) {
        new_node = MallocNew(AllowDeny);
        new_node->allow = allow;
        new_node->all = all;
        new_node->ip = ip_addr;
        if (ip_addr.family == IPADDR_INET6)
          new_node->subnet_bits = 128;
        else
          new_node->subnet_bits = 32;
      } else {
        command_parse_error();
      }      
    }
  }
  
  if (new_node) {
    new_node->prev = list->prev;
    new_node->next = list;
    list->prev->next = new_node;
    list->prev = new_node;
  }

}
  

/* ================================================== */

static void
parse_allow(char *line)
{
  parse_allow_deny(line, &ntp_auth_list, 1);
}


/* ================================================== */

static void
parse_deny(char *line)
{
  parse_allow_deny(line, &ntp_auth_list, 0);
}

/* ================================================== */

static void
parse_cmdallow(char *line)
{
  parse_allow_deny(line, &cmd_auth_list, 1);
}


/* ================================================== */

static void
parse_cmddeny(char *line)
{
  parse_allow_deny(line, &cmd_auth_list, 0);
}

/* ================================================== */

static void
parse_bindacqaddress(char *line)
{
  IPAddr ip;

  check_number_of_args(line, 1);
  if (UTI_StringToIP(line, &ip)) {
    if (ip.family == IPADDR_INET4)
      bind_acq_address4 = ip;
    else if (ip.family == IPADDR_INET6)
      bind_acq_address6 = ip;
  } else {
    command_parse_error();
  }
}

/* ================================================== */

static void
parse_bindaddress(char *line)
{
  IPAddr ip;

  check_number_of_args(line, 1);
  if (UTI_StringToIP(line, &ip)) {
    if (ip.family == IPADDR_INET4)
      bind_address4 = ip;
    else if (ip.family == IPADDR_INET6)
      bind_address6 = ip;
  } else {
    command_parse_error();
  }
}

/* ================================================== */

static void
parse_bindcmdaddress(char *line)
{
  IPAddr ip;

  check_number_of_args(line, 1);
  if (UTI_StringToIP(line, &ip)) {
    if (ip.family == IPADDR_INET4)
      bind_cmd_address4 = ip;
    else if (ip.family == IPADDR_INET6)
      bind_cmd_address6 = ip;
  } else {
    command_parse_error();
  }
}

/* ================================================== */

static void
parse_broadcast(char *line)
{
  /* Syntax : broadcast <interval> <broadcast-IP-addr> [<port>] */
  int port;
  int interval;
  char *p;
  IPAddr ip;
  
  p = line;
  line = CPS_SplitWord(line);

  if (sscanf(p, "%d", &interval) != 1) {
    command_parse_error();
    return;
  }

  p = line;
  line = CPS_SplitWord(line);

  if (!UTI_StringToIP(p, &ip)) {
    command_parse_error();
    return;
  }

  p = line;
  line = CPS_SplitWord(line);

  if (*p) {
    if (sscanf(p, "%d", &port) != 1 || *line) {
      command_parse_error();
      return;
    }
  } else {
    /* default port */
    port = 123;
  }

  if (max_broadcasts == n_broadcasts) {
    /* Expand array */
    max_broadcasts += 8;
    if (broadcasts) {
      broadcasts = ReallocArray(NTP_Broadcast_Destination, max_broadcasts, broadcasts);
    } else {
      broadcasts = MallocArray(NTP_Broadcast_Destination, max_broadcasts);
    }
  }

  broadcasts[n_broadcasts].addr = ip;
  broadcasts[n_broadcasts].port = port;
  broadcasts[n_broadcasts].interval = interval;
  ++n_broadcasts;
}

/* ================================================== */

static void
parse_tempcomp(char *line)
{
  char *p;

  check_number_of_args(line, 6);
  p = line;
  line = CPS_SplitWord(line);

  if (!*p) {
    command_parse_error();
    return;
  }

  if (sscanf(line, "%lf %lf %lf %lf %lf", &tempcomp_interval, &tempcomp_T0, &tempcomp_k0, &tempcomp_k1, &tempcomp_k2) != 5) {
    command_parse_error();
    return;
  }

  tempcomp_file = strdup(p);
}

/* ================================================== */

static void
parse_include(char *line)
{
  check_number_of_args(line, 1);
  CNF_ReadFile(line);
}

/* ================================================== */

void
CNF_ProcessInitStepSlew(void (*after_hook)(void *), void *anything)
{
  if (do_init_stepslew) {
    ACQ_StartAcquisition(n_init_srcs, init_srcs_ip, init_slew_threshold, after_hook, anything);
  } else {
    (after_hook)(anything);
  }
}

/* ================================================== */

void
CNF_AddSources(void) {
  int i;

  for (i=0; i<n_ntp_sources; i++) {
    NSR_AddUnresolvedSource(ntp_sources[i].params.name, ntp_sources[i].params.port,
        ntp_sources[i].type, &ntp_sources[i].params.params);
  }

  NSR_ResolveSources();
}

/* ================================================== */

void
CNF_AddRefclocks(void) {
  int i;

  for (i=0; i<n_refclock_sources; i++) {
    RCL_AddRefclock(&refclock_sources[i]);
  }
}

/* ================================================== */

void
CNF_AddBroadcasts(void)
{
  int i;
  for (i=0; i<n_broadcasts; i++) {
    BRD_AddDestination(&broadcasts[i].addr,
                       broadcasts[i].port,
                       broadcasts[i].interval);
  }
}

/* ================================================== */

unsigned short
CNF_GetNTPPort(void)
{
  return ntp_port;
}

/* ================================================== */

unsigned short
CNF_GetAcquisitionPort(void)
{
  return acquisition_port;
}

/* ================================================== */

char *
CNF_GetDriftFile(void)
{
  return drift_file;
}

/* ================================================== */

int
CNF_GetLogBanner(void)
{
  return log_banner;
}

/* ================================================== */

char *
CNF_GetLogDir(void)
{
  return logdir;
}

/* ================================================== */

char *
CNF_GetDumpDir(void)
{
  return dumpdir;
}

/* ================================================== */

int
CNF_GetLogMeasurements(void)
{
  return do_log_measurements;
}

/* ================================================== */

int
CNF_GetLogStatistics(void)
{
  return do_log_statistics;
}

/* ================================================== */

int
CNF_GetLogTracking(void)
{
  return do_log_tracking;
}

/* ================================================== */

int
CNF_GetLogRtc(void)
{
  return do_log_rtc;
}

/* ================================================== */

int
CNF_GetLogRefclocks(void)
{
  return do_log_refclocks;
}

/* ================================================== */

int
CNF_GetLogTempComp(void)
{
  return do_log_tempcomp;
}

/* ================================================== */

char *
CNF_GetKeysFile(void)
{
  return keys_file;
}

/* ================================================== */

double
CNF_GetRtcAutotrim(void)
{
  return rtc_autotrim_threshold;
}

/* ================================================== */

char *
CNF_GetRtcFile(void)
{
  return rtc_file;
}

/* ================================================== */

char *
CNF_GetRtcDevice(void)
{
  return rtc_device;
}

/* ================================================== */

unsigned long
CNF_GetCommandKey(void)
{
  return command_key_id;
}

/* ================================================== */

int
CNF_GetGenerateCommandKey(void)
{
  return generate_command_key;
}

/* ================================================== */

int
CNF_GetDumpOnExit(void)
{
  return do_dump_on_exit;
}

/* ================================================== */

double
CNF_GetMaxUpdateSkew(void)
{
  return max_update_skew;
}

/* ================================================== */

double
CNF_GetMaxClockError(void)
{
  return max_clock_error;
}

/* ================================================== */

double
CNF_GetCorrectionTimeRatio(void)
{
  return correction_time_ratio;
}

/* ================================================== */

double
CNF_GetReselectDistance(void)
{
  return reselect_distance;
}

/* ================================================== */

double
CNF_GetStratumWeight(void)
{
  return stratum_weight;
}

/* ================================================== */

double
CNF_GetCombineLimit(void)
{
  return combine_limit;
}

/* ================================================== */

int
CNF_GetManualEnabled(void)
{
  return enable_manual;
}

/* ================================================== */

int
CNF_GetCommandPort(void) {
  return cmd_port;
}

/* ================================================== */

int
CNF_AllowLocalReference(int *stratum)
{
  if (enable_local) {
    *stratum = local_stratum;
    return 1;
  } else {
    return 0;
  }
}

/* ================================================== */

int
CNF_GetRtcOnUtc(void)
{
  return rtc_on_utc;
}

/* ================================================== */

int
CNF_GetRtcSync(void)
{
  return rtc_sync;
}

/* ================================================== */

void
CNF_GetMakeStep(int *limit, double *threshold)
{
  *limit = make_step_limit;
  *threshold = make_step_threshold;
}

/* ================================================== */

void
CNF_GetMaxChange(int *delay, int *ignore, double *offset)
{
  *delay = max_offset_delay;
  *ignore = max_offset_ignore;
  *offset = max_offset;
}

/* ================================================== */

void
CNF_GetLogChange(int *enabled, double *threshold)
{
  *enabled = do_log_change;
  *threshold = log_change_threshold;
}

/* ================================================== */

void
CNF_GetMailOnChange(int *enabled, double *threshold, char **user)
{
  if (mail_user_on_change) {
    *enabled = 1;
    *threshold = mail_change_threshold;
    *user = mail_user_on_change;
  } else {
    *enabled = 0;
    *threshold = 0.0;
    *user = NULL;
  }
}  

/* ================================================== */

void
CNF_SetupAccessRestrictions(void)
{
  AllowDeny *node;
  int status;

  for (node = ntp_auth_list.next; node != &ntp_auth_list; node = node->next) {
    status = NCR_AddAccessRestriction(&node->ip, node->subnet_bits, node->allow, node->all);
    if (!status) {
      LOG_FATAL(LOGF_Configure, "Bad subnet in %s/%d", UTI_IPToString(&node->ip), node->subnet_bits);
    }
  }

  for (node = cmd_auth_list.next; node != &cmd_auth_list; node = node->next) {
    status = CAM_AddAccessRestriction(&node->ip, node->subnet_bits, node->allow, node->all);
    if (!status) {
      LOG_FATAL(LOGF_Configure, "Bad subnet in %s/%d", UTI_IPToString(&node->ip), node->subnet_bits);
    }
  }
}

/* ================================================== */

int
CNF_GetNoClientLog(void)
{
  return no_client_log;
}

/* ================================================== */

unsigned long
CNF_GetClientLogLimit(void)
{
  return client_log_limit;
}

/* ================================================== */

void
CNF_GetFallbackDrifts(int *min, int *max)
{
  *min = fb_drift_min;
  *max = fb_drift_max;
}

/* ================================================== */

void
CNF_GetBindAddress(int family, IPAddr *addr)
{
  if (family == IPADDR_INET4)
    *addr = bind_address4;
  else if (family == IPADDR_INET6)
    *addr = bind_address6;
  else
    addr->family = IPADDR_UNSPEC;
}

/* ================================================== */

void
CNF_GetBindAcquisitionAddress(int family, IPAddr *addr)
{
  if (family == IPADDR_INET4)
    *addr = bind_acq_address4;
  else if (family == IPADDR_INET6)
    *addr = bind_acq_address6;
  else
    addr->family = IPADDR_UNSPEC;
}

/* ================================================== */

void
CNF_GetBindCommandAddress(int family, IPAddr *addr)
{
  if (family == IPADDR_INET4)
    *addr = bind_cmd_address4.family != IPADDR_UNSPEC ? bind_cmd_address4 : bind_address4;
  else if (family == IPADDR_INET6)
    *addr = bind_cmd_address6.family != IPADDR_UNSPEC ? bind_cmd_address6 : bind_address6;
  else
    addr->family = IPADDR_UNSPEC;
}

/* ================================================== */

char *
CNF_GetPidFile(void)
{
  return pidfile;
}

/* ================================================== */

char *
CNF_GetLeapSecTimezone(void)
{
  return leapsec_tz;
}

/* ================================================== */

void
CNF_GetLinuxHz(int *set, int *hz)
{
  *set = set_linux_hz;
  *hz = linux_hz;
}

/* ================================================== */

void
CNF_GetLinuxFreqScale(int *set, double *freq_scale)
{
  *set = set_linux_freq_scale;
  *freq_scale = linux_freq_scale ;
}

/* ================================================== */

int
CNF_GetSchedPriority(void)
{
  return sched_priority;
}

/* ================================================== */

int
CNF_GetLockMemory(void)
{
  return lock_memory;
}

/* ================================================== */

void
CNF_GetTempComp(char **file, double *interval, double *T0, double *k0, double *k1, double *k2)
{
  *file = tempcomp_file;
  *interval = tempcomp_interval;
  *T0 = tempcomp_T0;
  *k0 = tempcomp_k0;
  *k1 = tempcomp_k1;
  *k2 = tempcomp_k2;
}

/* ================================================== */

char *
CNF_GetUser(void)
{
  return user;
}

/* ================================================== */

int
CNF_GetMaxSamples(void)
{
  return max_samples;
}

/* ================================================== */

int
CNF_GetMinSamples(void)
{
  return min_samples;
}

/* ================================================== */

char *
CNF_GetHwclockFile(void)
{
  return hwclock_file;
}
