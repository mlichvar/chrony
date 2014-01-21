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

  Command line client for configuring the daemon and obtaining status
  from it whilst running.
  */

#include "config.h"

#include "sysincl.h"

#include "candm.h"
#include "nameserv.h"
#include "hash.h"
#include "getdate.h"
#include "cmdparse.h"
#include "pktlength.h"
#include "memory.h"
#include "util.h"

#ifdef FEAT_READLINE
#ifdef USE_EDITLINE
#include <editline/readline.h>
#else
#include <readline/readline.h>
#include <readline/history.h>
#endif
#endif

/* ================================================== */

union sockaddr_in46 {
  struct sockaddr_in in4;
#ifdef HAVE_IPV6
  struct sockaddr_in6 in6;
#endif
  struct sockaddr u;
};

static int sock_fd;
union sockaddr_in46 his_addr;
static socklen_t his_addr_len;

static int on_terminal = 0;

static int no_dns = 0;

/* ================================================== */
/* Ought to extract some code from util.c to make
   a new set of utilities that can be linked into either
   the daemon or the client. */

static char *
time_to_log_form(time_t t)
{
  struct tm stm;
  static char buffer[64];
  static const char *months[] = {"Jan", "Feb", "Mar", "Apr",
                                 "May", "Jun", "Jul", "Aug",
                                 "Sep", "Oct", "Nov", "Dec"};


  stm = *gmtime(&t);
  snprintf(buffer, sizeof(buffer),
          "%2d%s%02d %02d:%02d:%02d",
          stm.tm_mday, months[stm.tm_mon], stm.tm_year % 100,
          stm.tm_hour, stm.tm_min, stm.tm_sec);

  return buffer;
}

/* ================================================== */
/* Read a single line of commands from standard input.  Eventually we
   might want to use the GNU readline library. */

static char *
read_line(void)
{
  static char line[2048];
  static const char *prompt = "chronyc> ";

  if (on_terminal) {
#ifdef FEAT_READLINE
    char *cmd;

    /* save line only if not empty */
    cmd = readline(prompt);
    if( cmd == NULL ) return( NULL );
    
    /* user pressed return */
    if( *cmd != '\0' ) {
      strncpy(line, cmd, sizeof(line) - 1);
      line[sizeof(line) - 1] = '\0';
      add_history(cmd);
      /* free the buffer allocated by readline */
      free(cmd);
    } else {
      /* simulate the user has entered an empty line */
      *line = '\0';
    }
    return( line );
#else
    printf("%s", prompt);
#endif
  }
  if (fgets(line, sizeof(line), stdin)) {
    return line;
  } else {
    return NULL;
  }
  
}

/* ================================================== */
/* Initialise the socket used to talk to the daemon */

static void
open_io(const char *hostname, int port)
{
  IPAddr ip;

  /* Note, this call could block for a while */
  if (DNS_Name2IPAddress(hostname, &ip) != DNS_Success) {
    fprintf(stderr, "Could not get IP address for %s\n", hostname);
    exit(1);
  }

  memset(&his_addr, 0, sizeof (his_addr));

  switch (ip.family) {
    case IPADDR_INET4:
      sock_fd = socket(AF_INET, SOCK_DGRAM, 0);

      his_addr.in4.sin_family = AF_INET;
      his_addr.in4.sin_addr.s_addr = htonl(ip.addr.in4);
      his_addr.in4.sin_port = htons(port);
      his_addr_len = sizeof (his_addr.in4);
      break;
#ifdef HAVE_IPV6
    case IPADDR_INET6:
      sock_fd = socket(AF_INET6, SOCK_DGRAM, 0);

      his_addr.in6.sin6_family = AF_INET6;
      memcpy(his_addr.in6.sin6_addr.s6_addr, ip.addr.in6,
          sizeof (his_addr.in6.sin6_addr.s6_addr));
      his_addr.in6.sin6_port = htons(port);
      his_addr_len = sizeof (his_addr.in6);
      break;
#endif
    default:
      assert(0);
  }

  if (sock_fd < 0) {
    perror("Can't create socket");
    exit(1);
  }
}

/* ================================================== */

static void
close_io(void)
{

  close(sock_fd);

}

/* ================================================== */

static void
bits_to_mask(int bits, int family, IPAddr *mask)
{
  int i;

  mask->family = family;
  switch (family) {
    case IPADDR_INET4:
      if (bits < 0)
        bits = 32;
      if (bits > 0) {
        mask->addr.in4 = -1;
        mask->addr.in4 <<= 32 - bits;
      } else {
        mask->addr.in4 = 0;
      }
      break;
    case IPADDR_INET6:
      if (bits > 128 || bits < 0)
        bits = 128;
      for (i = 0; i < bits / 8; i++)
        mask->addr.in6[i] = 0xff;
      if (i < 16)
        mask->addr.in6[i++] = (0xff << (8 - bits % 8)) & 0xff;
      for (; i < 16; i++)
        mask->addr.in6[i] = 0x0;
      break;
    default:
      assert(0);
  }
}

/* ================================================== */

static int
read_mask_address(char *line, IPAddr *mask, IPAddr *address)
{
  unsigned int bits;
  char *p, *q;

  p = line;
  if (!*p) {
    mask->family = address->family = IPADDR_UNSPEC;
    return 1;
  } else {
    q = strchr(p, '/');
    if (q) {
      *q++ = 0;
      if (UTI_StringToIP(p, mask)) {
        p = q;
        if (UTI_StringToIP(p, address)) {
          if (address->family == mask->family)
            return 1;
        } else if (sscanf(p, "%u", &bits) == 1) {
          *address = *mask;
          bits_to_mask(bits, address->family, mask);
          return 1;
        }
      }
    } else {
      if (DNS_Name2IPAddress(p, address) == DNS_Success) {
        bits_to_mask(-1, address->family, mask);
        return 1;
      } else {
        fprintf(stderr, "Could not get address for hostname\n");
        return 0;
      }
    }
  }

  fprintf(stderr, "Invalid syntax for mask/address\n");
  return 0;
}

/* ================================================== */

static int
process_cmd_offline(CMD_Request *msg, char *line)
{
  IPAddr mask, address;
  int ok;

  if (read_mask_address(line, &mask, &address)) {
    UTI_IPHostToNetwork(&mask, &msg->data.offline.mask);
    UTI_IPHostToNetwork(&address, &msg->data.offline.address);
    msg->command = htons(REQ_OFFLINE);
    ok = 1;
  } else {
    ok = 0;
  }

  return ok;

}

/* ================================================== */


static int
process_cmd_online(CMD_Request *msg, char *line)
{
  IPAddr mask, address;
  int ok;

  if (read_mask_address(line, &mask, &address)) {
    UTI_IPHostToNetwork(&mask, &msg->data.online.mask);
    UTI_IPHostToNetwork(&address, &msg->data.online.address);
    msg->command = htons(REQ_ONLINE);
    ok = 1;
  } else {
    ok = 0;
  }

  return ok;

}

/* ================================================== */

static int
read_address_integer(char *line, IPAddr *address, int *value)
{
  char *hostname;
  int ok = 0;

  hostname = line;
  line = CPS_SplitWord(line);

  if (sscanf(line, "%d", value) != 1) {
    fprintf(stderr, "Invalid syntax for address value\n");
    ok = 0;
  } else {
    if (DNS_Name2IPAddress(hostname, address) != DNS_Success) {
      fprintf(stderr, "Could not get address for hostname\n");
      ok = 0;
    } else {
      ok = 1;
    }
  }

  return ok;

}


/* ================================================== */

static int
read_address_double(char *line, IPAddr *address, double *value)
{
  char *hostname;
  int ok = 0;

  hostname = line;
  line = CPS_SplitWord(line);

  if (sscanf(line, "%lf", value) != 1) {
    fprintf(stderr, "Invalid syntax for address value\n");
    ok = 0;
  } else {
    if (DNS_Name2IPAddress(hostname, address) != DNS_Success) {
      fprintf(stderr, "Could not get address for hostname\n");
      ok = 0;
    } else {
      ok = 1;
    }
  }

  return ok;

}


/* ================================================== */

static int
process_cmd_minpoll(CMD_Request *msg, char *line)
{
  IPAddr address;
  int minpoll;
  int ok;
  
  if (read_address_integer(line, &address, &minpoll)) {
    UTI_IPHostToNetwork(&address, &msg->data.modify_minpoll.address);
    msg->data.modify_minpoll.new_minpoll = htonl(minpoll);
    msg->command = htons(REQ_MODIFY_MINPOLL);
    ok = 1;
  } else {
    ok = 0;
  }

  return ok;

}

/* ================================================== */

static int
process_cmd_maxpoll(CMD_Request *msg, char *line)
{
  IPAddr address;
  int maxpoll;
  int ok;
  
  if (read_address_integer(line, &address, &maxpoll)) {
    UTI_IPHostToNetwork(&address, &msg->data.modify_maxpoll.address);
    msg->data.modify_maxpoll.new_maxpoll = htonl(maxpoll);
    msg->command = htons(REQ_MODIFY_MAXPOLL);
    ok = 1;
  } else {
    ok = 0;
  }

  return ok;

}

/* ================================================== */

static int
process_cmd_maxdelay(CMD_Request *msg, char *line)
{
  IPAddr address;
  double max_delay;
  int ok;
  
  if (read_address_double(line, &address, &max_delay)) {
    UTI_IPHostToNetwork(&address, &msg->data.modify_maxdelay.address);
    msg->data.modify_maxdelay.new_max_delay = UTI_FloatHostToNetwork(max_delay);
    msg->command = htons(REQ_MODIFY_MAXDELAY);
    ok = 1;
  } else {
    ok = 0;
  }

  return ok;

}

/* ================================================== */

static int
process_cmd_maxdelaydevratio(CMD_Request *msg, char *line)
{
  IPAddr address;
  double max_delay_dev_ratio;
  int ok;
  
  if (read_address_double(line, &address, &max_delay_dev_ratio)) {
    UTI_IPHostToNetwork(&address, &msg->data.modify_maxdelaydevratio.address);
    msg->data.modify_maxdelayratio.new_max_delay_ratio = UTI_FloatHostToNetwork(max_delay_dev_ratio);
    msg->command = htons(REQ_MODIFY_MAXDELAYDEVRATIO);
    ok = 1;
  } else {
    ok = 0;
  }

  return ok;

}

/* ================================================== */

