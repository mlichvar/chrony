/*
  $Header: /cvs/src/chrony/conf.c,v 1.45 2003/09/22 21:22:30 richard Exp $

  =======================================================================

  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Richard P. Curnow  1997-2003
 * Copyright (C) Miroslav Lichvar  2009
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

1999-12-19  Kalle Olavi Niemitalo  <tosi@stekt.oulu.fi>

	* conf.c: Added a new configuration setting "acquisitionport" and
	a function CNF_GetAcquisitionPort to read its value.
	(acquisition_port): New variable.
	(parse_port): Delegate most work to new function parse_some_port.
	(parse_acquisitionport): New function.
	(commands): Added "acquisitionport".
	(CNF_GetAcquisitionPort): New function.

  */

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

#ifndef DEFAULT_CONF_DIR
#define DEFAULT_CONF_DIR "/etc"
#endif

#define DEFAULT_CONF_FILE DEFAULT_CONF_DIR"/chrony.conf"

/* ================================================== */
/* Forward prototypes */

static void parse_commandkey(const char *);
static void parse_driftfile(const char *);
static void parse_dumpdir(const char *);
static void parse_dumponexit(const char *);
static void parse_keyfile(const char *);
static void parse_rtcfile(const char *);
static void parse_log(const char *);
static void parse_logdir(const char *);
static void parse_maxupdateskew(const char *);
static void parse_peer(const char *);
static void parse_acquisitionport(const char *);
static void parse_port(const char *);
static void parse_server(const char *);
static void parse_refclock(const char *);
static void parse_local(const char *);
static void parse_manual(const char *);
static void parse_initstepslew(const char *);
static void parse_allow(const char *);
static void parse_deny(const char *);
static void parse_cmdallow(const char *);
static void parse_cmddeny(const char *);
static void parse_cmdport(const char *);
static void parse_rtconutc(const char *);
static void parse_noclientlog(const char *);
static void parse_clientloglimit(const char *);
static void parse_makestep(const char *);
static void parse_logchange(const char *);
static void parse_mailonchange(const char *);
static void parse_bindaddress(const char *);
static void parse_bindcmdaddress(const char *);
static void parse_rtcdevice(const char *);
static void parse_pidfile(const char *);
static void parse_broadcast(const char *);
static void parse_linux_hz(const char *);
static void parse_linux_freq_scale(const char *);
static void parse_sched_priority(const char *);
static void parse_lockall(const char *);

/* ================================================== */
/* Configuration variables */

static char *rtc_device = "/dev/rtc";
static int acquisition_port = 0; /* 0 means let kernel choose port */
static int ntp_port = 123;
static char *keys_file = NULL;
static char *drift_file = NULL;
static char *rtc_file = NULL;
static unsigned long command_key_id;
static double max_update_skew = DBL_MAX;

static int cmd_port = -1;

static int do_log_measurements = 0;
static int do_log_statistics = 0;
static int do_log_tracking = 0;
static int do_log_rtc = 0;
static int do_log_refclocks = 0;
static int do_dump_on_exit = 0;
static char *logdir = ".";
static char *dumpdir = ".";

static int enable_local=0;
#define DEFAULT_LOCAL_STRATUM 8
static int local_stratum;

static int do_init_stepslew = 0;
static int n_init_srcs;

/* Threshold (in seconds) - if absolute value of initial error is less
   than this, slew instead of stepping */
static int init_slew_threshold = -1;
#define MAX_INIT_SRCS 8
static IPAddr init_srcs_ip[MAX_INIT_SRCS];

static int enable_manual=0;

/* Flag set if the RTC runs UTC (default is it runs local time
   incl. daylight saving). */
static int rtc_on_utc = 0;

/* Limit and threshold for clock stepping */
static int make_step_limit = 0;
static double make_step_threshold = 0.0;

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

/* IP addresses for binding the NTP socket to.  UNSPEC family means INADDR_ANY
   will be used */
