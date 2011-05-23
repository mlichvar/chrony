/*
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
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 * 
 **********************************************************************

  =======================================================================
  
  Module for parsing various forms of directive and command lines that
  are common to the configuration file and to the command client.

  */

#include "config.h"

#include "sysincl.h"

#include "cmdparse.h"
#include "memory.h"
#include "nameserv.h"

#define MAXLEN 2047
#define SMAXLEN "2047"

/* ================================================== */

CPS_Status
CPS_ParseNTPSourceAdd(const char *line, CPS_NTP_Source *src)
{
  int ok, n, done;
  char cmd[MAXLEN+1], hostname[MAXLEN+1];
  CPS_Status result;
  DNS_Status s;
  
  src->port = SRC_DEFAULT_PORT;
  src->params.minpoll = SRC_DEFAULT_MINPOLL;
  src->params.maxpoll = SRC_DEFAULT_MAXPOLL;
  src->params.presend_minpoll = SRC_DEFAULT_PRESEND_MINPOLL;
  src->params.authkey = INACTIVE_AUTHKEY;
  src->params.max_delay = SRC_DEFAULT_MAXDELAY;
  src->params.max_delay_ratio = SRC_DEFAULT_MAXDELAYRATIO;
  src->params.max_delay_dev_ratio = SRC_DEFAULT_MAXDELAYDEVRATIO;
  src->params.online = 1;
  src->params.auto_offline = 0;
  src->params.iburst = 0;
  src->params.min_stratum = SRC_DEFAULT_MINSTRATUM;
  src->params.poll_target = SRC_DEFAULT_POLLTARGET;
  src->params.sel_option = SRC_SelectNormal;

  result = CPS_Success;
  
  ok = 0;
  if (sscanf(line, "%" SMAXLEN "s%n", hostname, &n) == 1) {
    s = DNS_Name2IPAddress(hostname, &src->ip_addr);
    if (s == DNS_Success) {
      ok = 1;
      src->name = NULL;
    } else if (s == DNS_TryAgain) {
      ok = 1;
      src->ip_addr.family = IPADDR_UNSPEC;
    }
  }

  if (!ok) {
    result = CPS_BadHost;
  } else {

    line += n;
    
    /* Parse subfields */
    ok = 1;
    done = 0;
    do {
      if (sscanf(line, "%" SMAXLEN "s%n", cmd, &n) == 1) {
        
        line += n;
        
        if (!strncasecmp(cmd, "port", 4)) {
          if (sscanf(line, "%hu%n", &src->port, &n) != 1) {
            result = CPS_BadPort;
            ok = 0;
            done = 1;
          } else {
            line += n;
          }
        } else if (!strncasecmp(cmd, "minpoll", 7)) {
          if (sscanf(line, "%d%n", &src->params.minpoll, &n) != 1) {
            result = CPS_BadMinpoll;
            ok = 0;
            done = 1;
          } else {
            line += n;
          }
        } else if (!strncasecmp(cmd, "maxpoll", 7)) {
          if (sscanf(line, "%d%n", &src->params.maxpoll, &n) != 1) {
            result = CPS_BadMaxpoll;
            ok = 0;
            done = 1;
          } else {
            line += n;
          }
        } else if (!strncasecmp(cmd, "presend", 7)) {
          if (sscanf(line, "%d%n", &src->params.presend_minpoll, &n) != 1) {
            result = CPS_BadPresend;
            ok = 0;
            done = 1;
          } else {
            line += n;
          }
        } else if (!strncasecmp(cmd, "maxdelaydevratio", 16)) {
          if (sscanf(line, "%lf%n", &src->params.max_delay_dev_ratio, &n) != 1) {
            result = CPS_BadMaxdelaydevratio;
            ok = 0;
            done = 1;
          } else {
            line += n;
          }
          /* This MUST come before the following one ! */
        } else if (!strncasecmp(cmd, "maxdelayratio", 13)) {
          if (sscanf(line, "%lf%n", &src->params.max_delay_ratio, &n) != 1) {
            result = CPS_BadMaxdelayratio;
            ok = 0;
            done = 1;
          } else {
            line += n;
          }
        } else if (!strncasecmp(cmd, "maxdelay", 8)) {
          if (sscanf(line, "%lf%n", &src->params.max_delay, &n) != 1) {
            result = CPS_BadMaxdelay;
            ok = 0;
            done = 1;
          } else {
            line += n;
          }
        } else if (!strncasecmp(cmd, "key", 3)) {
          if (sscanf(line, "%lu%n", &src->params.authkey, &n) != 1) {
            result = CPS_BadKey;
            ok = 0;
            done = 1;
          } else {
            line += n;
          }
        } else if (!strncasecmp(cmd, "offline", 7)) {
          src->params.online = 0;

        } else if (!strncasecmp(cmd, "auto_offline", 12)) {
          src->params.auto_offline = 1;
        
        } else if (!strncasecmp(cmd, "iburst", 6)) {
          src->params.iburst = 1;

        } else if (!strncasecmp(cmd, "minstratum", 10)) {
          if (sscanf(line, "%d%n", &src->params.min_stratum, &n) != 1) {
            result = CPS_BadMinstratum;
            ok = 0;
            done = 1;
          } else {
            line += n;
          }

        } else if (!strncasecmp(cmd, "polltarget", 10)) {
          if (sscanf(line, "%d%n", &src->params.poll_target, &n) != 1) {
            result = CPS_BadPolltarget;
            ok = 0;
            done = 1;
          } else {
            line += n;
          }

        } else if (!strncasecmp(cmd, "noselect", 8)) {
          src->params.sel_option = SRC_SelectNoselect;
        
        } else if (!strncasecmp(cmd, "prefer", 6)) {
          src->params.sel_option = SRC_SelectPrefer;
        
        } else {
          result = CPS_BadOption;
          ok = 0;
          done = 1;
        }
      } else {
        done = 1;
      }
    } while (!done);
  }

  if (ok && src->ip_addr.family == IPADDR_UNSPEC) {
    n = strlen(hostname);
    src->name = MallocArray(char, n + 1);
    strncpy(src->name, hostname, n);
    src->name[n] = '\0';
  }

  return result;
    
}

/* ================================================== */