static int
process_cmd_maxdelayratio(CMD_Request *msg, char *line)
{
  IPAddr address;
  double max_delay_ratio;
  int ok;
  
  if (read_address_double(line, &address, &max_delay_ratio)) {
    UTI_IPHostToNetwork(&address, &msg->data.modify_maxdelayratio.address);
    msg->data.modify_maxdelayratio.new_max_delay_ratio = UTI_FloatHostToNetwork(max_delay_ratio);
    msg->command = htons(REQ_MODIFY_MAXDELAYRATIO);
    ok = 1;
  } else {
    ok = 0;
  }

  return ok;

}

/* ================================================== */

static int
process_cmd_minstratum(CMD_Request *msg, char *line)
{
  IPAddr address;
  int min_stratum;
  int ok;
  
  if (read_address_integer(line, &address, &min_stratum)) {
    UTI_IPHostToNetwork(&address, &msg->data.modify_minstratum.address);
    msg->data.modify_minstratum.new_min_stratum = htonl(min_stratum);
    msg->command = htons(REQ_MODIFY_MINSTRATUM);
    ok = 1;
  } else {
    ok = 0;
  }

  return ok;

}

/* ================================================== */

static int
process_cmd_polltarget(CMD_Request *msg, char *line)
{
  IPAddr address;
  int poll_target;
  int ok;
  
  if (read_address_integer(line, &address, &poll_target)) {
    UTI_IPHostToNetwork(&address, &msg->data.modify_polltarget.address);
    msg->data.modify_polltarget.new_poll_target = htonl(poll_target);
    msg->command = htons(REQ_MODIFY_POLLTARGET);
    ok = 1;
  } else {
    ok = 0;
  }

  return ok;

}

/* ================================================== */

static int
process_cmd_maxupdateskew(CMD_Request *msg, char *line)
{
  int ok;
  double new_max_update_skew;
  
  if (sscanf(line, "%lf", &new_max_update_skew) == 1) {
    msg->data.modify_maxupdateskew.new_max_update_skew = UTI_FloatHostToNetwork(new_max_update_skew);
    msg->command = htons(REQ_MODIFY_MAXUPDATESKEW);
    ok = 1;
  } else {
    ok = 0;
  }

  return ok;

}

/* ================================================== */

static void
process_cmd_dump(CMD_Request *msg, char *line)
{
  msg->command = htons(REQ_DUMP);
  msg->data.dump.pad = htonl(0);
}

/* ================================================== */

static void
process_cmd_writertc(CMD_Request *msg, char *line)
{
  msg->command = htons(REQ_WRITERTC);
}

/* ================================================== */

static void
process_cmd_trimrtc(CMD_Request *msg, char *line)
{
  msg->command = htons(REQ_TRIMRTC);
}

/* ================================================== */

static void
process_cmd_cyclelogs(CMD_Request *msg, char *line)
{
  msg->command = htons(REQ_CYCLELOGS);
}

/* ================================================== */

static int
process_cmd_burst(CMD_Request *msg, char *line)
{
  int n_good_samples, n_total_samples;
  char *s1, *s2;
  IPAddr address, mask;

  s1 = line;
  s2 = CPS_SplitWord(s1);
  CPS_SplitWord(s2);

  if (sscanf(s1, "%d/%d", &n_good_samples, &n_total_samples) != 2) {
    fprintf(stderr, "Invalid syntax for burst command\n");
    return 0;
  }

  mask.family = address.family = IPADDR_UNSPEC;
  if (*s2 && !read_mask_address(s2, &mask, &address)) {
    return 0;
  }

  msg->command = htons(REQ_BURST);
  msg->data.burst.n_good_samples = ntohl(n_good_samples);
  msg->data.burst.n_total_samples = ntohl(n_total_samples);

  UTI_IPHostToNetwork(&mask, &msg->data.burst.mask);
  UTI_IPHostToNetwork(&address, &msg->data.burst.address);

  return 1;
}

/* ================================================== */

static int
process_cmd_local(CMD_Request *msg, const char *line)
{
  const char *p;
  int stratum;

  p = line;
  
  if (!strcmp(p, "off")) {
    msg->data.local.on_off = htonl(0);
    msg->data.local.stratum = htonl(0);
  } else if (sscanf(p, "stratum%d", &stratum) == 1) {
    msg->data.local.on_off = htonl(1);
    msg->data.local.stratum = htonl(stratum);
  } else {
    fprintf(stderr, "Invalid syntax for local command\n");
    return 0;
  }

  msg->command = htons(REQ_LOCAL);
  return 1;
}

/* ================================================== */

static int
process_cmd_manual(CMD_Request *msg, const char *line)
{
  const char *p;

  p = line;

  if (!strcmp(p, "off")) {
    msg->data.manual.option = htonl(0);
  } else if (!strcmp(p, "on")) {
    msg->data.manual.option = htonl(1);
  } else if (!strcmp(p, "reset")) {
    msg->data.manual.option = htonl(2);
  } else {
    fprintf(stderr, "Invalid syntax for manual command\n");
    return 0;
  }
  msg->command = htons(REQ_MANUAL);

  return 1;
}

/* ================================================== */

static int
parse_allow_deny(CMD_Request *msg, char *line)
{
  unsigned long a, b, c, d, n;
  IPAddr ip;
  char *p;
  
  p = line;
  if (!*p) {
    /* blank line - applies to all addresses */
    ip.family = IPADDR_UNSPEC;
    UTI_IPHostToNetwork(&ip, &msg->data.allow_deny.ip);
    msg->data.allow_deny.subnet_bits = htonl(0);
  } else {
    char *slashpos;
    slashpos = strchr(p, '/');
    if (slashpos) *slashpos = 0;
    
    n = 0;
    if (!UTI_StringToIP(p, &ip) &&
        (n = sscanf(p, "%lu.%lu.%lu.%lu", &a, &b, &c, &d)) == 0) {

      /* Try to parse as the name of a machine */
      if (DNS_Name2IPAddress(p, &ip) != DNS_Success) {
        fprintf(stderr, "Could not read address\n");
        return 0;
      } else {
        UTI_IPHostToNetwork(&ip, &msg->data.allow_deny.ip);
        if (ip.family == IPADDR_INET6)
          msg->data.allow_deny.subnet_bits = htonl(128);
        else
          msg->data.allow_deny.subnet_bits = htonl(32);
      }        
    } else {
      
      if (n == 0) {
        if (ip.family == IPADDR_INET6)
          msg->data.allow_deny.subnet_bits = htonl(128);
        else
          msg->data.allow_deny.subnet_bits = htonl(32);
      } else {
        ip.family = IPADDR_INET4;

        a &= 0xff;
        b &= 0xff;
        c &= 0xff;
        d &= 0xff;
        
        switch (n) {
          case 1:
            ip.addr.in4 = htonl((a<<24));
            msg->data.allow_deny.subnet_bits = htonl(8);
            break;
          case 2:
            ip.addr.in4 = htonl((a<<24) | (b<<16));
            msg->data.allow_deny.subnet_bits = htonl(16);
            break;
          case 3:
            ip.addr.in4 = htonl((a<<24) | (b<<16) | (c<<8));
            msg->data.allow_deny.subnet_bits = htonl(24);
            break;
          case 4:
            ip.addr.in4 = htonl((a<<24) | (b<<16) | (c<<8) | d);
            msg->data.allow_deny.subnet_bits = htonl(32);
            break;
          default:
            assert(0);
        }
      }

      UTI_IPHostToNetwork(&ip, &msg->data.allow_deny.ip);

      if (slashpos) {
        int specified_subnet_bits, n;
        n = sscanf(slashpos+1, "%d", &specified_subnet_bits);
        if (n == 1) {
          msg->data.allow_deny.subnet_bits = htonl(specified_subnet_bits);
        } else {
          fprintf(stderr, "Warning: badly formatted subnet size, using %ld\n", (long) ntohl(msg->data.allow_deny.subnet_bits));
        }
      } 
    }
  }
  return 1;
}

/* ================================================== */

static int
process_cmd_allow(CMD_Request *msg, char *line)
{
  int status;
  msg->command = htons(REQ_ALLOW);
  status = parse_allow_deny(msg, line);
  return status;
}

/* ================================================== */

static int
process_cmd_allowall(CMD_Request *msg, char *line)
{
  int status;
  msg->command = htons(REQ_ALLOWALL);
  status = parse_allow_deny(msg, line);
  return status;
}

/* ================================================== */

static int
process_cmd_deny(CMD_Request *msg, char *line)
{
  int status;
  msg->command = htons(REQ_DENY);
  status = parse_allow_deny(msg, line);
  return status;
}

/* ================================================== */

static int
process_cmd_denyall(CMD_Request *msg, char *line)
{
  int status;
  msg->command = htons(REQ_DENYALL);
  status = parse_allow_deny(msg, line);
  return status;
}

/* ================================================== */

static int
process_cmd_cmdallow(CMD_Request *msg, char *line)
{
  int status;
  msg->command = htons(REQ_CMDALLOW);
  status = parse_allow_deny(msg, line);
  return status;
}

/* ================================================== */

static int
process_cmd_cmdallowall(CMD_Request *msg, char *line)
{
  int status;
  msg->command = htons(REQ_CMDALLOWALL);
  status = parse_allow_deny(msg, line);
  return status;
}

/* ================================================== */

static int
process_cmd_cmddeny(CMD_Request *msg, char *line)
{
  int status;
  msg->command = htons(REQ_CMDDENY);
  status = parse_allow_deny(msg, line);
  return status;
}

/* ================================================== */

static int
process_cmd_cmddenyall(CMD_Request *msg, char *line)
{
  int status;
  msg->command = htons(REQ_CMDDENYALL);
  status = parse_allow_deny(msg, line);
  return status;
}

/* ================================================== */

static int
accheck_getaddr(char *line, IPAddr *addr)
{
  unsigned long a, b, c, d;
  IPAddr ip;
  char *p;
  p = line;
  if (!*p) {
    return 0;
  } else {
    if (sscanf(p, "%lu.%lu.%lu.%lu", &a, &b, &c, &d) == 4) {
      addr->family = IPADDR_INET4;
      addr->addr.in4 = (a<<24) | (b<<16) | (c<<8) | d;
      return 1;
    } else {
      if (DNS_Name2IPAddress(p, &ip) != DNS_Success) {
        return 0;
      } else {
        *addr = ip;
        return 1;
      }
    }
  }
}

/* ================================================== */

static int
process_cmd_accheck(CMD_Request *msg, char *line)
{
  IPAddr ip;
  msg->command = htons(REQ_ACCHECK);
  if (accheck_getaddr(line, &ip)) {
    UTI_IPHostToNetwork(&ip, &msg->data.ac_check.ip);
    return 1;
  } else {    
    fprintf(stderr, "Could not read address\n");
    return 0;
  }
}

/* ================================================== */