static IPAddr bind_address4, bind_address6;

/* IP addresses for binding the command socket to.  UNSPEC family means
   use the value of bind_address */
static IPAddr bind_cmd_address4, bind_cmd_address6;

/* Filename to use for storing pid of running chronyd, to prevent multiple
 * chronyds being started. */
static char *pidfile = "/var/run/chronyd.pid";

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

/* ================================================== */

typedef struct {
  const char *keyword;
  int len;
  void (*handler)(const char *);
} Command;

static const Command commands[] = {
  {"server", 6, parse_server},
  {"peer", 4, parse_peer},
  {"refclock", 8, parse_refclock},
  {"acquisitionport", 15, parse_acquisitionport},
  {"port", 4, parse_port},
  {"driftfile", 9, parse_driftfile},
  {"keyfile", 7, parse_keyfile},
  {"rtcfile", 7, parse_rtcfile},
  {"logdir", 6, parse_logdir},
  {"log", 3, parse_log},
  {"dumponexit", 10, parse_dumponexit},
  {"dumpdir", 7, parse_dumpdir},
  {"maxupdateskew", 13, parse_maxupdateskew},
  {"commandkey", 10, parse_commandkey},
  {"initstepslew", 12, parse_initstepslew},
  {"local", 5, parse_local},
  {"manual", 6, parse_manual},
  {"allow", 5, parse_allow},
  {"deny", 4, parse_deny},
  {"cmdallow", 8, parse_cmdallow},
  {"cmddeny", 7, parse_cmddeny},
  {"cmdport", 7, parse_cmdport},
  {"rtconutc", 8, parse_rtconutc},
  {"noclientlog", 11, parse_noclientlog},
  {"clientloglimit", 14, parse_clientloglimit},
  {"makestep", 8, parse_makestep},
  {"logchange", 9, parse_logchange},
  {"mailonchange", 12, parse_mailonchange},
  {"bindaddress", 11, parse_bindaddress},
  {"bindcmdaddress", 14, parse_bindcmdaddress},
  {"rtcdevice", 9, parse_rtcdevice},
  {"pidfile", 7, parse_pidfile},
  {"broadcast", 9, parse_broadcast},
  {"linux_hz", 8, parse_linux_hz},
  {"linux_freq_scale", 16, parse_linux_freq_scale},
  {"sched_priority", 14, parse_sched_priority},
  {"lock_all", 8, parse_lockall}
};

static int n_commands = (sizeof(commands) / sizeof(commands[0]));

/* The line number in the configuration file being processed */
static int line_number;

/* ================================================== */

typedef enum {
  SERVER, PEER
} NTP_Source_Type;

typedef struct {
  NTP_Source_Type type;
  IPAddr ip_addr;
  unsigned short port;
  SourceParameters params;
} NTP_Source;

#define MAX_NTP_SOURCES 64

static NTP_Source ntp_sources[MAX_NTP_SOURCES];
static int n_ntp_sources = 0;

#define MAX_RCL_SOURCES 8

static RefclockParameters refclock_sources[MAX_RCL_SOURCES];
static int n_refclock_sources = 0;

/* ================================================== */

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

/* ================================================== */

/* Read the configuration file */
void
CNF_ReadFile(const char *filename)
{
  FILE *in;
  char line[2048];
  char *p;
  int i, ok;

  if (filename == NULL) {
    filename = DEFAULT_CONF_FILE;
  }

  in = fopen(filename, "r");
  if (!in) {
    LOG(LOGS_ERR, LOGF_Configure, "Could not open configuration file [%s]", filename);
  } else {

    line_number = 0;

    /* Success */
    while (fgets(line, sizeof(line), in)) {

      line_number++;

      /* Strip trailing newline */
      line[strlen(line) - 1] = 0;

      /* Discard comment lines, blank lines etc */
      p = line;
      while(*p && (isspace((unsigned char)*p)))
        p++;

      if (!*p || (strchr("!;#%", *p) != NULL))
        continue;

      /* We have a real line, now try to match commands */
      ok = 0;
      for (i=0; i<n_commands; i++) {
        if (!strncasecmp(commands[i].keyword, p, commands[i].len)) {
          (*(commands[i].handler))(p + commands[i].len);
          ok = 1;
        }
      }      

      if (!ok) {
        LOG(LOGS_WARN, LOGF_Configure, "Line %d in configuration file [%s] contains invalid command",
            line_number, filename);
      }

    }

    fclose(in);
  }

}

