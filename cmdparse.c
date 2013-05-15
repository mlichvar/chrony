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
#include "util.h"

/* ================================================== */

CPS_Status
CPS_ParseNTPSourceAdd(char *line, CPS_NTP_Source *src)
{
  char *hostname, *cmd;
  int ok, n, done;
  CPS_Status result;
  
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
  
  hostname = line;
  line = CPS_SplitWord(line);

  if (!*hostname) {
    result = CPS_BadHost;
    ok = 0;
  } else {
    /* Parse subfields */
    ok = 1;
    done = 0;
    do {
      cmd = line;
      line = CPS_SplitWord(line);

      if (*cmd) {
        if (!strcasecmp(cmd, "port")) {
          if (sscanf(line, "%hu%n", &src->port, &n) != 1) {
            result = CPS_BadPort;
            ok = 0;
            done = 1;
          } else {
            line += n;
          }
        } else if (!strcasecmp(cmd, "minpoll")) {
          if (sscanf(line, "%d%n", &src->params.minpoll, &n) != 1) {
            result = CPS_BadMinpoll;
            ok = 0;
            done = 1;
          } else {
            line += n;
          }
        } else if (!strcasecmp(cmd, "maxpoll")) {
          if (sscanf(line, "%d%n", &src->params.maxpoll, &n) != 1) {
            result = CPS_BadMaxpoll;
            ok = 0;
            done = 1;
          } else {
            line += n;
          }
        } else if (!strcasecmp(cmd, "presend")) {
          if (sscanf(line, "%d%n", &src->params.presend_minpoll, &n) != 1) {
            result = CPS_BadPresend;
            ok = 0;
            done = 1;
          } else {
            line += n;
          }
        } else if (!strcasecmp(cmd, "maxdelaydevratio")) {
          if (sscanf(line, "%lf%n", &src->params.max_delay_dev_ratio, &n) != 1) {
            result = CPS_BadMaxdelaydevratio;
            ok = 0;
            done = 1;
          } else {
            line += n;
          }
        } else if (!strcasecmp(cmd, "maxdelayratio")) {
          if (sscanf(line, "%lf%n", &src->params.max_delay_ratio, &n) != 1) {
            result = CPS_BadMaxdelayratio;
            ok = 0;
            done = 1;
          } else {
            line += n;
          }
        } else if (!strcasecmp(cmd, "maxdelay")) {
          if (sscanf(line, "%lf%n", &src->params.max_delay, &n) != 1) {
            result = CPS_BadMaxdelay;
            ok = 0;
            done = 1;
          } else {
            line += n;
          }
        } else if (!strcasecmp(cmd, "key")) {
          if (sscanf(line, "%lu%n", &src->params.authkey, &n) != 1) {
            result = CPS_BadKey;
            ok = 0;
            done = 1;
          } else {
            line += n;
          }
        } else if (!strcasecmp(cmd, "offline")) {
          src->params.online = 0;

        } else if (!strcasecmp(cmd, "auto_offline")) {
          src->params.auto_offline = 1;
        
        } else if (!strcasecmp(cmd, "iburst")) {
          src->params.iburst = 1;

        } else if (!strcasecmp(cmd, "minstratum")) {
          if (sscanf(line, "%d%n", &src->params.min_stratum, &n) != 1) {
            result = CPS_BadMinstratum;
            ok = 0;
            done = 1;
          } else {
            line += n;
          }

        } else if (!strcasecmp(cmd, "polltarget")) {
          if (sscanf(line, "%d%n", &src->params.poll_target, &n) != 1) {
            result = CPS_BadPolltarget;
            ok = 0;
            done = 1;
          } else {
            line += n;
          }

        } else if (!strcasecmp(cmd, "noselect")) {
          src->params.sel_option = SRC_SelectNoselect;
        
        } else if (!strcasecmp(cmd, "prefer")) {
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

  if (ok) {
    src->name = strdup(hostname);
  }

  return result;
    
}

/* ================================================== */

void
CPS_NormalizeLine(char *line)
{
  char *p, *q;
  int space = 1, first = 1;

  /* Remove white-space at beginning and replace white-spaces with space char */
  for (p = q = line; *p; p++) {
    if (isspace(*p)) {
      if (!space)
        *q++ = ' ';
      space = 1;
      continue;
    }

    /* Discard comment lines */
    if (first && strchr("!;#%", *p))
      break;

    *q++ = *p;
    space = first = 0;
  }

  /* Strip trailing space */
  if (q > line && q[-1] == ' ')
    q--;

  *q = '\0';
}

/* ================================================== */

char *
CPS_SplitWord(char *line)
{
  char *p = line, *q = line;

  /* Skip white-space before the word */
  while (*q && isspace(*q))
    q++;

  /* Move the word to the beginning */
  while (*q && !isspace(*q))
    *p++ = *q++;

  /* Find the next word */
  while (*q && isspace(*q))
    q++;

  *p = '\0';

  /* Return pointer to the next word or NUL */
  return q;
}

/* ================================================== */

int
CPS_ParseKey(char *line, unsigned long *id, const char **hash, char **key)
{
  char *s1, *s2, *s3, *s4;

  s1 = line;
  s2 = CPS_SplitWord(s1);
  s3 = CPS_SplitWord(s2);
  s4 = CPS_SplitWord(s3);

  /* Require two or three words */
  if (!*s2 || *s4)
    return 0;

  if (sscanf(s1, "%lu", id) != 1)
    return 0;

  if (*s3) {
    *hash = s2;
    *key = s3;
  } else {
    *hash = "MD5";
    *key = s2;
  }

  return 1;
}