static int
process_cmd_cmdaccheck(CMD_Request *msg, char *line)
{
  IPAddr ip;
  msg->command = htons(REQ_CMDACCHECK);
  if (accheck_getaddr(line, &ip)) {
    UTI_IPHostToNetwork(&ip, &msg->data.ac_check.ip);
    return 1;
  } else {    
    fprintf(stderr, "Could not read address\n");
    return 0;
  }
}

/* ================================================== */

static void
process_cmd_dfreq(CMD_Request *msg, char *line)
{
  double dfreq;
  msg->command = htons(REQ_DFREQ);
  if (sscanf(line, "%lf", &dfreq) == 1) {
    msg->data.dfreq.dfreq = UTI_FloatHostToNetwork(dfreq);
  } else {
    msg->data.dfreq.dfreq = UTI_FloatHostToNetwork(0.0);
  }
}

/* ================================================== */

static void
cvt_to_sec_usec(double x, long *sec, long *usec) {
  long s, us;
  s = (long) x;
  us = (long)(0.5 + 1.0e6 * (x - (double) s));
  while (us >= 1000000) {
    us -= 1000000;
    s += 1;
  }
  while (us < 0) {
    us += 1000000;
    s -= 1;
  }
  
  *sec = s;
  *usec = us;
}

/* ================================================== */

static void
process_cmd_doffset(CMD_Request *msg, char *line)
{
  double doffset;
  long sec, usec;
  msg->command = htons(REQ_DOFFSET);
  if (sscanf(line, "%lf", &doffset) == 1) {
    cvt_to_sec_usec(doffset, &sec, &usec);
    msg->data.doffset.sec = htonl(sec);
    msg->data.doffset.usec = htonl(usec);
  } else {
    msg->data.doffset.sec = htonl(0);
    msg->data.doffset.usec = htonl(0);
  }
}

/* ================================================== */

static int
process_cmd_add_server_or_peer(CMD_Request *msg, char *line)
{
  CPS_NTP_Source data;
  CPS_Status status;
  IPAddr ip_addr;
  int result = 0;
  
  status = CPS_ParseNTPSourceAdd(line, &data);
  switch (status) {
    case CPS_Success:
      if (DNS_Name2IPAddress(data.name, &ip_addr) != DNS_Success) {
        Free(data.name);
        fprintf(stderr, "Invalid host/IP address\n");
        break;
      }
      Free(data.name);

      if (data.params.min_stratum != SRC_DEFAULT_MINSTRATUM) {
        fprintf(stderr, "Option minstratum not supported\n");
        break;
      }

      if (data.params.poll_target != SRC_DEFAULT_POLLTARGET) {
        fprintf(stderr, "Option polltarget not supported\n");
        break;
      }

      if (data.params.max_delay_dev_ratio != SRC_DEFAULT_MAXDELAYDEVRATIO) {
        fprintf(stderr, "Option maxdelaydevratio not supported\n");
        break;
      }

      msg->data.ntp_source.port = htonl((unsigned long) data.port);
      UTI_IPHostToNetwork(&ip_addr, &msg->data.ntp_source.ip_addr);
      msg->data.ntp_source.minpoll = htonl(data.params.minpoll);
      msg->data.ntp_source.maxpoll = htonl(data.params.maxpoll);
      msg->data.ntp_source.presend_minpoll = htonl(data.params.presend_minpoll);
      msg->data.ntp_source.authkey = htonl(data.params.authkey);
      msg->data.ntp_source.max_delay = UTI_FloatHostToNetwork(data.params.max_delay);
      msg->data.ntp_source.max_delay_ratio = UTI_FloatHostToNetwork(data.params.max_delay_ratio);
      msg->data.ntp_source.flags = htonl(
          (data.params.online ? REQ_ADDSRC_ONLINE : 0) |
          (data.params.auto_offline ? REQ_ADDSRC_AUTOOFFLINE : 0) |
          (data.params.iburst ? REQ_ADDSRC_IBURST : 0) |
          (data.params.sel_option == SRC_SelectPrefer ? REQ_ADDSRC_PREFER : 0) |
          (data.params.sel_option == SRC_SelectNoselect ? REQ_ADDSRC_NOSELECT : 0));
      result = 1;

      break;
    case CPS_BadOption:
      fprintf(stderr, "Unrecognized subcommand\n");
      break;
    case CPS_BadHost:
      fprintf(stderr, "Invalid host/IP address\n");
      break;
    case CPS_BadPort:
      fprintf(stderr, "Unreadable port number\n");
      break;
    case CPS_BadMinpoll:
      fprintf(stderr, "Unreadable minpoll value\n");
      break;
    case CPS_BadMaxpoll:
      fprintf(stderr, "Unreadable maxpoll value\n");
      break;
    case CPS_BadPresend:
      fprintf(stderr, "Unreadable presend value\n");
      break;
    case CPS_BadMaxdelaydevratio:
      fprintf(stderr, "Unreadable max delay dev ratio value\n");
      break;
    case CPS_BadMaxdelayratio:
      fprintf(stderr, "Unreadable max delay ratio value\n");
      break;
    case CPS_BadMaxdelay:
      fprintf(stderr, "Unreadable max delay value\n");
      break;
    case CPS_BadKey:
      fprintf(stderr, "Unreadable key value\n");
      break;
    case CPS_BadMinstratum:
      fprintf(stderr, "Unreadable minstratum value\n");
      break;
    case CPS_BadPolltarget:
      fprintf(stderr, "Unreadable polltarget value\n");
      break;
  }

  return result;
}

/* ================================================== */

static int
process_cmd_add_server(CMD_Request *msg, char *line)
{
  msg->command = htons(REQ_ADD_SERVER);
  return process_cmd_add_server_or_peer(msg, line);
}

/* ================================================== */

static int
process_cmd_add_peer(CMD_Request *msg, char *line)
{
  msg->command = htons(REQ_ADD_PEER);
  return process_cmd_add_server_or_peer(msg, line);
}

/* ================================================== */

static int
process_cmd_delete(CMD_Request *msg, char *line)
{
  char *hostname;
  int ok = 0;
  IPAddr address;

  msg->command = htons(REQ_DEL_SOURCE);
  hostname = line;
  CPS_SplitWord(line);

  if (!*hostname) {
    fprintf(stderr, "Invalid syntax for address\n");
    ok = 0;
  } else {
    if (DNS_Name2IPAddress(hostname, &address) != DNS_Success) {
      fprintf(stderr, "Could not get address for hostname\n");
      ok = 0;
    } else {
      UTI_IPHostToNetwork(&address, &msg->data.del_source.ip_addr);
      ok = 1;
    }
  }

  return ok;
  
}

/* ================================================== */

static char *password = NULL;
static int password_length;
static int auth_hash_id;

/* ================================================== */

static int
process_cmd_password(CMD_Request *msg, char *line)
{
  char *p;
  struct timeval now;
  int i, len;

  /* Blank and free the old password */
  if (password) {
    for (i = 0; i < password_length; i++)
      password[i] = 0;
    free(password);
    password = NULL;
  }

  p = line;

  if (!*p) {
    /* blank line, prompt for password */
    p = getpass("Password: ");
  }

  if (!*p)
    return 0;

  len = strlen(p);
  password_length = UTI_DecodePasswordFromText(p);

  if (password_length > 0) {
    password = malloc(password_length);
    memcpy(password, p, password_length);
  }

  /* Erase the password from the input or getpass buffer */
  for (i = 0; i < len; i++)
    p[i] = 0;

  if (password_length <= 0) {
      fprintf(stderr, "Could not decode password\n");
      return 0;
  }

  if (gettimeofday(&now, NULL) < 0) {
    printf("500 - Could not read time of day\n");
    return 0;
  } else {
    msg->command = htons(REQ_LOGON); /* Just force a round trip so that we get tokens etc */
    UTI_TimevalHostToNetwork(&now, &msg->data.logon.ts);
    return 1;
  }
}

/* ================================================== */

static int
generate_auth(CMD_Request *msg)
{
  int data_len;

  data_len = PKL_CommandLength(msg);

  assert(auth_hash_id >= 0);

  return UTI_GenerateNTPAuth(auth_hash_id, (unsigned char *)password, password_length,
      (unsigned char *)msg, data_len, ((unsigned char *)msg) + data_len, sizeof (msg->auth));
}

/* ================================================== */

static int
check_reply_auth(CMD_Reply *msg, int len)
{
  int data_len;

  data_len = PKL_ReplyLength(msg);

  assert(auth_hash_id >= 0);

  return UTI_CheckNTPAuth(auth_hash_id, (unsigned char *)password, password_length,
      (unsigned char *)msg, data_len,
      ((unsigned char *)msg) + data_len, len - data_len);
}

/* ================================================== */