/* ================================================== */

static void
parse_source(const char *line, NTP_Source_Type type)
{
  int i;
  NTP_Source s;
  CPS_Status status;
  CPS_NTP_Source params;

  s.type = type;
  status = CPS_ParseNTPSourceAdd(line, &params);

  switch (status) {
    case CPS_Success:
      s.port = params.port;
      s.ip_addr = params.ip_addr;
      s.params = params.params;

      i = n_ntp_sources++;
      ntp_sources[i] = s;

      break;
    case CPS_BadOption:
      LOG(LOGS_WARN, LOGF_Configure, "Unrecognized subcommand at line %d", line_number);
      break;
    case CPS_BadHost:
      LOG(LOGS_WARN, LOGF_Configure, "Invalid host/IP address at line %d", line_number);
      break;
    case CPS_BadPort:
      LOG(LOGS_WARN, LOGF_Configure, "Unreadable port number at line %d", line_number);
      break;
    case CPS_BadMinpoll:
      LOG(LOGS_WARN, LOGF_Configure, "Unreadable minpoll value at line %d", line_number);
      break;
    case CPS_BadMaxpoll:
      LOG(LOGS_WARN, LOGF_Configure, "Unreadable maxpoll value at line %d", line_number);
      break;
    case CPS_BadPresend:
      LOG(LOGS_WARN, LOGF_Configure, "Unreadable presend value at line %d", line_number);
      break;
    case CPS_BadMaxdelayratio:
      LOG(LOGS_WARN, LOGF_Configure, "Unreadable max delay ratio value at line %d", line_number);
      break;
    case CPS_BadMaxdelay:
      LOG(LOGS_WARN, LOGF_Configure, "Unreadable max delay value at line %d", line_number);
      break;
    case CPS_BadKey:
      LOG(LOGS_WARN, LOGF_Configure, "Unreadable key value at line %d", line_number);
      break;
  }

  return;

}

/* ================================================== */

static void
parse_sched_priority(const char *line)
{
  if (sscanf(line, "%d", &sched_priority) != 1) {
    LOG(LOGS_WARN, LOGF_Configure, "Could not read scheduling priority at line %d", line_number);
  }
}

/* ================================================== */

static void
parse_lockall(const char *line)
{
  lock_memory = 1;
}

/* ================================================== */

static void
parse_server(const char *line)
{
  parse_source(line, SERVER);
}

/* ================================================== */

static void
parse_peer(const char *line)
{
  parse_source(line, PEER);
}

/* ================================================== */

static void
parse_refclock(const char *line)
{
  int i, n, poll, dpoll, filter_length, pps_rate;
  unsigned long ref_id, lock_ref_id;
  double offset, delay, precision;
  const char *tmp;
  char name[5], cmd[10 + 1], *param;
  unsigned char ref[5];

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

  if (sscanf(line, "%4s%n", name, &n) != 1) {
    LOG(LOGS_WARN, LOGF_Configure, "Could not read refclock driver name at line %d", line_number);
    return;
  }
  line += n;

  while (isspace(line[0]))
    line++;
  tmp = line;
  while (line[0] != '\0' && !isspace(line[0]))
    line++;

  if (line == tmp) {
    LOG(LOGS_WARN, LOGF_Configure, "Could not read refclock parameter at line %d", line_number);
    return;
  }

  param = MallocArray(char, 1 + line - tmp);
  strncpy(param, tmp, line - tmp);
  param[line - tmp] = '\0';

  while (sscanf(line, "%10s%n", cmd, &n) == 1) {
    line += n;
    if (!strncasecmp(cmd, "refid", 5)) {
      if (sscanf(line, "%4s%n", (char *)ref, &n) != 1)
        break;
      ref_id = ref[0] << 24 | ref[1] << 16 | ref[2] << 8 | ref[3];
    } else if (!strncasecmp(cmd, "lock", 4)) {
      if (sscanf(line, "%4s%n", (char *)ref, &n) != 1)
        break;
      lock_ref_id = ref[0] << 24 | ref[1] << 16 | ref[2] << 8 | ref[3];
    } else if (!strncasecmp(cmd, "poll", 4)) {
      if (sscanf(line, "%d%n", &poll, &n) != 1) {
        break;
      }
    } else if (!strncasecmp(cmd, "dpoll", 5)) {
      if (sscanf(line, "%d%n", &dpoll, &n) != 1) {
        break;
      }
    } else if (!strncasecmp(cmd, "filter", 6)) {
      if (sscanf(line, "%d%n", &filter_length, &n) != 1) {
        break;
      }
    } else if (!strncasecmp(cmd, "rate", 4)) {
      if (sscanf(line, "%d%n", &pps_rate, &n) != 1)
        break;
    } else if (!strncasecmp(cmd, "offset", 6)) {
      if (sscanf(line, "%lf%n", &offset, &n) != 1)
        break;
    } else if (!strncasecmp(cmd, "delay", 5)) {
      if (sscanf(line, "%lf%n", &delay, &n) != 1)
        break;
    } else if (!strncasecmp(cmd, "precision", 9)) {
      if (sscanf(line, "%lf%n", &precision, &n) != 1)
        break;
    } else {
      LOG(LOGS_WARN, LOGF_Configure, "Unknown refclock parameter %s at line %d", cmd, line_number);
      break;
    }
    line += n;
  }

  strncpy(refclock_sources[i].driver_name, name, 4);
  refclock_sources[i].driver_parameter = param;
  refclock_sources[i].driver_poll = dpoll;
  refclock_sources[i].poll = poll;
  refclock_sources[i].filter_length = filter_length;
  refclock_sources[i].pps_rate = pps_rate;
  refclock_sources[i].offset = offset;
  refclock_sources[i].delay = delay;
  refclock_sources[i].precision = precision;
  refclock_sources[i].ref_id = ref_id;
  refclock_sources[i].lock_ref_id = lock_ref_id;

  n_refclock_sources++;
}

/* ================================================== */

static void
parse_some_port(const char *line, int *portvar)
{
  if (sscanf(line, "%d", portvar) != 1) {
    LOG(LOGS_WARN, LOGF_Configure, "Could not read port number at line %d in file", line_number);
  }
}

/* ================================================== */

static void
parse_acquisitionport(const char *line)
{
  parse_some_port(line, &acquisition_port);
}

/* ================================================== */

static void
parse_port(const char *line)
{
  parse_some_port(line, &ntp_port);
}

/* ================================================== */

static void
parse_maxupdateskew(const char *line)
{
  if (sscanf(line, "%lf", &max_update_skew) != 1) {
    LOG(LOGS_WARN, LOGF_Configure, "Could not read max update skew at line %d in file", line_number);
  }
}

/* ================================================== */

static void
parse_driftfile(const char *line)
{
  /* This must allocate enough space! */
  drift_file = MallocArray(char, 1 + strlen(line));
  sscanf(line, "%s", drift_file);
}

/* ================================================== */

static void
strip_trailing_spaces(char *p)
{
  char *q;
  for (q=p; *q; q++)
    ;
  
  for (q--; isspace((unsigned char)*q); q--)
    ;

  *++q = 0;
}

/* ================================================== */