static void
give_help(void)
{
  printf("Commands:\n");
  printf("accheck <address> : Check whether NTP access is allowed to <address>\n");
  printf("activity : Check how many NTP sources are online/offline\n");
  printf("add peer <address> ... : Add a new NTP peer\n");
  printf("add server <address> ... : Add a new NTP server\n");
  printf("allow [<subnet-addr>] : Allow NTP access to that subnet as a default\n");
  printf("allow all [<subnet-addr>] : Allow NTP access to that subnet and all children\n");
  printf("burst <n-good>/<n-max> [<mask>/<masked-address>] : Start a rapid set of measurements\n");
  printf("clients : Report on clients that have accessed the server\n");
  printf("cmdaccheck <address> : Check whether command access is allowed to <address>\n");
  printf("cmdallow [<subnet-addr>] : Allow command access to that subnet as a default\n");
  printf("cmdallow all [<subnet-addr>] : Allow command access to that subnet and all children\n");
  printf("cmddeny [<subnet-addr>] : Deny command access to that subnet as a default\n");
  printf("cmddeny all [<subnet-addr>] : Deny command access to that subnet and all children\n");
  printf("cyclelogs : Close and re-open logs files\n");
  printf("delete <address> : Remove an NTP server or peer\n");
  printf("deny [<subnet-addr>] : Deny NTP access to that subnet as a default\n");
  printf("deny all [<subnet-addr>] : Deny NTP access to that subnet and all children\n");
  printf("dump : Dump all measurements to save files\n");
  printf("local off : Disable server capability for unsynchronised clock\n");
  printf("local stratum <stratum> : Enable server capability for unsynchronised clock\n");
  printf("makestep : Jump the time to remove any correction being slewed\n");
  printf("manual off|on|reset : Disable/enable/reset settime command and statistics\n");
  printf("manual list : Show previous settime entries\n");
  printf("maxdelay <address> <new-max-delay> : Modify maximum round-trip valid sample delay for source\n");
  printf("maxdelayratio <address> <new-max-ratio> : Modify max round-trip delay ratio for source\n");
  printf("maxdelaydevratio <address> <new-max-ratio> : Modify max round-trip delay dev ratio for source\n");
  printf("maxpoll <address> <new-maxpoll> : Modify maximum polling interval of source\n");
  printf("maxupdateskew <new-max-skew> : Modify maximum skew for a clock frequency update to be made\n");
  printf("minpoll <address> <new-minpoll> : Modify minimum polling interval of source\n");
  printf("minstratum <address> <new-min-stratum> : Modify minimum stratum of source\n");
  printf("offline [<mask>/<masked-address>] : Set sources in subnet to offline status\n");
  printf("online [<mask>/<masked-address>] : Set sources in subnet to online status\n");
  printf("password [<new-password>] : Set command authentication password\n");
  printf("polltarget <address> <new-poll-target> : Modify poll target of source\n");
  printf("reselect : Reselect synchronisation source\n");
  printf("rtcdata : Print current RTC performance parameters\n");
  printf("settime <date/time (e.g. Nov 21, 1997 16:30:05 or 16:30:05)> : Manually set the daemon time\n");
  printf("sources [-v] : Display information about current sources\n");
  printf("sourcestats [-v] : Display estimation information about current sources\n");
  printf("tracking : Display system time information\n");
  printf("trimrtc : Correct RTC relative to system clock\n");
  printf("waitsync [max-tries [max-correction [max-skew]]] : Wait until synchronised\n");
  printf("writertc : Save RTC parameters to file\n");
  printf("\n");
  printf("authhash <name>: Set command authentication hash function\n");
  printf("dns -n|+n : Disable/enable resolving IP addresses to hostnames\n");
  printf("dns -4|-6|-46 : Resolve hostnames only to IPv4/IPv6/both addresses\n");
  printf("timeout <milliseconds> : Set initial response timeout\n");
  printf("retries <n> : Set maximum number of retries\n");
  printf("exit|quit : Leave the program\n");
  printf("help : Generate this help\n");
  printf("\n");
}

/* ================================================== */

static unsigned long sequence = 0;
static unsigned long utoken = 0;
static unsigned long token = 0;

static int max_retries = 2;
static int initial_timeout = 1000;

/* This is the core protocol module.  Complete particular fields in
   the outgoing packet, send it, wait for a response, handle retries,
   etc.  Returns a Boolean indicating whether the protocol was
   successful or not.*/

static int
submit_request(CMD_Request *request, CMD_Reply *reply, int *reply_auth_ok)
{
  unsigned long tx_sequence;
  socklen_t where_from_len;
  union sockaddr_in46 where_from;
  int bad_length, bad_sender, bad_sequence, bad_header;
  int select_status;
  int recvfrom_status;
  int read_length;
  int expected_length;
  int command_length;
  int auth_length;
  struct timeval tv;
  int timeout;
  int n_attempts;
  fd_set rdfd, wrfd, exfd;

  request->version = PROTO_VERSION_NUMBER;
  request->pkt_type = PKT_TYPE_CMD_REQUEST;
  request->res1 = 0;
  request->res2 = 0;
  tx_sequence = sequence++;
  request->sequence = htonl(tx_sequence);
  request->attempt = 0;
  request->utoken = htonl(utoken);
  request->token = htonl(token);

  timeout = initial_timeout;

  n_attempts = 0;

  do {

    /* Decide whether to authenticate */
    if (password) {
      if (!utoken || (request->command == htons(REQ_LOGON))) {
        /* Otherwise, the daemon won't bother authenticating our
           packet and we won't get a token back */
        request->utoken = htonl(SPECIAL_UTOKEN);
      }
      auth_length = generate_auth(request);
    } else {
      auth_length = 0;
    }

    command_length = PKL_CommandLength(request);
    assert(command_length > 0);

    /* add empty MD5 auth so older servers will not drop the request
       due to bad length */
    if (!auth_length) {
      memset(((char *)request) + command_length, 0, 16);
      auth_length = 16;
    }

#if 0
    printf("Sent command length=%d bytes auth length=%d bytes\n", command_length, auth_length);
#endif

    if (sendto(sock_fd, (void *) request, command_length + auth_length, 0,
               &his_addr.u, his_addr_len) < 0) {


#if 0
      perror("Could not send packet");
#endif
      return 0;
    }

    /* Increment this for next time */
    ++ request->attempt;
      
    tv.tv_sec = timeout / 1000;
    tv.tv_usec = timeout % 1000 * 1000;
    timeout *= 2;

    FD_ZERO(&rdfd);
    FD_ZERO(&wrfd);
    FD_ZERO(&exfd);

    FD_SET(sock_fd, &rdfd);

    select_status = select(sock_fd + 1, &rdfd, &wrfd, &exfd, &tv);

    if (select_status < 0) {
#if 0
      perror("Select returned negative status");
#endif
    } else if (select_status == 0) {
      /* Timeout must have elapsed, try a resend? */
      n_attempts ++;
      if (n_attempts > max_retries) {
        return 0;
      }

      /* Back to top of loop to do resend */
      continue;
      
    } else {
      
      where_from_len = sizeof(where_from);
      recvfrom_status = recvfrom(sock_fd, (void *) reply, sizeof(CMD_Reply), 0,
                                 &where_from.u, &where_from_len);
      

#if 0
      printf("Received packet, status=%d\n", recvfrom_status);
#endif

      if (recvfrom_status < 0) {
        /* If we get connrefused here, it suggests the sendto is
           going to a dead port - but only if the daemon machine is
           running Linux (Solaris doesn't return anything) */
        n_attempts++;
        if (n_attempts > max_retries) {
          return 0;
        }
      } else {
        
        read_length = recvfrom_status;
        if (read_length >= offsetof(CMD_Reply, data)) {
          expected_length = PKL_ReplyLength(reply);
        } else {
          expected_length = 0;
        }

        bad_length = (read_length < expected_length ||
                      expected_length < offsetof(CMD_Reply, data));
        bad_sender = (where_from.u.sa_family != his_addr.u.sa_family ||
                      (where_from.u.sa_family == AF_INET &&
                       (where_from.in4.sin_addr.s_addr != his_addr.in4.sin_addr.s_addr ||
                        where_from.in4.sin_port != his_addr.in4.sin_port)) ||
#ifdef HAVE_IPV6
                      (where_from.u.sa_family == AF_INET6 &&
                       (memcmp(where_from.in6.sin6_addr.s6_addr, his_addr.in6.sin6_addr.s6_addr,
                               sizeof (where_from.in6.sin6_addr.s6_addr)) != 0 ||
                        where_from.in6.sin6_port != his_addr.in6.sin6_port)) ||
#endif
                      0);
        
        if (!bad_length) {
          bad_sequence = (ntohl(reply->sequence) != tx_sequence);
        } else {
          bad_sequence = 0;
        }
        
        if (bad_length || bad_sender || bad_sequence) {
          n_attempts++;
          if (n_attempts > max_retries) {
            return 0;
          }
          continue;
        }
        
        bad_header = ((reply->version != PROTO_VERSION_NUMBER &&
                       !(reply->version >= PROTO_VERSION_MISMATCH_COMPAT &&
                         ntohs(reply->status) == STT_BADPKTVERSION)) ||
                      (reply->pkt_type != PKT_TYPE_CMD_REPLY) ||
                      (reply->res1 != 0) ||
                      (reply->res2 != 0) ||
                      (reply->command != request->command));
        
        if (bad_header) {
          n_attempts++;
          if (n_attempts > max_retries) {
            return 0;
          }
          continue;
        }
        
        /* Good packet received, print out results */
#if 0
        printf("Reply cmd=%d reply=%d stat=%d seq=%d utok=%08lx tok=%d\n",
               ntohs(reply->command), ntohs(reply->reply),
               ntohs(reply->status),
               ntohl(reply->sequence),
               ntohl(reply->utoken),
               ntohl(reply->token));
#endif

        if (password) {
          *reply_auth_ok = check_reply_auth(reply, read_length);
        } else {
          /* Assume in this case that the reply is always considered
             to be authentic */
          *reply_auth_ok = 1;
        }
            
        utoken = ntohl(reply->utoken);

        if (*reply_auth_ok) {
          /* If we're in authenticated mode, only acquire the utoken
             and new token values if the reply authenticated properly.
             This protects against forged packets with bogus tokens
             in.  We won't accept a repeat of an old message with a
             stale token in it, due to bad_sequence processing
             earlier. */
          utoken = ntohl(reply->utoken);
          token = ntohl(reply->token);
        }
        
        break;

      }
    }
  } while (1);

  return 1;
}

/* ================================================== */

static int
request_reply(CMD_Request *request, CMD_Reply *reply, int requested_reply, int verbose)
{
  int reply_auth_ok;
  int status;

  if (!submit_request(request, reply, &reply_auth_ok)) {
    printf("506 Cannot talk to daemon\n");
    return 0;
  }

  status = ntohs(reply->status);
        
  if (verbose || status != STT_SUCCESS) {
    switch (status) {
      case STT_SUCCESS:
        printf("200 OK");
        break;
      case STT_ACCESSALLOWED:
        printf("208 Access allowed");
        break;
      case STT_ACCESSDENIED:
        printf("209 Access denied");
        break;
      case STT_FAILED:
        printf("500 Failure");
        break;
      case STT_UNAUTH:
        printf("501 Not authorised");
        break;
      case STT_INVALID:
        printf("502 Invalid command");
        break;
      case STT_NOSUCHSOURCE:
        printf("503 No such source");
        break;
      case STT_INVALIDTS:
        printf("504 Duplicate or stale logon detected");
        break;
      case STT_NOTENABLED:
        printf("505 Facility not enabled in daemon");
        break;
      case STT_BADSUBNET:
        printf("507 Bad subnet");
        break;
      case STT_NOHOSTACCESS:
        printf("510 No command access from this host");
        break;
      case STT_SOURCEALREADYKNOWN:
        printf("511 Source already present");
        break;
      case STT_TOOMANYSOURCES:
        printf("512 Too many sources present");
        break;
      case STT_NORTC:
        printf("513 RTC driver not running");
        break;
      case STT_BADRTCFILE:
        printf("514 Can't write RTC parameters");
        break;
      case STT_INVALIDAF:
        printf("515 Invalid address family");
        break;
      case STT_BADSAMPLE:
        printf("516 Sample index out of range");
        break;
      case STT_BADPKTVERSION:
        printf("517 Protocol version mismatch");
        break;
      case STT_BADPKTLENGTH:
        printf("518 Packet length mismatch");
        break;
      case STT_INACTIVE:
        printf("519 Client logging is not active in the daemon");
        break;
      default:
        printf("520 Got unexpected error from daemon");
    }
    if (reply_auth_ok) {
      printf("\n");
    } else {
      printf(" --- Reply not authenticated\n");
    }
  }
  
  if (status != STT_SUCCESS &&
      status != STT_ACCESSALLOWED && status != STT_ACCESSDENIED) {
    return 0;
  } 

  if (ntohs(reply->reply) != requested_reply) {
    printf("508 Bad reply from daemon\n");
    return 0;
  }

  return 1;
}