static void
parse_keyfile(const char *line)
{
  /* This must allocate enough space! */
  keys_file = MallocArray(char, 1 + strlen(line));
  sscanf(line, "%s", keys_file);
  strip_trailing_spaces(keys_file);
}

/* ================================================== */

static void
parse_rtcfile(const char *line)
{
  rtc_file = MallocArray(char, 1 + strlen(line));
  sscanf(line, "%s", rtc_file);
  strip_trailing_spaces(rtc_file);
}  

/* ================================================== */

static void
parse_rtcdevice(const char *line)
{
  rtc_device = MallocArray(char, 1 + strlen(line));
  sscanf(line, "%s", rtc_device);
}

/* ================================================== */

static void
parse_logdir(const char *line)
{
  logdir = MallocArray(char, 1 + strlen(line));
  sscanf(line, "%s", logdir);
}

/* ================================================== */

static void
parse_dumpdir(const char *line)
{
  dumpdir = MallocArray(char, 1 + strlen(line));
  sscanf(line, "%s", dumpdir);
}

/* ================================================== */

static void
parse_dumponexit(const char *line)
{
  do_dump_on_exit = 1;
}

/* ================================================== */

static void
parse_log(const char *line)
{
  do {
    while (*line && isspace((unsigned char)*line)) line++;
    if (*line) {
      if (!strncmp(line, "measurements", 12)) {
        do_log_measurements = 1;
        line += 12;
      } else if (!strncmp(line, "statistics", 10)) {
        do_log_statistics = 1;
        line += 10;
      } else if (!strncmp(line, "tracking", 8)) {
        do_log_tracking = 1;
        line += 8;
      } else if (!strncmp(line, "rtc", 3)) {
        do_log_rtc = 1;
        line += 3;
      } else if (!strncmp(line, "refclocks", 9)) {
        do_log_refclocks = 1;
        line += 9;
      } else {
        break;
      }
    } else {
      break;
    }
  } while (1);
}

/* ================================================== */

static void
parse_commandkey(const char *line)
{
  if (sscanf(line, "%lu", &command_key_id) != 1) {
    LOG(LOGS_WARN, LOGF_Configure, "Could not read command key ID at line %d", line_number);
  }
}

/* ================================================== */

static void
parse_local(const char *line)
{
  int stratum;
  enable_local = 1;
  if (sscanf(line, "%*[ \t]stratum%d", &stratum) == 1) {
    local_stratum = stratum;
  } else {
    local_stratum = DEFAULT_LOCAL_STRATUM;
  }
}

/* ================================================== */

static void
parse_cmdport(const char *line)
{
  if (sscanf(line, "%d", &cmd_port) != 1) {
    LOG(LOGS_WARN, LOGF_Configure, "Could not read command port number at line %d", line_number);
  }
}

/* ================================================== */

#define HOSTNAME_LEN 2047
#define SHOSTNAME_LEN "2047"

static void
parse_initstepslew(const char *line)
{
  const char *p;
  char hostname[HOSTNAME_LEN+1];
  int n;
  int threshold;
  IPAddr ip_addr;

  n_init_srcs = 0;
  p = line;

  if (sscanf(p, "%d%n", &threshold, &n) == 1) {
    p += n;
  } else {
    LOG(LOGS_WARN, LOGF_Configure, "Could not parse initstepslew threshold at line %d", line_number);
    return;
  }
  while (*p) {
    if (sscanf(p, "%" SHOSTNAME_LEN "s%n", hostname, &n) == 1) {
      if (DNS_Name2IPAddress(hostname, &ip_addr, 1)) {
        init_srcs_ip[n_init_srcs] = ip_addr;
        ++n_init_srcs;
      }
      
      if (n_init_srcs >= MAX_INIT_SRCS) {
        break;
      }

    } else {
      /* If we get invalid trailing syntax, forget it ... */
      break;
    }
    p += n;
  }
  if (n_init_srcs > 0) {
    do_init_stepslew = 1;
    init_slew_threshold = threshold;
  } else {
    LOG(LOGS_WARN, LOGF_Configure, "No usable initstepslew servers at line %d\n", line_number);
  }
}