/* ================================================== */

static void
print_seconds(unsigned long s)
{
  unsigned long d;
  if (s <= 1024) {
    printf("%4ld", s);
  } else if (s < 36000) {
    printf("%3ldm", s / 60);
  } else if (s < 345600) {
    printf("%3ldh", s / 3600);
  } else {
    d = s / 86400;
    if (d > 999) {
      printf("%3ldy", d / 365);
    } else {
      printf("%3ldd", d);
    }
  }
}

/* ================================================== */

static void
print_nanoseconds(double s)
{
  s = fabs(s);

  if (s < 9999.5e-9) {
    printf("%4.0fns", s * 1e9);
  } else if (s < 9999.5e-6) {
    printf("%4.0fus", s * 1e6);
  } else if (s < 9999.5e-3) {
    printf("%4.0fms", s * 1e3);
  } else if (s < 999.5) {
    printf("%5.1fs", s);
  } else if (s < 99999.5) {
    printf("%5.0fs", s);
  } else if (s < 99999.5 * 60) {
    printf("%5.0fm", s / 60);
  } else if (s < 99999.5 * 3600) {
    printf("%5.0fh", s / 3600);
  } else if (s < 99999.5 * 3600 * 24) {
    printf("%5.0fd", s / (3600 * 24));
  } else {
    printf("%5.0fy", s / (3600 * 24 * 365));
  }
}

/* ================================================== */

static void
print_signed_nanoseconds(double s)
{
  double x;

  x = fabs(s);

  if (x < 9999.5e-9) {
    printf("%+5.0fns", s * 1e9);
  } else if (x < 9999.5e-6) {
    printf("%+5.0fus", s * 1e6);
  } else if (x < 9999.5e-3) {
    printf("%+5.0fms", s * 1e3);
  } else if (x < 999.5) {
    printf("%+6.1fs", s);
  } else if (x < 99999.5) {
    printf("%+6.0fs", s);
  } else if (x < 99999.5 * 60) {
    printf("%+6.0fm", s / 60);
  } else if (x < 99999.5 * 3600) {
    printf("%+6.0fh", s / 3600);
  } else if (x < 99999.5 * 3600 * 24) {
    printf("%+6.0fd", s / (3600 * 24));
  } else {
    printf("%+6.0fy", s / (3600 * 24 * 365));
  }
}

/* ================================================== */

static void
print_freq_ppm(double f)
{
  if (fabs(f) < 99999.5) {
    printf("%10.3f", f);
  } else {
    printf("%10.0f", f);
  }
}

/* ================================================== */

static int
check_for_verbose_flag(char *line)
{
  char *p = line;
  if (!strcmp(p, "-v")) {
    return 1;
  } else {
    return 0;
  }
}

/* ================================================== */

static int
process_cmd_sources(char *line)
{
  CMD_Request request;
  CMD_Reply reply;
  int n_sources, i;
  int verbose = 0;

  double orig_latest_meas, latest_meas, latest_meas_err;
  IPAddr ip_addr;
  uint32_t latest_meas_ago;
  uint16_t poll, stratum;
  uint16_t state, mode, flags, reachability;
  char hostname_buf[50];

  /* Check whether to output verbose headers */
  verbose = check_for_verbose_flag(line);
  
  request.command = htons(REQ_N_SOURCES);
  if (request_reply(&request, &reply, RPY_N_SOURCES, 0)) {
    n_sources = ntohl(reply.data.n_sources.n_sources);
    printf("210 Number of sources = %d\n", n_sources);
    if (verbose) {
      printf("\n");
      printf("  .-- Source mode  '^' = server, '=' = peer, '#' = local clock.\n");
      printf(" / .- Source state '*' = current synced, '+' = combined , '-' = not combined,\n");
      printf("| /   '?' = unreachable, 'x' = time may be in error, '~' = time too variable.\n");
      printf("||                                                 .- xxxx [ yyyy ] +/- zzzz\n");
      printf("||                                                /   xxxx = adjusted offset,\n");
      printf("||         Log2(Polling interval) -.             |    yyyy = measured offset,\n");
      printf("||                                  \\            |    zzzz = estimated error.\n");
      printf("||                                   |           |                         \n");
    }

    printf("MS Name/IP address         Stratum Poll Reach LastRx Last sample\n");
    printf("===============================================================================\n");

    /*     "MS NNNNNNNNNNNNNNNNNNNNNNNNNNN  SS  PP   RRR  RRRR  SSSSSSS[SSSSSSS] +/- SSSSSS" */

    for (i=0; i<n_sources; i++) {
      request.command = htons(REQ_SOURCE_DATA);
      request.data.source_data.index = htonl(i);
      if (request_reply(&request, &reply, RPY_SOURCE_DATA, 0)) {
          UTI_IPNetworkToHost(&reply.data.source_data.ip_addr, &ip_addr);
          poll = ntohs(reply.data.source_data.poll);
          stratum = ntohs(reply.data.source_data.stratum);
          state = ntohs(reply.data.source_data.state);
          mode = ntohs(reply.data.source_data.mode);
          flags = ntohs(reply.data.source_data.flags);
          reachability = ntohs(reply.data.source_data.reachability);
          latest_meas_ago = ntohl(reply.data.source_data.since_sample);
          orig_latest_meas = UTI_FloatNetworkToHost(reply.data.source_data.orig_latest_meas);
          latest_meas = UTI_FloatNetworkToHost(reply.data.source_data.latest_meas);
          latest_meas_err = UTI_FloatNetworkToHost(reply.data.source_data.latest_meas_err);

          if (mode == RPY_SD_MD_REF) {
            snprintf(hostname_buf, sizeof(hostname_buf), "%s", UTI_RefidToString(ip_addr.addr.in4));
          } else if (no_dns) {
            snprintf(hostname_buf, sizeof(hostname_buf), "%s", UTI_IPToString(&ip_addr));
          } else {
            DNS_IPAddress2Name(&ip_addr, hostname_buf, sizeof(hostname_buf));
            hostname_buf[25] = 0;
          }

          switch (mode) {
            case RPY_SD_MD_CLIENT:
              printf("^"); break;
            case RPY_SD_MD_PEER:
              printf("="); break;
            case RPY_SD_MD_REF:
              printf("#"); break;
            default:
              printf(" ");
          }
          switch (state) {
            case RPY_SD_ST_SYNC:
              printf("*"); break;
            case RPY_SD_ST_UNREACH:
              printf("?"); break;
            case RPY_SD_ST_FALSETICKER:
              printf("x"); break;
            case RPY_SD_ST_JITTERY:
              printf("~"); break;
            case RPY_SD_ST_CANDIDATE:
              printf("+"); break;
            case RPY_SD_ST_OUTLIER:
              printf("-"); break;
            default:
              printf(" ");
          }
          switch (flags) {
            default:
              break;
          }

          printf(" %-27s  %2d  %2d   %3o  ", hostname_buf, stratum, poll, reachability);
          print_seconds(latest_meas_ago);
          printf("  ");
          print_signed_nanoseconds(latest_meas);
          printf("[");
          print_signed_nanoseconds(orig_latest_meas);
          printf("]");
          printf(" +/- ");
          print_nanoseconds(latest_meas_err);
          printf("\n");
      } else {
        return 0;
      }
    }
  } else {
    return 0;
  }
  return 1;
}

/* ================================================== */

static int
process_cmd_sourcestats(char *line)
{
  CMD_Request request;
  CMD_Reply reply;
  int n_sources, i;
  int verbose = 0;

  char hostname_buf[50];
  unsigned long n_samples, n_runs, span_seconds;
  double resid_freq_ppm, skew_ppm, sd, est_offset;
  uint32_t ref_id;
  IPAddr ip_addr;

  verbose = check_for_verbose_flag(line);

  request.command = htons(REQ_N_SOURCES);
  if (request_reply(&request, &reply, RPY_N_SOURCES, 0)) {
    n_sources = ntohl(reply.data.n_sources.n_sources);
    printf("210 Number of sources = %d\n", n_sources);
    if (verbose) {
      printf("                             .- Number of sample points in measurement set.\n");
      printf("                            /    .- Number of residual runs with same sign.\n");
      printf("                           |    /    .- Length of measurement set (time).\n");
      printf("                           |   |    /      .- Est. clock freq error (ppm).\n");
      printf("                           |   |   |      /           .- Est. error in freq.\n");
      printf("                           |   |   |     |           /         .- Est. offset.\n");
      printf("                           |   |   |     |          |          |   On the -.\n");
      printf("                           |   |   |     |          |          |   samples. \\\n");
      printf("                           |   |   |     |          |          |             |\n");
    }

    printf("Name/IP Address            NP  NR  Span  Frequency  Freq Skew  Offset  Std Dev\n");
    printf("==============================================================================\n");

    /*      NNNNNNNNNNNNNNNNNNNNNNNNN  NP  NR  SSSS FFFFFFFFFF SSSSSSSSSS  SSSSSSS  SSSSSS*/

    for (i=0; i<n_sources; i++) {
      request.command = htons(REQ_SOURCESTATS);
      request.data.source_data.index = htonl(i);
      if (request_reply(&request, &reply, RPY_SOURCESTATS, 0)) {
          ref_id = ntohl(reply.data.sourcestats.ref_id);
          UTI_IPNetworkToHost(&reply.data.sourcestats.ip_addr, &ip_addr);
          n_samples = ntohl(reply.data.sourcestats.n_samples);
          n_runs = ntohl(reply.data.sourcestats.n_runs);
          span_seconds = ntohl(reply.data.sourcestats.span_seconds);
          resid_freq_ppm = UTI_FloatNetworkToHost(reply.data.sourcestats.resid_freq_ppm);
          skew_ppm = UTI_FloatNetworkToHost(reply.data.sourcestats.skew_ppm);
          sd = UTI_FloatNetworkToHost(reply.data.sourcestats.sd);
          est_offset = UTI_FloatNetworkToHost(reply.data.sourcestats.est_offset);
          /* est_offset_err = UTI_FloatNetworkToHost(reply.data.sourcestats.est_offset_err); */

          if (ip_addr.family == IPADDR_UNSPEC)
            snprintf(hostname_buf, sizeof(hostname_buf), "%s", UTI_RefidToString(ref_id));
          else if (no_dns) {
            snprintf(hostname_buf, sizeof(hostname_buf), "%s", UTI_IPToString(&ip_addr));
          } else {
            DNS_IPAddress2Name(&ip_addr, hostname_buf, sizeof(hostname_buf));
            hostname_buf[25] = 0;
          }

          printf("%-25s %3lu %3lu  ", hostname_buf, n_samples, n_runs);
          print_seconds(span_seconds);
          printf(" ");
          print_freq_ppm(resid_freq_ppm);
          printf(" ");
          print_freq_ppm(skew_ppm);
          printf("  ");
          print_signed_nanoseconds(est_offset);
          printf("  ");
          print_nanoseconds(sd);
          printf("\n");
      } else {
        return 0;
      }
    }
  } else {
    return 0;
  }
  return 1;
}

/* ================================================== */

static int
process_cmd_tracking(char *line)
{
  CMD_Request request;
  CMD_Reply reply;
  IPAddr ip_addr;
  uint32_t ref_id;
  char host[50];
  char *ref_ip;
  struct timeval ref_time;
  struct tm ref_time_tm;
  unsigned long a, b, c, d;
  double correction;
  double last_offset;
  double rms_offset;
  double freq_ppm;
  double resid_freq_ppm;
  double skew_ppm;
  double root_delay;
  double root_dispersion;
  double last_update_interval;
  const char *leap_status;
  
  request.command = htons(REQ_TRACKING);
  if (request_reply(&request, &reply, RPY_TRACKING, 0)) {
    ref_id = ntohl(reply.data.tracking.ref_id);
    a = (ref_id >> 24);
    b = (ref_id >> 16) & 0xff;
    c = (ref_id >> 8) & 0xff;
    d = (ref_id) & 0xff;
    
    UTI_IPNetworkToHost(&reply.data.tracking.ip_addr, &ip_addr);
    if (ip_addr.family == IPADDR_UNSPEC) {
      ref_ip = UTI_RefidToString(ref_id);
    } else if (no_dns) {
      ref_ip = UTI_IPToString(&ip_addr);
    } else {
      DNS_IPAddress2Name(&ip_addr, host, sizeof (host));
      ref_ip = host;
    }
    
    switch (ntohs(reply.data.tracking.leap_status)) {
      case LEAP_Normal:
        leap_status = "Normal";
        break;
      case LEAP_InsertSecond:
        leap_status = "Insert second";
        break;
      case LEAP_DeleteSecond:
        leap_status = "Delete second";
        break;
      case LEAP_Unsynchronised:
        leap_status = "Not synchronised";
        break;
      default:
        leap_status = "Unknown";
        break;
    }

    printf("Reference ID    : %lu.%lu.%lu.%lu (%s)\n", a, b, c, d, ref_ip);
    printf("Stratum         : %lu\n", (unsigned long) ntohs(reply.data.tracking.stratum));
    UTI_TimevalNetworkToHost(&reply.data.tracking.ref_time, &ref_time);
    ref_time_tm = *gmtime((time_t *)&ref_time.tv_sec);
    printf("Ref time (UTC)  : %s", asctime(&ref_time_tm));
    correction = UTI_FloatNetworkToHost(reply.data.tracking.current_correction);
    last_offset = UTI_FloatNetworkToHost(reply.data.tracking.last_offset);
    rms_offset = UTI_FloatNetworkToHost(reply.data.tracking.rms_offset);
    printf("System time     : %.9f seconds %s of NTP time\n", fabs(correction),
           (correction > 0.0) ? "slow" : "fast");
    printf("Last offset     : %.9f seconds\n", last_offset);
    printf("RMS offset      : %.9f seconds\n", rms_offset);
    freq_ppm = UTI_FloatNetworkToHost(reply.data.tracking.freq_ppm);
    resid_freq_ppm = UTI_FloatNetworkToHost(reply.data.tracking.resid_freq_ppm);
    skew_ppm = UTI_FloatNetworkToHost(reply.data.tracking.skew_ppm);
    root_delay = UTI_FloatNetworkToHost(reply.data.tracking.root_delay);
    root_dispersion = UTI_FloatNetworkToHost(reply.data.tracking.root_dispersion);
    last_update_interval = UTI_FloatNetworkToHost(reply.data.tracking.last_update_interval);
    printf("Frequency       : %.3f ppm %s\n", fabs(freq_ppm), (freq_ppm < 0.0) ? "slow" : "fast"); 
    printf("Residual freq   : %.3f ppm\n", resid_freq_ppm);
    printf("Skew            : %.3f ppm\n", skew_ppm);
    printf("Root delay      : %.6f seconds\n", root_delay);
    printf("Root dispersion : %.6f seconds\n", root_dispersion);
    printf("Update interval : %.1f seconds\n", last_update_interval);
    printf("Leap status     : %s\n", leap_status);
    return 1;
  }
  return 0;
}
/* ================================================== */

static int
process_cmd_rtcreport(char *line)
{
  CMD_Request request;
  CMD_Reply reply;
  struct timeval ref_time;
  struct tm ref_time_tm;
  unsigned short n_samples;
  unsigned short n_runs;
  unsigned long span_seconds;
  double coef_seconds_fast;
  double coef_gain_rate_ppm;
  
  request.command = htons(REQ_RTCREPORT);
  if (request_reply(&request, &reply, RPY_RTC, 0)) {
    UTI_TimevalNetworkToHost(&reply.data.rtc.ref_time, &ref_time);
    ref_time_tm = *gmtime(&ref_time.tv_sec);
    n_samples = ntohs(reply.data.rtc.n_samples);
    n_runs = ntohs(reply.data.rtc.n_runs);
    span_seconds = ntohl(reply.data.rtc.span_seconds);
    coef_seconds_fast = UTI_FloatNetworkToHost(reply.data.rtc.rtc_seconds_fast);
    coef_gain_rate_ppm = UTI_FloatNetworkToHost(reply.data.rtc.rtc_gain_rate_ppm);
    printf("RTC ref time (UTC) : %s", asctime(&ref_time_tm));
    printf("Number of samples  : %d\n", n_samples);
    printf("Number of runs     : %d\n", n_runs);
    printf("Sample span period : ");
    print_seconds(span_seconds);
    printf("\n");
    printf("RTC is fast by     : %12.6f seconds\n", coef_seconds_fast);
    printf("RTC gains time at  : %9.3f ppm\n", coef_gain_rate_ppm);
    return 1;
  }
  return 0;
}

/* ================================================== */

static int
process_cmd_clients(char *line)
{
  CMD_Request request;
  CMD_Reply reply;
  unsigned long next_index;
  int j;
  IPAddr ip;
  unsigned long client_hits;
  unsigned long peer_hits;
  unsigned long cmd_hits_auth;
  unsigned long cmd_hits_normal;
  unsigned long cmd_hits_bad;
  unsigned long last_ntp_hit_ago;
  unsigned long last_cmd_hit_ago;
  char hostname_buf[50];

  int n_replies;
  int n_indices_in_table;

  next_index = 0;

  printf("Hostname                   Client    Peer CmdAuth CmdNorm  CmdBad  LstN  LstC\n"
         "=========================  ======  ======  ======  ======  ======  ====  ====\n");

  do {

    request.command = htons(REQ_CLIENT_ACCESSES_BY_INDEX);
    request.data.client_accesses_by_index.first_index = htonl(next_index);
    request.data.client_accesses_by_index.n_indices = htonl(MAX_CLIENT_ACCESSES);

    if (request_reply(&request, &reply, RPY_CLIENT_ACCESSES_BY_INDEX, 0)) {
          n_replies = ntohl(reply.data.client_accesses_by_index.n_clients);
          n_indices_in_table = ntohl(reply.data.client_accesses_by_index.n_indices);
          if (n_replies == 0) {
            goto finished;
          }
          for (j=0; j<n_replies; j++) {
            UTI_IPNetworkToHost(&reply.data.client_accesses_by_index.clients[j].ip, &ip);
            if (ip.family != IPADDR_UNSPEC) {
              /* UNSPEC implies that the node could not be found in
                 the daemon's tables; we shouldn't ever generate this
                 case, but ignore it if we do.  (In future there might
                 be a protocol to reset the client logging; if another
                 administrator runs that while we're doing the clients
                 command, there will be a race condition that could
                 cause this). */
              
              client_hits = ntohl(reply.data.client_accesses_by_index.clients[j].client_hits);
              peer_hits = ntohl(reply.data.client_accesses_by_index.clients[j].peer_hits);
              cmd_hits_auth = ntohl(reply.data.client_accesses_by_index.clients[j].cmd_hits_auth);
              cmd_hits_normal = ntohl(reply.data.client_accesses_by_index.clients[j].cmd_hits_normal);
              cmd_hits_bad = ntohl(reply.data.client_accesses_by_index.clients[j].cmd_hits_bad);
              last_ntp_hit_ago = ntohl(reply.data.client_accesses_by_index.clients[j].last_ntp_hit_ago);
              last_cmd_hit_ago = ntohl(reply.data.client_accesses_by_index.clients[j].last_cmd_hit_ago);

              if (no_dns) {
                snprintf(hostname_buf, sizeof(hostname_buf),
                         "%s", UTI_IPToString(&ip));
              } else {
                DNS_IPAddress2Name(&ip, hostname_buf, sizeof(hostname_buf));
                hostname_buf[25] = 0;
              }
              printf("%-25s  %6ld  %6ld  %6ld  %6ld  %6ld  ",
                     hostname_buf,
                     client_hits, peer_hits,
                     cmd_hits_auth, cmd_hits_normal, cmd_hits_bad);
              print_seconds(last_ntp_hit_ago);
              printf("  ");
              print_seconds(last_cmd_hit_ago);
              printf("\n");
            }
          }              

          /* Set the next index to probe based on what the server tells us */
          next_index = ntohl(reply.data.client_accesses_by_index.next_index);
          if (next_index >= n_indices_in_table) {
            goto finished;
          }
    } else {
      return 0;
    }
  } while (1); /* keep going until all subnets have been expanded,
                  down to single nodes */

finished:
  return 1;
}