/* ================================================== */

static void
parse_manual(const char *line)
{
  enable_manual = 1;
}

/* ================================================== */

static void
parse_rtconutc(const char *line)
{
  rtc_on_utc = 1;
}

/* ================================================== */

static void
parse_noclientlog(const char *line)
{
  no_client_log = 1;
}

/* ================================================== */

static void
parse_clientloglimit(const char *line)
{
  if (sscanf(line, "%lu", &client_log_limit) != 1) {
    LOG(LOGS_WARN, LOGF_Configure, "Could not read clientlog memory limit at line %d", line_number);
  }

  if (client_log_limit == 0) {
    /* unlimited */
    client_log_limit = (unsigned long)-1;
  }
}

/* ================================================== */

static void
parse_makestep(const char *line)
{
  if (sscanf(line, "%lf %d", &make_step_threshold, &make_step_limit) != 2) {
    make_step_limit = 0;
    LOG(LOGS_WARN, LOGF_Configure,
        "Could not read threshold or update limit for stepping clock at line %d\n",
        line_number);
  }
}

/* ================================================== */

static void
parse_logchange(const char *line)
{
  if (sscanf(line, "%lf", &log_change_threshold) == 1) {
    do_log_change = 1;
  } else {
    do_log_change = 0;
    LOG(LOGS_WARN, LOGF_Configure,
        "Could not read threshold for logging clock changes at line %d\n",
        line_number);
  }
}


/* ================================================== */

#define BUFLEN 2047
#define SBUFLEN "2047"

static void
parse_mailonchange(const char *line)
{
  char buffer[BUFLEN+1];
  if (sscanf(line, "%" SBUFLEN "s%lf", buffer, &mail_change_threshold) == 2) {
    mail_user_on_change = MallocArray(char, strlen(buffer)+1);
    strcpy(mail_user_on_change, buffer);
  } else {
    mail_user_on_change = NULL;
    LOG(LOGS_WARN, LOGF_Configure,
        "Could not read user or threshold for clock change mail notify at line %d\n",
        line_number);
  }
}

/* ================================================== */