/* ================================================== */
/* Process the manual list command */
static int
process_cmd_manual_list(const char *line)
{
  CMD_Request request;
  CMD_Reply reply;
  int n_samples;
  RPY_ManualListSample *sample;
  int i;
  struct timeval when;
  double slewed_offset, orig_offset, residual;

  request.command = htons(REQ_MANUAL_LIST);
  if (request_reply(&request, &reply, RPY_MANUAL_LIST, 0)) {
          n_samples = ntohl(reply.data.manual_list.n_samples);
          printf("210 n_samples = %d\n", n_samples);
          printf("#    Date  Time(UTC)    Slewed   Original   Residual\n"
                 "====================================================\n");
          for (i=0; i<n_samples; i++) {
            sample = &reply.data.manual_list.samples[i];
            UTI_TimevalNetworkToHost(&sample->when, &when);
            slewed_offset = UTI_FloatNetworkToHost(sample->slewed_offset);
            orig_offset = UTI_FloatNetworkToHost(sample->orig_offset);
            residual = UTI_FloatNetworkToHost(sample->residual);
            printf("%2d %s %10.2f %10.2f %10.2f\n", i, time_to_log_form(when.tv_sec), slewed_offset, orig_offset, residual);
          }
          return 1;
  }
  return 0;
}

/* ================================================== */

static int
process_cmd_manual_delete(CMD_Request *msg, const char *line)
{
  int index;

  if (sscanf(line, "%d", &index) != 1) {
    fprintf(stderr, "Bad syntax for manual delete command\n");
    return 0;

  }

  msg->command = htons(REQ_MANUAL_DELETE);
  msg->data.manual_delete.index = htonl(index);
  return 1;
}

/* ================================================== */

static int
process_cmd_settime(char *line)
{
  struct timeval ts;
  time_t now, new_time;
  CMD_Request request;
  CMD_Reply reply;
  long offset_cs;
  double dfreq_ppm, new_afreq_ppm;
  double offset;

  now = time(NULL);
  new_time = get_date(line, &now);

  if (new_time == -1) {
    printf("510 - Could not parse date string\n");
  } else {
    ts.tv_sec = new_time;
    ts.tv_usec = 0;
    UTI_TimevalHostToNetwork(&ts, &request.data.settime.ts);
    request.command = htons(REQ_SETTIME);
    if (request_reply(&request, &reply, RPY_MANUAL_TIMESTAMP, 1)) {
          offset_cs = ntohl(reply.data.manual_timestamp.centiseconds);
          offset = 0.01 * (double)(int32_t)offset_cs;
          dfreq_ppm = UTI_FloatNetworkToHost(reply.data.manual_timestamp.dfreq_ppm);
          new_afreq_ppm = UTI_FloatNetworkToHost(reply.data.manual_timestamp.new_afreq_ppm);
          printf("Clock was %.2f seconds fast.  Frequency change = %.2fppm, new frequency = %.2fppm\n",
              offset, dfreq_ppm, new_afreq_ppm);
          return 1;
    }
  }
  return 0;
}

/* ================================================== */

static void
process_cmd_rekey(CMD_Request *msg, char *line)
{
  msg->command = htons(REQ_REKEY);
}

/* ================================================== */

static void
process_cmd_makestep(CMD_Request *msg, char *line)
{
  msg->command = htons(REQ_MAKESTEP);
}

/* ================================================== */

static int
process_cmd_activity(const char *line)
{
  CMD_Request request;
  CMD_Reply reply;
  request.command = htons(REQ_ACTIVITY);
  if (request_reply(&request, &reply, RPY_ACTIVITY, 1)) {
        printf(
               "%ld sources online\n"
               "%ld sources offline\n"
               "%ld sources doing burst (return to online)\n"
               "%ld sources doing burst (return to offline)\n"
               "%ld sources with unknown address\n",
                (long) ntohl(reply.data.activity.online),
                (long) ntohl(reply.data.activity.offline),
                (long) ntohl(reply.data.activity.burst_online),
                (long) ntohl(reply.data.activity.burst_offline),
                (long) ntohl(reply.data.activity.unresolved));
        return 1;
  }
  return 0;
}

/* ================================================== */

static int
process_cmd_reselectdist(CMD_Request *msg, char *line)
{
  double dist;
  int ok;
  msg->command = htons(REQ_RESELECTDISTANCE);
  if (sscanf(line, "%lf", &dist) == 1) {
    msg->data.reselect_distance.distance = UTI_FloatHostToNetwork(dist);
    ok = 1;
  } else {
    ok = 0;
  }
  return ok;
}

/* ================================================== */

static void
process_cmd_reselect(CMD_Request *msg, char *line)
{
  msg->command = htons(REQ_RESELECT);
}

/* ================================================== */

static int
process_cmd_waitsync(char *line)
{
  CMD_Request request;
  CMD_Reply reply;
  uint32_t ref_id, a, b, c, d;
  double correction, skew_ppm, max_correction, max_skew_ppm;
  int ret = 0, max_tries, i;

  max_tries = 0;
  max_correction = 0.0;
  max_skew_ppm = 0.0;

  sscanf(line, "%d %lf %lf", &max_tries, &max_correction, &max_skew_ppm);

  request.command = htons(REQ_TRACKING);

  for (i = 1; ; i++) {
    if (request_reply(&request, &reply, RPY_TRACKING, 0)) {
      ref_id = ntohl(reply.data.tracking.ref_id);
      a = (ref_id >> 24);
      b = (ref_id >> 16) & 0xff;
      c = (ref_id >> 8) & 0xff;
      d = (ref_id) & 0xff;

      correction = UTI_FloatNetworkToHost(reply.data.tracking.current_correction);
      correction = fabs(correction);
      skew_ppm = UTI_FloatNetworkToHost(reply.data.tracking.skew_ppm);

      printf("try: %d, refid: %d.%d.%d.%d, correction: %.9f, skew: %.3f\n",
          i, a, b, c, d, correction, skew_ppm);

      if (ref_id != 0 && ref_id != 0x7f7f0101L /* LOCAL refid */ &&
          (max_correction == 0.0 || correction <= max_correction) &&
          (max_skew_ppm == 0.0 || skew_ppm <= max_skew_ppm)) {
        ret = 1;
      }
    }

    if (!ret && (!max_tries || i < max_tries)) {
      sleep(10);
    } else {
      break;
    }
  }
  return ret;
}

/* ================================================== */

static int
process_cmd_dns(const char *line)
{
  if (!strcmp(line, "-46")) {
    DNS_SetAddressFamily(IPADDR_UNSPEC);
  } else if (!strcmp(line, "-4")) {
    DNS_SetAddressFamily(IPADDR_INET4);
  } else if (!strcmp(line, "-6")) {
    DNS_SetAddressFamily(IPADDR_INET6);
  } else if (!strcmp(line, "-n")) {
    no_dns = 1;
  } else if (!strcmp(line, "+n")) {
    no_dns = 0;
  } else {
    fprintf(stderr, "Unrecognized dns command\n");
    return 0;
  }
  return 1;
}

/* ================================================== */

static int
process_cmd_authhash(const char *line)
{
  const char *hash_name;
  int new_hash_id;

  assert(auth_hash_id >= 0);
  hash_name = line;

  if (!*hash_name) {
    fprintf(stderr, "Could not parse hash name\n");
    return 0;
  }

  new_hash_id = HSH_GetHashId(hash_name);
  if (new_hash_id < 0) {
    fprintf(stderr, "Unknown hash name: %s\n", hash_name);
    return 0;
  }

  auth_hash_id = new_hash_id;

  return 1;
}

/* ================================================== */

static int
process_cmd_timeout(const char *line)
{
  int timeout;

  timeout = atoi(line);
  if (timeout < 100) {
    fprintf(stderr, "Timeout %d is too short\n", timeout);
    return 0;
  }
  initial_timeout = timeout;
  return 1;
}

/* ================================================== */

static int
process_cmd_retries(const char *line)
{
  int retries;

  retries = atoi(line);
  if (retries < 0) {
    fprintf(stderr, "Invalid maximum number of retries\n");
    return 0;
  }
  max_retries = retries;
  return 1;
}

/* ================================================== */

static int
process_line(char *line, int *quit)
{
  char *command;
  int do_normal_submit;
  int ret;
  CMD_Request tx_message;
  CMD_Reply rx_message;

  *quit = 0;
  ret = 0;

  do_normal_submit = 1;

  CPS_NormalizeLine(line);

  if (!*line) {
    fflush(stderr);
    fflush(stdout);
    return 1;
  };

  command = line;
  line = CPS_SplitWord(line);

  if (!strcmp(command, "accheck")) {
    do_normal_submit = process_cmd_accheck(&tx_message, line);
  } else if (!strcmp(command, "activity")) {
    do_normal_submit = 0;
    ret = process_cmd_activity(line);
  } else if (!strcmp(command, "add") && !strncmp(line, "peer", 4)) {
    do_normal_submit = process_cmd_add_peer(&tx_message, CPS_SplitWord(line));
  } else if (!strcmp(command, "add") && !strncmp(line, "server", 6)) {
    do_normal_submit = process_cmd_add_server(&tx_message, CPS_SplitWord(line));
  } else if (!strcmp(command, "allow")) {
    if (!strncmp(line, "all", 3)) {
      do_normal_submit = process_cmd_allowall(&tx_message, CPS_SplitWord(line));
    } else {
      do_normal_submit = process_cmd_allow(&tx_message, line);
    }
  } else if (!strcmp(command, "authhash")) {
    ret = process_cmd_authhash(line);
    do_normal_submit = 0;
  } else if (!strcmp(command, "burst")) {
    do_normal_submit = process_cmd_burst(&tx_message, line);
  } else if (!strcmp(command, "clients")) {
    ret = process_cmd_clients(line);
    do_normal_submit = 0;
  } else if (!strcmp(command, "cmdaccheck")) {
    do_normal_submit = process_cmd_cmdaccheck(&tx_message, line);
  } else if (!strcmp(command, "cmdallow")) {
    if (!strncmp(line, "all", 3)) {
      do_normal_submit = process_cmd_cmdallowall(&tx_message, CPS_SplitWord(line));
    } else {
      do_normal_submit = process_cmd_cmdallow(&tx_message, line);
    }
  } else if (!strcmp(command, "cmddeny")) {
    if (!strncmp(line, "all", 3)) {
      line = CPS_SplitWord(line);
      do_normal_submit = process_cmd_cmddenyall(&tx_message, line);
    } else {
      do_normal_submit = process_cmd_cmddeny(&tx_message, line);
    }
  } else if (!strcmp(command, "cyclelogs")) {
    process_cmd_cyclelogs(&tx_message, line);
  } else if (!strcmp(command, "delete")) {
    do_normal_submit = process_cmd_delete(&tx_message, line);
  } else if (!strcmp(command, "deny")) {
    if (!strncmp(line, "all", 3)) {
      do_normal_submit = process_cmd_denyall(&tx_message, CPS_SplitWord(line));
    } else {
      do_normal_submit = process_cmd_deny(&tx_message, line);
    }
  } else if (!strcmp(command, "dfreq")) {
    process_cmd_dfreq(&tx_message, line);
  } else if (!strcmp(command, "dns")) {
    ret = process_cmd_dns(line);
    do_normal_submit = 0;
  } else if (!strcmp(command, "doffset")) {
    process_cmd_doffset(&tx_message, line);
  } else if (!strcmp(command, "dump")) {
    process_cmd_dump(&tx_message, line);
  } else if (!strcmp(command, "exit")) {
    do_normal_submit = 0;
    *quit = 1;
    ret = 1;
  } else if (!strcmp(command, "help")) {
    do_normal_submit = 0;
    give_help();
    ret = 1;
  } else if (!strcmp(command, "local")) {
    do_normal_submit = process_cmd_local(&tx_message, line);
  } else if (!strcmp(command, "makestep")) {
    process_cmd_makestep(&tx_message, line);
  } else if (!strcmp(command, "manual")) {
    if (!strncmp(line, "list", 4)) {
      do_normal_submit = 0;
      ret = process_cmd_manual_list(CPS_SplitWord(line));
    } else if (!strncmp(line, "delete", 6)) {
      do_normal_submit = process_cmd_manual_delete(&tx_message, CPS_SplitWord(line));
    } else {
      do_normal_submit = process_cmd_manual(&tx_message, line);
    }
  } else if (!strcmp(command, "maxdelay")) {
    do_normal_submit = process_cmd_maxdelay(&tx_message, line);
  } else if (!strcmp(command, "maxdelaydevratio")) {
    do_normal_submit = process_cmd_maxdelaydevratio(&tx_message, line);
  } else if (!strcmp(command, "maxdelayratio")) {
    do_normal_submit = process_cmd_maxdelayratio(&tx_message, line);
  } else if (!strcmp(command, "maxpoll")) {
    do_normal_submit = process_cmd_maxpoll(&tx_message, line);
  } else if (!strcmp(command, "maxupdateskew")) {
    do_normal_submit = process_cmd_maxupdateskew(&tx_message, line);
  } else if (!strcmp(command, "minpoll")) {
    do_normal_submit = process_cmd_minpoll(&tx_message, line);
  } else if (!strcmp(command, "minstratum")) {
    do_normal_submit = process_cmd_minstratum(&tx_message, line);
  } else if (!strcmp(command, "offline")) {
    do_normal_submit = process_cmd_offline(&tx_message, line);
  } else if (!strcmp(command, "online")) {
    do_normal_submit = process_cmd_online(&tx_message, line);
  } else if (!strcmp(command, "password")) {
    do_normal_submit = process_cmd_password(&tx_message, line);
  } else if (!strcmp(command, "polltarget")) {
    do_normal_submit = process_cmd_polltarget(&tx_message, line);
  } else if (!strcmp(command, "quit")) {
    do_normal_submit = 0;
    *quit = 1;
    ret = 1;
  } else if (!strcmp(command, "rekey")) {
    process_cmd_rekey(&tx_message, line);
  } else if (!strcmp(command, "reselect")) {
    process_cmd_reselect(&tx_message, line);
  } else if (!strcmp(command, "reselectdist")) {
    do_normal_submit = process_cmd_reselectdist(&tx_message, line);
  } else if (!strcmp(command, "retries")) {
    ret = process_cmd_retries(line);
    do_normal_submit = 0;
  } else if (!strcmp(command, "rtcdata")) {
    do_normal_submit = 0;
    ret = process_cmd_rtcreport(line);
  } else if (!strcmp(command, "settime")) {
    do_normal_submit = 0;
    ret = process_cmd_settime(line);
  } else if (!strcmp(command, "sources")) {
    do_normal_submit = 0;
    ret = process_cmd_sources(line);
  } else if (!strcmp(command, "sourcestats")) {
    do_normal_submit = 0;
    ret = process_cmd_sourcestats(line);
  } else if (!strcmp(command, "timeout")) {
    ret = process_cmd_timeout(line);
    do_normal_submit = 0;
  } else if (!strcmp(command, "tracking")) {
    ret = process_cmd_tracking(line);
    do_normal_submit = 0;
  } else if (!strcmp(command, "trimrtc")) {
    process_cmd_trimrtc(&tx_message, line);
  } else if (!strcmp(command, "waitsync")) {
    ret = process_cmd_waitsync(line);
    do_normal_submit = 0;
  } else if (!strcmp(command, "writertc")) {
    process_cmd_writertc(&tx_message, line);
  } else {
    fprintf(stderr, "Unrecognized command\n");
    do_normal_submit = 0;
  }
    
  if (do_normal_submit) {
    ret = request_reply(&tx_message, &rx_message, RPY_NULL, 1);
  }
  fflush(stderr);
  fflush(stdout);
  return ret;
}

/* ================================================== */

static int
authenticate_from_config(const char *filename)
{
  CMD_Request tx_message;
  CMD_Reply rx_message;
  char line[2048], keyfile[2048], *command, *arg, *password;
  const char *hashname;
  unsigned long key_id = 0, key_id2 = -1;
  int ret;
  FILE *in;

  in = fopen(filename, "r");
  if (!in) {
    fprintf(stderr, "Could not open file %s\n", filename);
    return 0;
  }

  *keyfile = '\0';
  while (fgets(line, sizeof (line), in)) {
    CPS_NormalizeLine(line);
    command = line;
    arg = CPS_SplitWord(line);
    if (!strcasecmp(command, "keyfile")) {
      snprintf(keyfile, sizeof (keyfile), "%s", arg);
    } else if (!strcasecmp(command, "commandkey")) {
      if (sscanf(arg, "%lu", &key_id) != 1)
        key_id = -1;
    }
  }
  fclose(in);

  if (!*keyfile || key_id == -1) {
    fprintf(stderr, "Could not read keyfile or commandkey in file %s\n", filename);
    return 0;
  }

  in = fopen(keyfile, "r");
  if (!in) {
    fprintf(stderr, "Could not open keyfile %s\n", keyfile);
    return 0;
  }

  while (fgets(line, sizeof (line), in)) {
    CPS_NormalizeLine(line);
    if (!*line || !CPS_ParseKey(line, &key_id2, &hashname, &password))
      continue;
    if (key_id == key_id2)
      break;
  }
  fclose(in);

  if (key_id == key_id2) {
    if (process_cmd_authhash(hashname) &&
        process_cmd_password(&tx_message, password)) {
      ret = request_reply(&tx_message, &rx_message, RPY_NULL, 1);
    } else {
      ret = 0;
    }
  } else {
    fprintf(stderr, "Could not find key %lu in keyfile %s\n", key_id, keyfile);
    ret = 0;
  }

  /* Erase password from stack */
  memset(line, 0, sizeof (line));

  return ret;
}

/* ================================================== */

static int
process_args(int argc, char **argv, int multi)
{
  int total_length, i, ret, quit;
  char *line;

  total_length = 0;
  for(i=0; i<argc; i++) {
    total_length += strlen(argv[i]) + 1;
  }

  line = (char *) malloc((2 + total_length) * sizeof(char));

  for (i = 0; i < argc; i++) {
    line[0] = '\0';
    if (multi) {
      strcat(line, argv[i]);
    } else {
      for (; i < argc; i++) {
        strcat(line, argv[i]);
        if (i + 1 < argc)
          strcat(line, " ");
      }
    }

    ret = process_line(line, &quit);
    if (!ret || quit)
      break;
  }

  free(line);

  return ret;
}

/* ================================================== */

static void
display_gpl(void)
{
    printf("chrony version %s\n"
           "Copyright (C) 1997-2003, 2007, 2009-2013 Richard P. Curnow and others\n"
           "chrony comes with ABSOLUTELY NO WARRANTY.  This is free software, and\n"
           "you are welcome to redistribute it under certain conditions.  See the\n"
           "GNU General Public License version 2 for details.\n\n",
           CHRONY_VERSION);
}

/* ================================================== */

int
main(int argc, char **argv)
{
  char *line;
  const char *progname = argv[0];
  const char *hostname = "localhost";
  const char *conf_file = DEFAULT_CONF_FILE;
  int quit = 0, ret = 1, multi = 0, auto_auth = 0;
  int port = DEFAULT_CANDM_PORT;

  /* Parse command line options */
  while (++argv, --argc) {
    if (!strcmp(*argv, "-h")) {
      ++argv, --argc;
      if (*argv) {
        hostname = *argv;
      }
    } else if (!strcmp(*argv, "-p")) {
      ++argv, --argc;
      if (*argv) {
        port = atoi(*argv);
      }
    } else if (!strcmp(*argv, "-f")) {
      ++argv, --argc;
      if (*argv) {
        conf_file = *argv;
      }
    } else if (!strcmp(*argv, "-a")) {
      auto_auth = 1;
    } else if (!strcmp(*argv, "-m")) {
      multi = 1;
    } else if (!strcmp(*argv, "-n")) {
      no_dns = 1;
    } else if (!strcmp(*argv, "-4")) {
      DNS_SetAddressFamily(IPADDR_INET4);
      hostname = "127.0.0.1";
    } else if (!strcmp(*argv, "-6")) {
      DNS_SetAddressFamily(IPADDR_INET6);
      hostname = "::1";
    } else if (!strcmp("-v", *argv) || !strcmp("--version",*argv)) {
      printf("chronyc (chrony) version %s\n", CHRONY_VERSION);
      exit(0);
    } else if (!strncmp(*argv, "-", 1)) {
      fprintf(stderr, "Usage : %s [-h <hostname>] [-p <port-number>] [-n] [-4|-6] [-m] [-a] [-f <file>]] [command]\n", progname);
      exit(1);
    } else {
      break; /* And process remainder of line as a command */
    }
  }

  if (isatty(0) && isatty(1) && isatty(2)) {
    on_terminal = 1;
  }

  if (on_terminal && (argc == 0)) {
    display_gpl();
  }

  /* MD5 is the default authentication hash */
  auth_hash_id = HSH_GetHashId("MD5");
  if (auth_hash_id < 0) {
    fprintf(stderr, "Could not initialize MD5\n");
    return 1;
  }
  
  open_io(hostname, port);

  if (auto_auth) {
    ret = authenticate_from_config(conf_file);
  }

  if (!ret) {
    ;
  } else if (argc > 0) {
    ret = process_args(argc, argv, multi);
  } else {
    do {
      line = read_line();
      if (line) {
        ret = process_line(line, &quit);
      }else {
	/* supply the final '\n' when user exits via ^D */
        if( on_terminal ) printf("\n");
      }
    } while (line && !quit);
  }

  close_io();

  free(password);

  return !ret;
}