static void
parse_allow_deny(const char *line, AllowDeny *list, int allow)
{
  const char *p;
  unsigned long a, b, c, d, n;
  int all = 0;
  AllowDeny *new_node = NULL;
  IPAddr ip_addr;

  p = line;

  while (*p && isspace((unsigned char)*p)) p++;

  if (!strncmp(p, "all", 3)) {
    all = 1;
    p += 3;
  }

  while (*p && isspace((unsigned char)*p)) p++;
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
          LOG(LOGS_WARN, LOGF_Configure, "Could not read subnet size at line %d", line_number);
        }
      }

    } else {
      if (DNS_Name2IPAddress(p, &ip_addr, 1)) {
        new_node = MallocNew(AllowDeny);
        new_node->allow = allow;
        new_node->all = all;
        new_node->ip = ip_addr;
        if (ip_addr.family == IPADDR_INET6)
          new_node->subnet_bits = 128;
        else
          new_node->subnet_bits = 32;
      } else {
        LOG(LOGS_WARN, LOGF_Configure, "Could not read address at line %d", line_number);
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
parse_allow(const char *line)
{
  parse_allow_deny(line, &ntp_auth_list, 1);
}


/* ================================================== */

static void
parse_deny(const char *line)
{
  parse_allow_deny(line, &ntp_auth_list, 0);
}

/* ================================================== */

static void
parse_cmdallow(const char *line)
{
  parse_allow_deny(line, &cmd_auth_list, 1);
}


/* ================================================== */

static void
parse_cmddeny(const char *line)
{
  parse_allow_deny(line, &cmd_auth_list, 0);
}

/* ================================================== */

static void
parse_bindaddress(const char *line)
{
  IPAddr ip;
  char addr[51];

  if (sscanf(line, "%50s", addr) == 1 && UTI_StringToIP(addr, &ip)) {
    if (ip.family == IPADDR_INET4)
      bind_address4 = ip;
    else if (ip.family == IPADDR_INET6)
      bind_address6 = ip;
  } else {
    LOG(LOGS_WARN, LOGF_Configure, "Could not read bind address at line %d\n", line_number);
  }
}

/* ================================================== */

static void
parse_bindcmdaddress(const char *line)
{
  IPAddr ip;
  char addr[51];

  if (sscanf(line, "%50s", addr) == 1 && UTI_StringToIP(addr, &ip)) {
    if (ip.family == IPADDR_INET4)
      bind_cmd_address4 = ip;
    else if (ip.family == IPADDR_INET6)
      bind_cmd_address6 = ip;
  } else {
    LOG(LOGS_WARN, LOGF_Configure, "Could not read bind command address at line %d\n", line_number);
  }
}

/* ================================================== */

static void
parse_pidfile(const char *line)
{
  pidfile = MallocArray(char, 1 + strlen(line));
  sscanf(line, "%s", pidfile);
  strip_trailing_spaces(pidfile);
}  

/* ================================================== */

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

static void
parse_broadcast(const char *line)
{
  /* Syntax : broadcast <interval> <broadcast-IP-addr> [<port>] */
  int port;
  int n;
  int interval;
  char addr[51];
  IPAddr ip;
  
  n = sscanf(line, "%d %50s %d", &interval, addr, &port);
  if (n < 2 || !UTI_StringToIP(addr, &ip)) {
    LOG(LOGS_WARN, LOGF_Configure, "Could not parse broadcast directive at line %d", line_number);
    return;
  } else if (n == 2) {
    /* default port */
    port = 123;
  } else if (n > 3) {
    LOG(LOGS_WARN, LOGF_Configure, "Too many fields in broadcast directive at line %d", line_number);
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
parse_linux_hz(const char *line)
{
  if (1 == sscanf(line, "%d", &linux_hz)) {
    set_linux_hz = 1;
  } else {
    LOG(LOGS_WARN, LOGF_Configure, "Could not parse linux_hz directive at line %d", line_number);
  }
}

/* ================================================== */

static void
parse_linux_freq_scale(const char *line)
{
  if (1 == sscanf(line, "%lf", &linux_freq_scale)) {
    set_linux_freq_scale = 1;
  } else {
    LOG(LOGS_WARN, LOGF_Configure, "Could not parse linux_freq_scale directive at line %d", line_number);
  }
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
  NTP_Remote_Address server;
  int i;

  for (i=0; i<n_ntp_sources; i++) {
    server.ip_addr = ntp_sources[i].ip_addr;
    memset(&server.local_ip_addr, 0, sizeof (server.local_ip_addr));
    server.port = ntp_sources[i].port;

    switch (ntp_sources[i].type) {
      case SERVER:
        NSR_AddServer(&server, &ntp_sources[i].params);
        break;

      case PEER:
        NSR_AddPeer(&server, &ntp_sources[i].params);
        break;
    }

  }

  return;

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

char *
CNF_GetKeysFile(void)
{
  return keys_file;
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
CNF_GetRTCOnUTC(void)
{
  return rtc_on_utc;
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
      LOG(LOGS_WARN, LOGF_Configure, "Bad subnet for %08lx", node->ip);
    }
  }

  for (node = cmd_auth_list.next; node != &cmd_auth_list; node = node->next) {
    status = CAM_AddAccessRestriction(&node->ip, node->subnet_bits, node->allow, node->all);
    if (!status) {
      LOG(LOGS_WARN, LOGF_Configure, "Bad subnet for %08lx", node->ip);
    }
  }

  return;
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
