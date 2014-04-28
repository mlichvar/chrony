/*
  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Richard P. Curnow  1997-2003
 * Copyright (C) Miroslav Lichvar  2009-2012
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

  Command and monitoring module in the main program
  */

#include "config.h"

#include "sysincl.h"

#include "cmdmon.h"
#include "candm.h"
#include "sched.h"
#include "util.h"
#include "logging.h"
#include "keys.h"
#include "ntp_sources.h"
#include "ntp_core.h"
#include "sources.h"
#include "sourcestats.h"
#include "reference.h"
#include "manual.h"
#include "memory.h"
#include "local.h"
#include "addrfilt.h"
#include "conf.h"
#include "rtc.h"
#include "pktlength.h"
#include "clientlog.h"
#include "refclock.h"

/* ================================================== */

union sockaddr_in46 {
  struct sockaddr_in in4;
#ifdef HAVE_IPV6
  struct sockaddr_in6 in6;
#endif
  struct sockaddr u;
};

/* File descriptors for command and monitoring sockets */
static int sock_fd4;
#ifdef HAVE_IPV6
static int sock_fd6;
#endif

/* Flag indicating whether this module has been initialised or not */
static int initialised = 0;

/* Token which is unique every time the daemon is run */
static unsigned long utoken;

/* The register of issued tokens */
static unsigned long issued_tokens;

/* The register of received tokens */
static unsigned long returned_tokens;

/* The token number corresponding to the base of the registers */
static unsigned long token_base;

/* The position of the next free token to issue in the issue register */
static unsigned long issue_pointer;

/* Type and linked list for buffering responses */
typedef struct _ResponseCell {
  struct _ResponseCell *next;
  unsigned long tok; /* The token that the client sent in the message
                        to which this was the reply */
  unsigned long next_tok; /* The next token issued to the same client.
                             If we receive a request with this token,
                             it implies the reply stored in this cell
                             was successfully received */
  unsigned long msg_seq; /* Client's sequence number used in request
                            to which this is the response. */
  unsigned long attempt; /* Attempt number that we saw in the last request
                            with this sequence number (prevents attacker
                            firing the same request at us to make us
                            keep generating the same reply). */
  struct timeval ts; /* Time we saved the reply - allows purging based
                        on staleness. */
  CMD_Reply rpy;
} ResponseCell;

static ResponseCell kept_replies;
static ResponseCell *free_replies;

/* ================================================== */
/* Array of permission levels for command types */

static int permissions[] = {
  PERMIT_OPEN, /* NULL */
  PERMIT_AUTH, /* ONLINE */
  PERMIT_AUTH, /* OFFLINE */
  PERMIT_AUTH, /* BURST */
  PERMIT_AUTH, /* MODIFY_MINPOLL */
  PERMIT_AUTH, /* MODIFY_MAXPOLL */
  PERMIT_AUTH, /* DUMP */
  PERMIT_AUTH, /* MODIFY_MAXDELAY */
  PERMIT_AUTH, /* MODIFY_MAXDELAYRATIO */
  PERMIT_AUTH, /* MODIFY_MAXUPDATESKEW */
  PERMIT_OPEN, /* LOGON */
  PERMIT_AUTH, /* SETTIME */
  PERMIT_AUTH, /* LOCAL */
  PERMIT_AUTH, /* MANUAL */
  PERMIT_OPEN, /* N_SOURCES */
  PERMIT_OPEN, /* SOURCE_DATA */
  PERMIT_AUTH, /* REKEY */
  PERMIT_AUTH, /* ALLOW */
  PERMIT_AUTH, /* ALLOWALL */
  PERMIT_AUTH, /* DENY */
  PERMIT_AUTH, /* DENYALL */
  PERMIT_AUTH, /* CMDALLOW */
  PERMIT_AUTH, /* CMDALLOWALL */
  PERMIT_AUTH, /* CMDDENY */
  PERMIT_AUTH, /* CMDDENYALL */
  PERMIT_AUTH, /* ACCHECK */
  PERMIT_AUTH, /* CMDACCHECK */
  PERMIT_AUTH, /* ADD_SERVER */
  PERMIT_AUTH, /* ADD_PEER */
  PERMIT_AUTH, /* DEL_SOURCE */
  PERMIT_AUTH, /* WRITERTC */
  PERMIT_AUTH, /* DFREQ */
  PERMIT_AUTH, /* DOFFSET */
  PERMIT_OPEN, /* TRACKING */
  PERMIT_OPEN, /* SOURCESTATS */
  PERMIT_OPEN, /* RTCREPORT */
  PERMIT_AUTH, /* TRIMRTC */
  PERMIT_AUTH, /* CYCLELOGS */
  PERMIT_AUTH, /* SUBNETS_ACCESSED */
  PERMIT_AUTH, /* CLIENT_ACCESSES (by subnet) */
  PERMIT_AUTH, /* CLIENT_ACCESSES_BY_INDEX */
  PERMIT_OPEN, /* MANUAL_LIST */
  PERMIT_AUTH, /* MANUAL_DELETE */
  PERMIT_AUTH, /* MAKESTEP */
  PERMIT_OPEN, /* ACTIVITY */
  PERMIT_AUTH, /* MODIFY_MINSTRATUM */
  PERMIT_AUTH, /* MODIFY_POLLTARGET */
  PERMIT_AUTH, /* MODIFY_MAXDELAYDEVRATIO */
  PERMIT_AUTH, /* RESELECT */
  PERMIT_AUTH  /* RESELECTDISTANCE */
};

/* ================================================== */

/* This authorisation table is used for checking whether particular
   machines are allowed to make command and monitoring requests. */
static ADF_AuthTable access_auth_table;

/* ================================================== */
/* Forward prototypes */
static int prepare_socket(int family);
static void read_from_cmd_socket(void *anything);

/* ================================================== */

static int
prepare_socket(int family)
{
  int port_number, sock_fd;
  socklen_t my_addr_len;
  union sockaddr_in46 my_addr;
  IPAddr bind_address;
  int on_off = 1;

  port_number = CNF_GetCommandPort();

  sock_fd = socket(family, SOCK_DGRAM, 0);
  if (sock_fd < 0) {
    LOG(LOGS_ERR, LOGF_CmdMon, "Could not open %s command socket : %s",
        family == AF_INET ? "IPv4" : "IPv6", strerror(errno));
    return -1;
  }

  /* Close on exec */
  UTI_FdSetCloexec(sock_fd);

  /* Allow reuse of port number */
  if (setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, (char *) &on_off, sizeof(on_off)) < 0) {
    LOG(LOGS_ERR, LOGF_CmdMon, "Could not set reuseaddr socket options");
    /* Don't quit - we might survive anyway */
  }
#ifdef HAVE_IPV6
  if (family == AF_INET6) {
#ifdef IPV6_V6ONLY
    /* Receive IPv6 packets only */
    if (setsockopt(sock_fd, IPPROTO_IPV6, IPV6_V6ONLY, (char *)&on_off, sizeof(on_off)) < 0) {
      LOG(LOGS_ERR, LOGF_CmdMon, "Could not request IPV6_V6ONLY socket option");
    }
#endif
  }
#endif

  memset(&my_addr, 0, sizeof (my_addr));

  switch (family) {
    case AF_INET:
      my_addr_len = sizeof (my_addr.in4);
      my_addr.in4.sin_family = family;
      my_addr.in4.sin_port = htons((unsigned short)port_number);

      CNF_GetBindCommandAddress(IPADDR_INET4, &bind_address);

      if (bind_address.family == IPADDR_INET4)
        my_addr.in4.sin_addr.s_addr = htonl(bind_address.addr.in4);
      else
        my_addr.in4.sin_addr.s_addr = htonl(INADDR_ANY);
      break;
#ifdef HAVE_IPV6
    case AF_INET6:
      my_addr_len = sizeof (my_addr.in6);
      my_addr.in6.sin6_family = family;
      my_addr.in6.sin6_port = htons((unsigned short)port_number);

      CNF_GetBindCommandAddress(IPADDR_INET6, &bind_address);

      if (bind_address.family == IPADDR_INET6)
        memcpy(my_addr.in6.sin6_addr.s6_addr, bind_address.addr.in6,
            sizeof (my_addr.in6.sin6_addr.s6_addr));
      else
        my_addr.in6.sin6_addr = in6addr_any;
      break;
#endif
    default:
      assert(0);
  }

  if (bind(sock_fd, &my_addr.u, my_addr_len) < 0) {
    LOG(LOGS_ERR, LOGF_CmdMon, "Could not bind %s command socket : %s",
        family == AF_INET ? "IPv4" : "IPv6", strerror(errno));
    close(sock_fd);
    return -1;
  }

  /* Register handler for read events on the socket */
  SCH_AddInputFileHandler(sock_fd, read_from_cmd_socket, (void *)(long)sock_fd);

  return sock_fd;
}

/* ================================================== */

void
CAM_Initialise(int family)
{
  int i;

  assert(!initialised);
  initialised = 1;

  assert(sizeof (permissions) / sizeof (permissions[0]) == N_REQUEST_TYPES);

  for (i = 0; i < N_REQUEST_TYPES; i++) {
    CMD_Request r;
    int command_length, padding_length;

    r.version = PROTO_VERSION_NUMBER;
    r.command = htons(i);
    command_length = PKL_CommandLength(&r);
    padding_length = PKL_CommandPaddingLength(&r);
    assert(padding_length <= MAX_PADDING_LENGTH && padding_length <= command_length);
    assert(command_length == 0 || command_length >= offsetof(CMD_Reply, data));
  }

  utoken = (unsigned long) time(NULL);

  issued_tokens = returned_tokens = issue_pointer = 0;
  token_base = 1; /* zero is the value used when the previous command was
                     unauthenticated */

  free_replies = NULL;
  kept_replies.next = NULL;

  if (family == IPADDR_UNSPEC || family == IPADDR_INET4)
    sock_fd4 = prepare_socket(AF_INET);
  else
    sock_fd4 = -1;
#ifdef HAVE_IPV6
  if (family == IPADDR_UNSPEC || family == IPADDR_INET6)
    sock_fd6 = prepare_socket(AF_INET6);
  else
    sock_fd6 = -1;
#endif

  if (sock_fd4 < 0
#ifdef HAVE_IPV6
      && sock_fd6 < 0
#endif
      ) {
    LOG_FATAL(LOGF_CmdMon, "Could not open any command socket");
  }

  access_auth_table = ADF_CreateTable();

}

/* ================================================== */

void
CAM_Finalise(void)
{
  if (sock_fd4 >= 0) {
    SCH_RemoveInputFileHandler(sock_fd4);
    close(sock_fd4);
  }
  sock_fd4 = -1;
#ifdef HAVE_IPV6
  if (sock_fd6 >= 0) {
    SCH_RemoveInputFileHandler(sock_fd6);
    close(sock_fd6);
  }
  sock_fd6 = -1;
#endif

  ADF_DestroyTable(access_auth_table);

  initialised = 0;
}

/* ================================================== */
/* This function checks whether the authenticator field of the packet
   checks correctly against what we would compute locally given the
   rest of the packet */

static int
check_rx_packet_auth(CMD_Request *packet, int packet_len)
{
  int pkt_len, auth_len;

  pkt_len = PKL_CommandLength(packet);
  auth_len = packet_len - pkt_len;

  return KEY_CheckAuth(KEY_GetCommandKey(), (unsigned char *)packet,
      pkt_len, ((unsigned char *)packet) + pkt_len, auth_len);
}

/* ================================================== */

static int
generate_tx_packet_auth(CMD_Reply *packet)
{
  int pkt_len;

  pkt_len = PKL_ReplyLength(packet);

  return KEY_GenerateAuth(KEY_GetCommandKey(), (unsigned char *)packet,
      pkt_len, ((unsigned char *)packet) + pkt_len, sizeof (packet->auth));
}

/* ================================================== */

static void
shift_tokens(void)
{
  do {
    issued_tokens >>= 1;
    returned_tokens >>= 1;
    token_base++;
    issue_pointer--;
  } while ((issued_tokens & 1) && (returned_tokens & 1));
}

/* ================================================== */

static unsigned long
get_token(void)
{
  unsigned long result;

  if (issue_pointer == 32) {
    /* The lowest number open token has not been returned - bad luck
       to that command client */
    shift_tokens();
  }

  result = token_base + issue_pointer;
  issued_tokens |= (1UL << issue_pointer);
  issue_pointer++;

  return result;
}

/* ================================================== */

static int
check_token(unsigned long token)
{
  int result;
  unsigned long pos;

  if (token < token_base) {
    /* Token too old */
    result = 0;
  } else {
    pos = token - token_base;
    if (pos >= issue_pointer) {
      /* Token hasn't been issued yet */
      result = 0;
    } else {
      if (returned_tokens & (1UL << pos)) {
        /* Token has already been returned */
        result = 0;
      } else {
        /* Token is OK */
        result = 1;
        returned_tokens |= (1UL << pos);
        if (pos == 0) {
          shift_tokens();
        }
      }
    }
  }

  return result;

}

/* ================================================== */

#define TS_MARGIN 20

/* ================================================== */

typedef struct _TimestampCell {
  struct _TimestampCell *next;
  struct timeval ts;
} TimestampCell;

static struct _TimestampCell seen_ts_list={NULL};
static struct _TimestampCell *free_ts_list=NULL;

#define EXTEND_QUANTUM 32

/* ================================================== */

static TimestampCell *
allocate_ts_cell(void)
{
  TimestampCell *result;
  int i;
  if (free_ts_list == NULL) {
    free_ts_list = MallocArray(TimestampCell, EXTEND_QUANTUM);
    for (i=0; i<EXTEND_QUANTUM-1; i++) {
      free_ts_list[i].next = free_ts_list + i + 1;
    }
    free_ts_list[EXTEND_QUANTUM - 1].next = NULL;
  }

  result = free_ts_list;
  free_ts_list = free_ts_list->next;
  return result;
}

/* ================================================== */

static void
release_ts_cell(TimestampCell *node)
{
  node->next = free_ts_list;
  free_ts_list = node;
}

/* ================================================== */
/* Return 1 if not found, 0 if found (i.e. not unique).  Prune out any
   stale entries. */

static int
check_unique_ts(struct timeval *ts, struct timeval *now)
{
  TimestampCell *last_valid, *cell, *next;
  int ok;

  ok = 1;
  last_valid = &(seen_ts_list);
  cell = last_valid->next;

  while (cell) {
    next = cell->next;
    /* Check if stale */
    if ((now->tv_sec - cell->ts.tv_sec) > TS_MARGIN) {
      release_ts_cell(cell);
      last_valid->next = next;
    } else {
      /* Timestamp in cell is still within window */
      last_valid->next = cell;
      last_valid = cell;
      if ((cell->ts.tv_sec == ts->tv_sec) && (cell->ts.tv_usec == ts->tv_usec)) {
        ok = 0;
      }
    }
    cell = next;
  }

  if (ok) {
    /* Need to add this timestamp to the list */
    cell = allocate_ts_cell();
    last_valid->next = cell;
    cell->next = NULL;
    cell->ts = *ts;
  }

  return ok;
}

/* ================================================== */

static int
ts_is_unique_and_not_stale(struct timeval *ts, struct timeval *now)
{
  int within_margin=0;
  int is_unique=0;
  long diff;

  diff = now->tv_sec - ts->tv_sec;
  if ((diff < TS_MARGIN) && (diff > -TS_MARGIN)) {
    within_margin = 1;
  } else {
    within_margin = 0;
  }
  is_unique = check_unique_ts(ts, now);
    
  return within_margin && is_unique;
}

/* ================================================== */

#define REPLY_EXTEND_QUANTUM 32

static void
get_more_replies(void)
{
  ResponseCell *new_replies;
  int i;

  if (!free_replies) {
    new_replies = MallocArray(ResponseCell, REPLY_EXTEND_QUANTUM);
    for (i=1; i<REPLY_EXTEND_QUANTUM; i++) {
      new_replies[i-1].next = new_replies + i;
    }
    free_replies = new_replies;
  }
}

/* ================================================== */

static ResponseCell *
get_reply_slot(void)
{
  ResponseCell *result;
  if (!free_replies) {
    get_more_replies();
  }
  result = free_replies;
  free_replies = result->next;
  return result;
}

/* ================================================== */

static void
free_reply_slot(ResponseCell *cell)
{
  cell->next = free_replies;
  free_replies = cell;
}

/* ================================================== */

static void
save_reply(CMD_Reply *msg,
           unsigned long tok_reply_to,
           unsigned long new_tok_issued,
           unsigned long client_msg_seq,
           unsigned short attempt,
           struct timeval *now)
{
  ResponseCell *cell;

  cell = get_reply_slot();

  cell->ts = *now;
  memcpy(&cell->rpy, msg, sizeof(CMD_Reply));
  cell->tok = tok_reply_to;
  cell->next_tok = new_tok_issued;
  cell->msg_seq = client_msg_seq;
  cell->attempt = (unsigned long) attempt;

  cell->next = kept_replies.next;
  kept_replies.next = cell;

}

/* ================================================== */

static CMD_Reply *
lookup_reply(unsigned long prev_msg_token, unsigned long client_msg_seq, unsigned short attempt)
{
  ResponseCell *ptr;

  ptr = kept_replies.next;
  while (ptr) {
    if ((ptr->tok == prev_msg_token) &&
        (ptr->msg_seq == client_msg_seq) &&
        ((unsigned long) attempt > ptr->attempt)) {

      /* Set the attempt field to remember the highest number we have
         had so far */
      ptr->attempt = (unsigned long) attempt;
      return &ptr->rpy;
    }
    ptr = ptr->next;
  }

  return NULL;
}


/* ================================================== */

#define REPLY_MAXAGE 300

static void
token_acknowledged(unsigned long token, struct timeval *now)
{
  ResponseCell *last_valid, *cell, *next;

  last_valid = &kept_replies;
  cell = kept_replies.next;
  
  while(cell) {
    next = cell->next;

    /* Discard if it's the one or if the reply is stale */
    if ((cell->next_tok == token) ||
        ((now->tv_sec - cell->ts.tv_sec) > REPLY_MAXAGE)) {
      free_reply_slot(cell);
      last_valid->next = next;
    } else {
      last_valid->next = cell;
      last_valid = cell;
    }
    cell = next;
  }
}

/* ================================================== */

#if 0
/* These two routines are not legal if the program is operating as a daemon, since
   stderr is no longer open */

static void
print_command_packet(CMD_Request *pkt, int length)
{
  unsigned char *x;
  int i;
  x = (unsigned char *) pkt;
  for (i=0; i<length; i++) {
    fprintf(stderr, "%02x ", x[i]);
    if (i%16 == 15) {
      fprintf(stderr, "\n");
    }
  }
  fprintf(stderr, "\n");
}

/* ================================================== */

static void
print_reply_packet(CMD_Reply *pkt)
{
  unsigned char *x;
  int i;
  x = (unsigned char *) pkt;
  for (i=0; i<sizeof(CMD_Reply); i++) {
    fprintf(stderr, "%02x ", x[i]);
    if (i%16 == 15) {
      fprintf(stderr, "\n");
    }
  }
  fprintf(stderr, "\n");
}

#endif 

/* ================================================== */

static void
transmit_reply(CMD_Reply *msg, union sockaddr_in46 *where_to, int auth_len)
{
  int status;
  int tx_message_length;
  int sock_fd;
  socklen_t addrlen;
  
  switch (where_to->u.sa_family) {
    case AF_INET:
      sock_fd = sock_fd4;
      addrlen = sizeof (where_to->in4);
      break;
#ifdef HAVE_IPV6
    case AF_INET6:
      sock_fd = sock_fd6;
      addrlen = sizeof (where_to->in6);
      break;
#endif
    default:
      assert(0);
  }

  tx_message_length = PKL_ReplyLength(msg) + auth_len;
  status = sendto(sock_fd, (void *) msg, tx_message_length, 0,
                  &where_to->u, addrlen);

  if (status < 0 && !LOG_RateLimited()) {
    unsigned short port;
    IPAddr ip;

    switch (where_to->u.sa_family) {
      case AF_INET:
        ip.family = IPADDR_INET4;
        ip.addr.in4 = ntohl(where_to->in4.sin_addr.s_addr);
        port = ntohs(where_to->in4.sin_port);
        break;
#ifdef HAVE_IPV6
      case AF_INET6:
        ip.family = IPADDR_INET6;
        memcpy(ip.addr.in6, (where_to->in6.sin6_addr.s6_addr), sizeof(ip.addr.in6));
        port = ntohs(where_to->in6.sin6_port);
        break;
#endif
      default:
        assert(0);
    }

    LOG(LOGS_WARN, LOGF_CmdMon, "Could not send response to %s:%hu", UTI_IPToString(&ip), port);
  }
}
  

/* ================================================== */

static void
handle_null(CMD_Request *rx_message, CMD_Reply *tx_message)
{
  tx_message->status = htons(STT_SUCCESS);
}

/* ================================================== */

static void
handle_online(CMD_Request *rx_message, CMD_Reply *tx_message)
{
  int status;
  IPAddr address, mask;
  UTI_IPNetworkToHost(&rx_message->data.online.mask, &mask);
  UTI_IPNetworkToHost(&rx_message->data.online.address, &address);
  status = NSR_TakeSourcesOnline(&mask, &address);
  if (status) {
    tx_message->status = htons(STT_SUCCESS);
  } else {
    tx_message->status = htons(STT_NOSUCHSOURCE);
  }
}

/* ================================================== */

static void
handle_offline(CMD_Request *rx_message, CMD_Reply *tx_message)
{
  int status;
  IPAddr address, mask;
  UTI_IPNetworkToHost(&rx_message->data.offline.mask, &mask);
  UTI_IPNetworkToHost(&rx_message->data.offline.address, &address);
  status = NSR_TakeSourcesOffline(&mask, &address);
  if (status) {
    tx_message->status = htons(STT_SUCCESS);
  } else {
    tx_message->status = htons(STT_NOSUCHSOURCE);
  }
}

/* ================================================== */

static void
handle_burst(CMD_Request *rx_message, CMD_Reply *tx_message)
{
  int status;
  IPAddr address, mask;
  UTI_IPNetworkToHost(&rx_message->data.burst.mask, &mask);
  UTI_IPNetworkToHost(&rx_message->data.burst.address, &address);
  status = NSR_InitiateSampleBurst(ntohl(rx_message->data.burst.n_good_samples),
                                   ntohl(rx_message->data.burst.n_total_samples),
                                   &mask, &address);
  
  if (status) {
    tx_message->status = htons(STT_SUCCESS);
  } else {
    tx_message->status = htons(STT_NOSUCHSOURCE);
  }
}

/* ================================================== */

static void
handle_modify_minpoll(CMD_Request *rx_message, CMD_Reply *tx_message)
{
  int status;
  IPAddr address;
  UTI_IPNetworkToHost(&rx_message->data.modify_minpoll.address, &address);
  status = NSR_ModifyMinpoll(&address,
                             ntohl(rx_message->data.modify_minpoll.new_minpoll));
  
  if (status) {
    tx_message->status = htons(STT_SUCCESS);
  } else {
    tx_message->status = htons(STT_NOSUCHSOURCE);
  }
}

/* ================================================== */

static void
handle_modify_maxpoll(CMD_Request *rx_message, CMD_Reply *tx_message)
{
  int status;
  IPAddr address;
  UTI_IPNetworkToHost(&rx_message->data.modify_minpoll.address, &address);
  status = NSR_ModifyMaxpoll(&address,
                             ntohl(rx_message->data.modify_minpoll.new_minpoll));
  
  if (status) {
    tx_message->status = htons(STT_SUCCESS);
  } else {
    tx_message->status = htons(STT_NOSUCHSOURCE);
  }
}

/* ================================================== */

static void
handle_modify_maxdelay(CMD_Request *rx_message, CMD_Reply *tx_message)
{
  int status;
  IPAddr address;
  UTI_IPNetworkToHost(&rx_message->data.modify_maxdelay.address, &address);
  status = NSR_ModifyMaxdelay(&address,
                              UTI_FloatNetworkToHost(rx_message->data.modify_maxdelay.new_max_delay));
  if (status) {
    tx_message->status = htons(STT_SUCCESS);
  } else {
    tx_message->status = htons(STT_NOSUCHSOURCE);
  }
}

/* ================================================== */

static void
handle_modify_maxdelayratio(CMD_Request *rx_message, CMD_Reply *tx_message)
{
  int status;
  IPAddr address;
  UTI_IPNetworkToHost(&rx_message->data.modify_maxdelayratio.address, &address);
  status = NSR_ModifyMaxdelayratio(&address,
                                   UTI_FloatNetworkToHost(rx_message->data.modify_maxdelayratio.new_max_delay_ratio));
  if (status) {
    tx_message->status = htons(STT_SUCCESS);
  } else {
    tx_message->status = htons(STT_NOSUCHSOURCE);
  }
}

/* ================================================== */

static void
handle_modify_maxdelaydevratio(CMD_Request *rx_message, CMD_Reply *tx_message)
{
  int status;
  IPAddr address;
  UTI_IPNetworkToHost(&rx_message->data.modify_maxdelaydevratio.address, &address);
  status = NSR_ModifyMaxdelaydevratio(&address,
                                   UTI_FloatNetworkToHost(rx_message->data.modify_maxdelaydevratio.new_max_delay_dev_ratio));
  if (status) {
    tx_message->status = htons(STT_SUCCESS);
  } else {
    tx_message->status = htons(STT_NOSUCHSOURCE);
  }
}

/* ================================================== */

static void
handle_modify_minstratum(CMD_Request *rx_message, CMD_Reply *tx_message)
{
  int status;
  IPAddr address;
  UTI_IPNetworkToHost(&rx_message->data.modify_minpoll.address, &address);
  status = NSR_ModifyMinstratum(&address,
                             ntohl(rx_message->data.modify_minstratum.new_min_stratum));
  
  if (status) {
    tx_message->status = htons(STT_SUCCESS);
  } else {
    tx_message->status = htons(STT_NOSUCHSOURCE);
  }
}

/* ================================================== */

static void
handle_modify_polltarget(CMD_Request *rx_message, CMD_Reply *tx_message)
{
  int status;
  IPAddr address;
  UTI_IPNetworkToHost(&rx_message->data.modify_polltarget.address, &address);
  status = NSR_ModifyPolltarget(&address,
                             ntohl(rx_message->data.modify_polltarget.new_poll_target));
  
  if (status) {
    tx_message->status = htons(STT_SUCCESS);
  } else {
    tx_message->status = htons(STT_NOSUCHSOURCE);
  }
}

/* ================================================== */

static void
handle_modify_maxupdateskew(CMD_Request *rx_message, CMD_Reply *tx_message)
{
  REF_ModifyMaxupdateskew(UTI_FloatNetworkToHost(rx_message->data.modify_maxupdateskew.new_max_update_skew));
  tx_message->status = htons(STT_SUCCESS);
}

/* ================================================== */

static void
handle_settime(CMD_Request *rx_message, CMD_Reply *tx_message)
{
  struct timeval ts;
  long offset_cs;
  double dfreq_ppm, new_afreq_ppm;
  UTI_TimevalNetworkToHost(&rx_message->data.settime.ts, &ts);
  if (MNL_AcceptTimestamp(&ts, &offset_cs, &dfreq_ppm, &new_afreq_ppm)) {
    tx_message->status = htons(STT_SUCCESS);
    tx_message->reply = htons(RPY_MANUAL_TIMESTAMP);
    tx_message->data.manual_timestamp.centiseconds = htonl((int32_t)offset_cs);
    tx_message->data.manual_timestamp.dfreq_ppm = UTI_FloatHostToNetwork(dfreq_ppm);
    tx_message->data.manual_timestamp.new_afreq_ppm = UTI_FloatHostToNetwork(new_afreq_ppm);
  } else {
    tx_message->status = htons(STT_NOTENABLED);
  }
}

/* ================================================== */

static void
handle_local(CMD_Request *rx_message, CMD_Reply *tx_message)
{
  int on_off, stratum;
  on_off = ntohl(rx_message->data.local.on_off);
  if (on_off) {
    stratum = ntohl(rx_message->data.local.stratum);
    REF_EnableLocal(stratum);
  } else {
    REF_DisableLocal();
  }
  tx_message->status = htons(STT_SUCCESS);
}

/* ================================================== */

static void
handle_manual(CMD_Request *rx_message, CMD_Reply *tx_message)
{
  int option;
  option = ntohl(rx_message->data.manual.option);
  switch (option) {
    case 0:
      MNL_Disable();
      break;
    case 1:
      MNL_Enable();
      break;
    case 2:
      MNL_Reset();
      break;
  }
  tx_message->status = htons(STT_SUCCESS);
}

/* ================================================== */

static void
handle_n_sources(CMD_Request *rx_message, CMD_Reply *tx_message)
{
  int n_sources;
  n_sources = SRC_ReadNumberOfSources();
  tx_message->status = htons(STT_SUCCESS);
  tx_message->reply = htons(RPY_N_SOURCES);
  tx_message->data.n_sources.n_sources = htonl(n_sources);
}

/* ================================================== */

static void
handle_source_data(CMD_Request *rx_message, CMD_Reply *tx_message)
{
  RPT_SourceReport report;
  struct timeval now_corr;

  /* Get data */
  LCL_ReadCookedTime(&now_corr, NULL);
  if (SRC_ReportSource(ntohl(rx_message->data.source_data.index), &report, &now_corr)) {
    switch (SRC_GetType(ntohl(rx_message->data.source_data.index))) {
      case SRC_NTP:
        NSR_ReportSource(&report, &now_corr);
        break;
      case SRC_REFCLOCK:
        RCL_ReportSource(&report, &now_corr);
        break;
    }
    
    tx_message->status = htons(STT_SUCCESS);
    tx_message->reply  = htons(RPY_SOURCE_DATA);
    
    UTI_IPHostToNetwork(&report.ip_addr, &tx_message->data.source_data.ip_addr);
    tx_message->data.source_data.stratum = htons(report.stratum);
    tx_message->data.source_data.poll    = htons(report.poll);
    switch (report.state) {
      case RPT_SYNC:
        tx_message->data.source_data.state   = htons(RPY_SD_ST_SYNC);
        break;
      case RPT_UNREACH:
        tx_message->data.source_data.state   = htons(RPY_SD_ST_UNREACH);
        break;
      case RPT_FALSETICKER:
        tx_message->data.source_data.state   = htons(RPY_SD_ST_FALSETICKER);
        break;
      case RPT_JITTERY:
        tx_message->data.source_data.state   = htons(RPY_SD_ST_JITTERY);
        break;
      case RPT_CANDIDATE:
        tx_message->data.source_data.state   = htons(RPY_SD_ST_CANDIDATE);
        break;
      case RPT_OUTLIER:
        tx_message->data.source_data.state   = htons(RPY_SD_ST_OUTLIER);
        break;
    }
    switch (report.mode) {
      case RPT_NTP_CLIENT:
        tx_message->data.source_data.mode    = htons(RPY_SD_MD_CLIENT);
        break;
      case RPT_NTP_PEER:
        tx_message->data.source_data.mode    = htons(RPY_SD_MD_PEER);
        break;
      case RPT_LOCAL_REFERENCE:
        tx_message->data.source_data.mode    = htons(RPY_SD_MD_REF);
        break;
    }
    switch (report.sel_option) {
      case RPT_NORMAL:
        tx_message->data.source_data.flags = htons(0);
        break;
      case RPT_PREFER:
        tx_message->data.source_data.flags = htons(RPY_SD_FLAG_PREFER);
        break;
      case RPT_NOSELECT:
        tx_message->data.source_data.flags = htons(RPY_SD_FLAG_PREFER);
        break;
    }
    tx_message->data.source_data.reachability = htons(report.reachability);
    tx_message->data.source_data.since_sample = htonl(report.latest_meas_ago);
    tx_message->data.source_data.orig_latest_meas = UTI_FloatHostToNetwork(report.orig_latest_meas);
    tx_message->data.source_data.latest_meas = UTI_FloatHostToNetwork(report.latest_meas);
    tx_message->data.source_data.latest_meas_err = UTI_FloatHostToNetwork(report.latest_meas_err);
  } else {
    tx_message->status = htons(STT_NOSUCHSOURCE);
  }
}

/* ================================================== */

static void
handle_rekey(CMD_Request *rx_message, CMD_Reply *tx_message)
{
  tx_message->status = htons(STT_SUCCESS);
  KEY_Reload();
}

/* ================================================== */

static void
handle_allow(CMD_Request *rx_message, CMD_Reply *tx_message)
{
  IPAddr ip;
  int subnet_bits;
  UTI_IPNetworkToHost(&rx_message->data.allow_deny.ip, &ip);
  subnet_bits = ntohl(rx_message->data.allow_deny.subnet_bits);
  if (NCR_AddAccessRestriction(&ip, subnet_bits, 1, 0)) {
    tx_message->status = htons(STT_SUCCESS);
  } else {
    tx_message->status = htons(STT_BADSUBNET);
  }              
}

/* ================================================== */

static void
handle_allowall(CMD_Request *rx_message, CMD_Reply *tx_message)
{
  IPAddr ip;
  int subnet_bits;
  UTI_IPNetworkToHost(&rx_message->data.allow_deny.ip, &ip);
  subnet_bits = ntohl(rx_message->data.allow_deny.subnet_bits);
  if (NCR_AddAccessRestriction(&ip, subnet_bits, 1, 1)) {
    tx_message->status = htons(STT_SUCCESS);
  } else {
    tx_message->status = htons(STT_BADSUBNET);
  }              
}

/* ================================================== */

static void
handle_deny(CMD_Request *rx_message, CMD_Reply *tx_message)
{
  IPAddr ip;
  int subnet_bits;
  UTI_IPNetworkToHost(&rx_message->data.allow_deny.ip, &ip);
  subnet_bits = ntohl(rx_message->data.allow_deny.subnet_bits);
  if (NCR_AddAccessRestriction(&ip, subnet_bits, 0, 0)) {
    tx_message->status = htons(STT_SUCCESS);
  } else {
    tx_message->status = htons(STT_BADSUBNET);
  }              
}

/* ================================================== */

static void
handle_denyall(CMD_Request *rx_message, CMD_Reply *tx_message)
{
  IPAddr ip;
  int subnet_bits;
  UTI_IPNetworkToHost(&rx_message->data.allow_deny.ip, &ip);
  subnet_bits = ntohl(rx_message->data.allow_deny.subnet_bits);
  if (NCR_AddAccessRestriction(&ip, subnet_bits, 0, 1)) {
    tx_message->status = htons(STT_SUCCESS);
  } else {
    tx_message->status = htons(STT_BADSUBNET);
  }              
}

/* ================================================== */

static void
handle_cmdallow(CMD_Request *rx_message, CMD_Reply *tx_message)
{
  IPAddr ip;
  int subnet_bits;
  UTI_IPNetworkToHost(&rx_message->data.allow_deny.ip, &ip);
  subnet_bits = ntohl(rx_message->data.allow_deny.subnet_bits);
  if (CAM_AddAccessRestriction(&ip, subnet_bits, 1, 0)) {
    tx_message->status = htons(STT_SUCCESS);
  } else {
    tx_message->status = htons(STT_BADSUBNET);
  }              
}

/* ================================================== */

static void
handle_cmdallowall(CMD_Request *rx_message, CMD_Reply *tx_message)
{
  IPAddr ip;
  int subnet_bits;
  UTI_IPNetworkToHost(&rx_message->data.allow_deny.ip, &ip);
  subnet_bits = ntohl(rx_message->data.allow_deny.subnet_bits);
  if (CAM_AddAccessRestriction(&ip, subnet_bits, 1, 1)) {
    tx_message->status = htons(STT_SUCCESS);
  } else {
    tx_message->status = htons(STT_BADSUBNET);
  }              
}

/* ================================================== */

static void
handle_cmddeny(CMD_Request *rx_message, CMD_Reply *tx_message)
{
  IPAddr ip;
  int subnet_bits;
  UTI_IPNetworkToHost(&rx_message->data.allow_deny.ip, &ip);
  subnet_bits = ntohl(rx_message->data.allow_deny.subnet_bits);
  if (CAM_AddAccessRestriction(&ip, subnet_bits, 0, 0)) {
    tx_message->status = htons(STT_SUCCESS);
  } else {
    tx_message->status = htons(STT_BADSUBNET);
  }              
}

/* ================================================== */

static void
handle_cmddenyall(CMD_Request *rx_message, CMD_Reply *tx_message)
{
  IPAddr ip;
  int subnet_bits;
  UTI_IPNetworkToHost(&rx_message->data.allow_deny.ip, &ip);
  subnet_bits = ntohl(rx_message->data.allow_deny.subnet_bits);
  if (CAM_AddAccessRestriction(&ip, subnet_bits, 0, 1)) {
    tx_message->status = htons(STT_SUCCESS);
  } else {
    tx_message->status = htons(STT_BADSUBNET);
  }              
}

/* ================================================== */

static void
handle_accheck(CMD_Request *rx_message, CMD_Reply *tx_message)
{
  IPAddr ip;
  UTI_IPNetworkToHost(&rx_message->data.ac_check.ip, &ip);
  if (NCR_CheckAccessRestriction(&ip)) {
    tx_message->status = htons(STT_ACCESSALLOWED);
  } else {
    tx_message->status = htons(STT_ACCESSDENIED);
  }
}

/* ================================================== */

static void
handle_cmdaccheck(CMD_Request *rx_message, CMD_Reply *tx_message)
{
  IPAddr ip;
  UTI_IPNetworkToHost(&rx_message->data.ac_check.ip, &ip);
  if (CAM_CheckAccessRestriction(&ip)) {
    tx_message->status = htons(STT_ACCESSALLOWED);
  } else {
    tx_message->status = htons(STT_ACCESSDENIED);
  }
}

/* ================================================== */

static void
handle_add_source(NTP_Source_Type type, CMD_Request *rx_message, CMD_Reply *tx_message)
{
  NTP_Remote_Address rem_addr;
  SourceParameters params;
  NSR_Status status;
  
  UTI_IPNetworkToHost(&rx_message->data.ntp_source.ip_addr, &rem_addr.ip_addr);
  rem_addr.port = (unsigned short)(ntohl(rx_message->data.ntp_source.port));
  params.minpoll = ntohl(rx_message->data.ntp_source.minpoll);
  params.maxpoll = ntohl(rx_message->data.ntp_source.maxpoll);
  params.presend_minpoll = ntohl(rx_message->data.ntp_source.presend_minpoll);
  params.authkey = ntohl(rx_message->data.ntp_source.authkey);
  params.online  = ntohl(rx_message->data.ntp_source.flags) & REQ_ADDSRC_ONLINE ? 1 : 0;
  params.auto_offline = ntohl(rx_message->data.ntp_source.flags) & REQ_ADDSRC_AUTOOFFLINE ? 1 : 0;
  params.iburst = ntohl(rx_message->data.ntp_source.flags) & REQ_ADDSRC_IBURST ? 1 : 0;
  params.sel_option = ntohl(rx_message->data.ntp_source.flags) & REQ_ADDSRC_PREFER ? SRC_SelectPrefer :
                      ntohl(rx_message->data.ntp_source.flags) & REQ_ADDSRC_NOSELECT ? SRC_SelectNoselect : SRC_SelectNormal;
  params.max_delay = UTI_FloatNetworkToHost(rx_message->data.ntp_source.max_delay);
  params.max_delay_ratio = UTI_FloatNetworkToHost(rx_message->data.ntp_source.max_delay_ratio);

 /* not transmitted in cmdmon protocol yet */
  params.min_stratum = SRC_DEFAULT_MINSTRATUM;       
  params.poll_target = SRC_DEFAULT_POLLTARGET;
  params.max_delay_dev_ratio = SRC_DEFAULT_MAXDELAYDEVRATIO;

  status = NSR_AddSource(&rem_addr, type, &params);
  switch (status) {
    case NSR_Success:
      tx_message->status = htons(STT_SUCCESS);
      break;
    case NSR_AlreadyInUse:
      tx_message->status = htons(STT_SOURCEALREADYKNOWN);
      break;
    case NSR_TooManySources:
      tx_message->status = htons(STT_TOOMANYSOURCES);
      break;
    case NSR_InvalidAF:
      tx_message->status = htons(STT_INVALIDAF);
      break;
    case NSR_NoSuchSource:
      assert(0);
      break;
  }
}

/* ================================================== */

static void
handle_del_source(CMD_Request *rx_message, CMD_Reply *tx_message)
{
  NTP_Remote_Address rem_addr;
  NSR_Status status;
  
  UTI_IPNetworkToHost(&rx_message->data.del_source.ip_addr, &rem_addr.ip_addr);
  rem_addr.port = 0;
  
  status = NSR_RemoveSource(&rem_addr);
  switch (status) {
    case NSR_Success:
      tx_message->status = htons(STT_SUCCESS);
      break;
    case NSR_NoSuchSource:
      tx_message->status = htons(STT_NOSUCHSOURCE);
      break;
    case NSR_TooManySources:
    case NSR_AlreadyInUse:
    case NSR_InvalidAF:
      assert(0);
      break;
  }
}

/* ================================================== */

static void
handle_writertc(CMD_Request *rx_message, CMD_Reply *tx_message)
{
  switch (RTC_WriteParameters()) {
    case RTC_ST_OK:
      tx_message->status = htons(STT_SUCCESS);
      break;
    case RTC_ST_NODRV:
      tx_message->status = htons(STT_NORTC);
      break;
    case RTC_ST_BADFILE:
      tx_message->status = htons(STT_BADRTCFILE);
      break;
  }
}

/* ================================================== */

static void
handle_dfreq(CMD_Request *rx_message, CMD_Reply *tx_message)
{
  double dfreq;
  dfreq = UTI_FloatNetworkToHost(rx_message->data.dfreq.dfreq);
  LCL_AccumulateDeltaFrequency(dfreq * 1.0e-6);
  LOG(LOGS_INFO, LOGF_CmdMon, "Accumulated delta freq of %.3fppm", dfreq);
  tx_message->status = htons(STT_SUCCESS);
}

/* ================================================== */

static void
handle_doffset(CMD_Request *rx_message, CMD_Reply *tx_message)
{
  long sec, usec;
  double doffset;
  sec = (int32_t)ntohl(rx_message->data.doffset.sec);
  usec = (int32_t)ntohl(rx_message->data.doffset.usec);
  doffset = (double) sec + 1.0e-6 * (double) usec;
  LOG(LOGS_INFO, LOGF_CmdMon, "Accumulated delta offset of %.6f seconds", doffset);
  LCL_AccumulateOffset(doffset, 0.0);
  tx_message->status = htons(STT_SUCCESS);
}

/* ================================================== */

static void
handle_tracking(CMD_Request *rx_message, CMD_Reply *tx_message)
{
  RPT_TrackingReport rpt;

  REF_GetTrackingReport(&rpt);
  tx_message->status = htons(STT_SUCCESS);
  tx_message->reply  = htons(RPY_TRACKING);
  tx_message->data.tracking.ref_id = htonl(rpt.ref_id);
  UTI_IPHostToNetwork(&rpt.ip_addr, &tx_message->data.tracking.ip_addr);
  tx_message->data.tracking.stratum = htons(rpt.stratum);
  tx_message->data.tracking.leap_status = htons(rpt.leap_status);
  UTI_TimevalHostToNetwork(&rpt.ref_time, &tx_message->data.tracking.ref_time);
  tx_message->data.tracking.current_correction = UTI_FloatHostToNetwork(rpt.current_correction);
  tx_message->data.tracking.last_offset = UTI_FloatHostToNetwork(rpt.last_offset);
  tx_message->data.tracking.rms_offset = UTI_FloatHostToNetwork(rpt.rms_offset);
  tx_message->data.tracking.freq_ppm = UTI_FloatHostToNetwork(rpt.freq_ppm);
  tx_message->data.tracking.resid_freq_ppm = UTI_FloatHostToNetwork(rpt.resid_freq_ppm);
  tx_message->data.tracking.skew_ppm = UTI_FloatHostToNetwork(rpt.skew_ppm);
  tx_message->data.tracking.root_delay = UTI_FloatHostToNetwork(rpt.root_delay);
  tx_message->data.tracking.root_dispersion = UTI_FloatHostToNetwork(rpt.root_dispersion);
  tx_message->data.tracking.last_update_interval = UTI_FloatHostToNetwork(rpt.last_update_interval);
}

/* ================================================== */

static void
handle_sourcestats(CMD_Request *rx_message, CMD_Reply *tx_message)
{
  int status;
  RPT_SourcestatsReport report;
  struct timeval now_corr;

  LCL_ReadCookedTime(&now_corr, NULL);
  status = SRC_ReportSourcestats(ntohl(rx_message->data.sourcestats.index),
                                 &report, &now_corr);

  if (status) {
    tx_message->status = htons(STT_SUCCESS);
    tx_message->reply = htons(RPY_SOURCESTATS);
    tx_message->data.sourcestats.ref_id = htonl(report.ref_id);
    UTI_IPHostToNetwork(&report.ip_addr, &tx_message->data.sourcestats.ip_addr);
    tx_message->data.sourcestats.n_samples = htonl(report.n_samples);
    tx_message->data.sourcestats.n_runs = htonl(report.n_runs);
    tx_message->data.sourcestats.span_seconds = htonl(report.span_seconds);
    tx_message->data.sourcestats.resid_freq_ppm = UTI_FloatHostToNetwork(report.resid_freq_ppm);
    tx_message->data.sourcestats.skew_ppm = UTI_FloatHostToNetwork(report.skew_ppm);
    tx_message->data.sourcestats.sd = UTI_FloatHostToNetwork(report.sd);
    tx_message->data.sourcestats.est_offset = UTI_FloatHostToNetwork(report.est_offset);
    tx_message->data.sourcestats.est_offset_err = UTI_FloatHostToNetwork(report.est_offset_err);
  } else {
    tx_message->status = htons(STT_NOSUCHSOURCE);
  }
}

/* ================================================== */

static void
handle_rtcreport(CMD_Request *rx_message, CMD_Reply *tx_message)
{
  int status;
  RPT_RTC_Report report;
  status = RTC_GetReport(&report);
  if (status) {
    tx_message->status = htons(STT_SUCCESS);
    tx_message->reply  = htons(RPY_RTC);
    UTI_TimevalHostToNetwork(&report.ref_time, &tx_message->data.rtc.ref_time);
    tx_message->data.rtc.n_samples = htons(report.n_samples);
    tx_message->data.rtc.n_runs = htons(report.n_runs);
    tx_message->data.rtc.span_seconds = htonl(report.span_seconds);
    tx_message->data.rtc.rtc_seconds_fast = UTI_FloatHostToNetwork(report.rtc_seconds_fast);
    tx_message->data.rtc.rtc_gain_rate_ppm = UTI_FloatHostToNetwork(report.rtc_gain_rate_ppm);
  } else {
    tx_message->status = htons(STT_NORTC);
  }
}

/* ================================================== */

static void
handle_trimrtc(CMD_Request *rx_message, CMD_Reply *tx_message)
{
  int status;
  status = RTC_Trim();
  if (status) {
    tx_message->status = htons(STT_SUCCESS);
  } else {
    tx_message->status = htons(STT_NORTC);
  }
}

/* ================================================== */

static void
handle_cyclelogs(CMD_Request *rx_message, CMD_Reply *tx_message)
{
  LOG_CycleLogFiles();
  
  tx_message->status = htons(STT_SUCCESS);
}

/* ================================================== */

static void
handle_client_accesses_by_index(CMD_Request *rx_message, CMD_Reply *tx_message)
{
  CLG_Status result;
  RPT_ClientAccessByIndex_Report report;
  unsigned long first_index, n_indices, last_index, n_indices_in_table;
  int i, j;
  struct timeval now;

  LCL_ReadCookedTime(&now, NULL);

  first_index = ntohl(rx_message->data.client_accesses_by_index.first_index);
  n_indices = ntohl(rx_message->data.client_accesses_by_index.n_indices);
  last_index = first_index + n_indices - 1;

  tx_message->status = htons(STT_SUCCESS);
  tx_message->reply = htons(RPY_CLIENT_ACCESSES_BY_INDEX);

  for (i = first_index, j = 0;
       (i <= last_index) && (j < MAX_CLIENT_ACCESSES);
       i++) {

    result = CLG_GetClientAccessReportByIndex(i, &report, now.tv_sec, &n_indices_in_table);
    tx_message->data.client_accesses_by_index.n_indices = htonl(n_indices_in_table);

    switch (result) {
      case CLG_SUCCESS:
        UTI_IPHostToNetwork(&report.ip_addr, &tx_message->data.client_accesses_by_index.clients[j].ip);
        tx_message->data.client_accesses_by_index.clients[j].client_hits = htonl(report.client_hits);
        tx_message->data.client_accesses_by_index.clients[j].peer_hits = htonl(report.peer_hits);
        tx_message->data.client_accesses_by_index.clients[j].cmd_hits_auth = htonl(report.cmd_hits_auth);
        tx_message->data.client_accesses_by_index.clients[j].cmd_hits_normal = htonl(report.cmd_hits_normal);
        tx_message->data.client_accesses_by_index.clients[j].cmd_hits_bad = htonl(report.cmd_hits_bad);
        tx_message->data.client_accesses_by_index.clients[j].last_ntp_hit_ago = htonl(report.last_ntp_hit_ago);
        tx_message->data.client_accesses_by_index.clients[j].last_cmd_hit_ago = htonl(report.last_cmd_hit_ago);
        j++;
        break;
      case CLG_INDEXTOOLARGE:
        break; /* ignore this index */
      case CLG_INACTIVE:
        tx_message->status = htons(STT_INACTIVE);
        return;
      default:
        assert(0);
        break;
    }
  }

  tx_message->data.client_accesses_by_index.next_index = htonl(i);
  tx_message->data.client_accesses_by_index.n_clients = htonl(j);
}

/* ================================================== */

static void
handle_manual_list(CMD_Request *rx_message, CMD_Reply *tx_message)
{
  int n_samples;
  int i;
  RPY_ManualListSample *sample;
  RPT_ManualSamplesReport report[MAX_MANUAL_LIST_SAMPLES];

  tx_message->status = htons(STT_SUCCESS);
  tx_message->reply = htons(RPY_MANUAL_LIST);
  
  MNL_ReportSamples(report, MAX_MANUAL_LIST_SAMPLES, &n_samples);
  tx_message->data.manual_list.n_samples = htonl(n_samples);
  for (i=0; i<n_samples; i++) {
    sample = &tx_message->data.manual_list.samples[i];
    UTI_TimevalHostToNetwork(&report[i].when, &sample->when);
    sample->slewed_offset = UTI_FloatHostToNetwork(report[i].slewed_offset);
    sample->orig_offset = UTI_FloatHostToNetwork(report[i].orig_offset);
    sample->residual = UTI_FloatHostToNetwork(report[i].residual);
  }
}

/* ================================================== */

static void
handle_manual_delete(CMD_Request *rx_message, CMD_Reply *tx_message)
{
  int status;
  int index;

  index = ntohl(rx_message->data.manual_delete.index);
  status = MNL_DeleteSample(index);
  if (!status) {
    tx_message->status = htons(STT_BADSAMPLE);
  } else {
    tx_message->status = htons(STT_SUCCESS);
  }
}  

/* ================================================== */

static void
handle_make_step(CMD_Request *rx_message, CMD_Reply *tx_message)
{
  LCL_MakeStep(0.0);
  tx_message->status = htons(STT_SUCCESS);
}

/* ================================================== */

static void
handle_activity(CMD_Request *rx_message, CMD_Reply *tx_message)
{
  RPT_ActivityReport report;
  NSR_GetActivityReport(&report);
  tx_message->data.activity.online = htonl(report.online);
  tx_message->data.activity.offline = htonl(report.offline);
  tx_message->data.activity.burst_online = htonl(report.burst_online);
  tx_message->data.activity.burst_offline = htonl(report.burst_offline);
  tx_message->data.activity.unresolved = htonl(report.unresolved);
  tx_message->status = htons(STT_SUCCESS);
  tx_message->reply = htons(RPY_ACTIVITY);
}

/* ================================================== */

static void
handle_reselect_distance(CMD_Request *rx_message, CMD_Reply *tx_message)
{
  double dist;
  dist = UTI_FloatNetworkToHost(rx_message->data.reselect_distance.distance);
  SRC_SetReselectDistance(dist);
  tx_message->status = htons(STT_SUCCESS);
}

/* ================================================== */

static void
handle_reselect(CMD_Request *rx_message, CMD_Reply *tx_message)
{
  SRC_ReselectSource();
  tx_message->status = htons(STT_SUCCESS);
}

/* ================================================== */

#if 0
/* ================================================== */

static void
handle_(CMD_Request *rx_message, CMD_Reply *tx_message)
{
  int status;
}


#endif

/* ================================================== */
/* Read a packet and process it */

static void
read_from_cmd_socket(void *anything)
{
  int status;
  int read_length; /* Length of packet read */
  int expected_length; /* Expected length of packet without auth data */
  unsigned long flags;
  CMD_Request rx_message;
  CMD_Reply tx_message, *prev_tx_message;
  int rx_message_length, tx_message_length;
  int sock_fd;
  union sockaddr_in46 where_from;
  socklen_t from_length;
  IPAddr remote_ip;
  unsigned short remote_port;
  int auth_length;
  int auth_ok;
  int utoken_ok, token_ok;
  int issue_token;
  int valid_ts;
  int authenticated;
  int localhost;
  int allowed;
  unsigned short rx_command;
  unsigned long rx_message_token;
  unsigned long tx_message_token;
  unsigned long rx_message_seq;
  unsigned long rx_attempt;
  struct timeval now;
  struct timeval cooked_now;

  flags = 0;
  rx_message_length = sizeof(rx_message);
  from_length = sizeof(where_from);

  sock_fd = (long)anything;
  status = recvfrom(sock_fd, (char *)&rx_message, rx_message_length, flags,
                    &where_from.u, &from_length);

  if (status < 0) {
    LOG(LOGS_WARN, LOGF_CmdMon, "Error [%s] reading from control socket %d",
        strerror(errno), sock_fd);
    return;
  }

  read_length = status;

  LCL_ReadRawTime(&now);
  LCL_CookTime(&now, &cooked_now, NULL);

  switch (where_from.u.sa_family) {
    case AF_INET:
      remote_ip.family = IPADDR_INET4;
      remote_ip.addr.in4 = ntohl(where_from.in4.sin_addr.s_addr);
      remote_port = ntohs(where_from.in4.sin_port);
      localhost = (remote_ip.addr.in4 == 0x7f000001UL);
      break;
#ifdef HAVE_IPV6
    case AF_INET6:
      remote_ip.family = IPADDR_INET6;
      memcpy(&remote_ip.addr.in6, where_from.in6.sin6_addr.s6_addr,
          sizeof (remote_ip.addr.in6));
      remote_port = ntohs(where_from.in6.sin6_port);
      /* Check for ::1 */
      for (localhost = 0; localhost < 16; localhost++)
        if (remote_ip.addr.in6[localhost] != 0)
          break;
      localhost = (localhost == 15 && remote_ip.addr.in6[localhost] == 1);
      break;
#endif
    default:
      assert(0);
  }

  if (!(localhost || ADF_IsAllowed(access_auth_table, &remote_ip))) {
    /* The client is not allowed access, so don't waste any more time
       on him.  Note that localhost is always allowed access
       regardless of the defined access rules - otherwise, we could
       shut ourselves out completely! */
    return;
  }

  /* Message size sanity check */
  if (read_length >= offsetof(CMD_Request, data)) {
    expected_length = PKL_CommandLength(&rx_message);
  } else {
    expected_length = 0;
  }

  if (expected_length < offsetof(CMD_Request, data) ||
      read_length < offsetof(CMD_Reply, data) ||
      rx_message.pkt_type != PKT_TYPE_CMD_REQUEST ||
      rx_message.res1 != 0 ||
      rx_message.res2 != 0) {

    /* We don't know how to process anything like this */
    CLG_LogCommandAccess(&remote_ip, CLG_CMD_BAD_PKT, cooked_now.tv_sec);
    
    return;
  }

  rx_command = ntohs(rx_message.command);

  tx_message.version = PROTO_VERSION_NUMBER;
  tx_message.pkt_type = PKT_TYPE_CMD_REPLY;
  tx_message.res1 = 0;
  tx_message.res2 = 0;
  tx_message.command = rx_message.command;
  tx_message.sequence = rx_message.sequence;
  tx_message.reply = htons(RPY_NULL);
  tx_message.pad1 = 0;
  tx_message.pad2 = 0;
  tx_message.pad3 = 0;
  tx_message.utoken = htonl(utoken);
  /* Set this to a default (invalid) value.  This protects against the
     token field being set to an arbitrary value if we reject the
     message, e.g. due to the host failing the access check. */
  tx_message.token = htonl(0xffffffffUL);
  memset(&tx_message.auth, 0, sizeof(tx_message.auth));

  if (rx_message.version != PROTO_VERSION_NUMBER) {
    if (!LOG_RateLimited()) {
      LOG(LOGS_WARN, LOGF_CmdMon, "Read command packet with protocol version %d (expected %d) from %s:%hu", rx_message.version, PROTO_VERSION_NUMBER, UTI_IPToString(&remote_ip), remote_port);
    }

    CLG_LogCommandAccess(&remote_ip, CLG_CMD_BAD_PKT, cooked_now.tv_sec);

    if (rx_message.version >= PROTO_VERSION_MISMATCH_COMPAT_SERVER) {
      tx_message.status = htons(STT_BADPKTVERSION);
      transmit_reply(&tx_message, &where_from, 0);
    }
    return;
  }

  if (rx_command >= N_REQUEST_TYPES) {
    if (!LOG_RateLimited()) {
      LOG(LOGS_WARN, LOGF_CmdMon, "Read command packet with invalid command %d from %s:%hu", rx_command, UTI_IPToString(&remote_ip), remote_port);
    }

    CLG_LogCommandAccess(&remote_ip, CLG_CMD_BAD_PKT, cooked_now.tv_sec);

    tx_message.status = htons(STT_INVALID);
    transmit_reply(&tx_message, &where_from, 0);
    return;
  }

  if (read_length < expected_length) {
    if (!LOG_RateLimited()) {
      LOG(LOGS_WARN, LOGF_CmdMon, "Read incorrectly sized command packet from %s:%hu", UTI_IPToString(&remote_ip), remote_port);
    }

    CLG_LogCommandAccess(&remote_ip, CLG_CMD_BAD_PKT, cooked_now.tv_sec);

    tx_message.status = htons(STT_BADPKTLENGTH);
    transmit_reply(&tx_message, &where_from, 0);
    return;
  }

  /* OK, we have a valid message.  Now dispatch on message type and process it. */

  /* Do authentication stuff and command tokens here.  Well-behaved
     clients will set their utokens to 0 to save us wasting our time
     if the packet is unauthenticatable. */
  if (rx_message.utoken != 0) {
    auth_ok = check_rx_packet_auth(&rx_message, read_length);
  } else {
    auth_ok = 0;
  }

  /* All this malarky is to protect the system against various forms
     of attack.

     Simple packet forgeries are blocked by requiring the packet to
     authenticate properly with MD5 or other crypto hash.  (The
     assumption is that the command key is in a read-only keys file
     read by the daemon, and is known only to administrators.)

     Replay attacks are prevented by 2 fields in the packet.  The
     'token' field is where the client plays back to us a token that
     he was issued in an earlier reply.  Each time we reply to a
     suitable packet, we issue a new token.  The 'utoken' field is set
     to a new (hopefully increasing) value each time the daemon is
     run.  This prevents packets from a previous incarnation being
     played back at us when the same point in the 'token' sequence
     comes up.  (The token mechanism also prevents a non-idempotent
     command from being executed twice from the same client, if the
     client fails to receive our reply the first time and tries a
     resend.)

     The problem is how a client should get its first token.  Our
     token handling only remembers a finite number of issued tokens
     (actually 32) - if a client replies with a (legitimate) token
     older than that, it will be treated as though a duplicate token
     has been supplied.  If a simple token-request protocol were used,
     the whole thing would be vulnerable to a denial of service
     attack, where an attacker just replays valid token-request
     packets at us, causing us to keep issuing new tokens,
     invalidating all the ones we have given out to true clients
     already.

     To protect against this, the token-request (REQ_LOGON) packet
     includes a timestamp field.  To issue a token, we require that
     this field is different from any we have processed before.  To
     bound our storage, we require that the timestamp is within a
     certain period of our current time.  For clients running on the
     same host this will be easily satisfied.

     */

  utoken_ok = (ntohl(rx_message.utoken) == utoken);

  /* Avoid binning a valid user's token if we merely get a forged
     packet */
  rx_message_token = ntohl(rx_message.token);
  rx_message_seq = ntohl(rx_message.sequence);
  rx_attempt = ntohs(rx_message.attempt);

  if (auth_ok && utoken_ok) {
    token_ok = check_token(rx_message_token);
  } else {
    token_ok = 0;
  }

  if (auth_ok && utoken_ok && !token_ok) {
    /* This might be a resent message, due to the client not getting
       our reply to the first attempt.  See if we can find the message. */
    prev_tx_message = lookup_reply(rx_message_token, rx_message_seq, rx_attempt);
    if (prev_tx_message) {
      /* Just send this message again */
      tx_message_length = PKL_ReplyLength(prev_tx_message);
      status = sendto(sock_fd, (void *) prev_tx_message, tx_message_length, 0,
                      &where_from.u, from_length);
      if (status < 0 && !LOG_RateLimited()) {
        LOG(LOGS_WARN, LOGF_CmdMon, "Could not send response to %s:%hu", UTI_IPToString(&remote_ip), remote_port);
      }
      return;
    }
    /* Otherwise, just fall through into normal processing */

  }

  if (auth_ok && utoken_ok && token_ok) {
    /* See whether we can discard the previous reply from storage */
    token_acknowledged(rx_message_token, &now);
  }

  valid_ts = 0;

  if (auth_ok) {
    struct timeval ts;

    UTI_TimevalNetworkToHost(&rx_message.data.logon.ts, &ts);
    if ((utoken_ok && token_ok) ||
        ((ntohl(rx_message.utoken) == SPECIAL_UTOKEN) &&
         (rx_command == REQ_LOGON) &&
         (valid_ts = ts_is_unique_and_not_stale(&ts, &now))))
      issue_token = 1;
    else 
      issue_token = 0;
  } else {
    issue_token = 0;
  }

  authenticated = auth_ok & utoken_ok & token_ok;

  if (authenticated) {
    CLG_LogCommandAccess(&remote_ip, CLG_CMD_AUTH, cooked_now.tv_sec);
  } else {
    CLG_LogCommandAccess(&remote_ip, CLG_CMD_NORMAL, cooked_now.tv_sec);
  }

  if (issue_token) {
    /* Only command clients where the user has apparently 'logged on'
       get a token to allow them to emit an authenticated command next
       time */
    tx_message_token = get_token();
  } else {
    tx_message_token = 0xffffffffUL;
  }

  tx_message.token = htonl(tx_message_token);


  if (rx_command >= N_REQUEST_TYPES) {
    /* This should be already handled */
    assert(0);
  } else {
    /* Check level of authority required to issue the command */
    switch(permissions[rx_command]) {
      case PERMIT_AUTH:
        if (authenticated) {
          allowed = 1;
        } else {
          allowed = 0;
        }
        break;
      case PERMIT_LOCAL:
        if (authenticated || localhost) {
          allowed = 1;
        } else {
          allowed = 0;
        }
        break;
      case PERMIT_OPEN:
        allowed = 1;
        break;
      default:
        assert(0);
        allowed = 0;
    }

    if (allowed) {
      switch(rx_command) {
        case REQ_NULL:
          handle_null(&rx_message, &tx_message);
          break;

        case REQ_ONLINE:
          handle_online(&rx_message, &tx_message);
          break;

        case REQ_OFFLINE:
          handle_offline(&rx_message, &tx_message);
          break;

        case REQ_BURST:
          handle_burst(&rx_message, &tx_message);
          break;

        case REQ_MODIFY_MINPOLL:
          handle_modify_minpoll(&rx_message, &tx_message);
          break;

        case REQ_MODIFY_MAXPOLL:
          handle_modify_maxpoll(&rx_message, &tx_message);
          break;

        case REQ_DUMP:
          SRC_DumpSources();
          tx_message.status = htons(STT_SUCCESS);
          break;

        case REQ_MODIFY_MAXDELAY:
          handle_modify_maxdelay(&rx_message, &tx_message);
          break;

        case REQ_MODIFY_MAXDELAYRATIO:
          handle_modify_maxdelayratio(&rx_message, &tx_message);
          break;

        case REQ_MODIFY_MAXDELAYDEVRATIO:
          handle_modify_maxdelaydevratio(&rx_message, &tx_message);
          break;

        case REQ_MODIFY_MAXUPDATESKEW:
          handle_modify_maxupdateskew(&rx_message, &tx_message);
          break;

        case REQ_LOGON:
          /* If the log-on fails, record the reason why */
          if (!issue_token && !LOG_RateLimited()) {
            LOG(LOGS_WARN, LOGF_CmdMon,
                "Bad command logon from %s port %d (auth_ok=%d valid_ts=%d)",
                UTI_IPToString(&remote_ip),
                remote_port,
                auth_ok, valid_ts);
          }

          if (issue_token == 1) {
            tx_message.status = htons(STT_SUCCESS);
          } else if (!auth_ok) {
            tx_message.status = htons(STT_UNAUTH);
          } else if (!valid_ts) {
            tx_message.status = htons(STT_INVALIDTS);
          } else {
            tx_message.status = htons(STT_FAILED);
          }
            
          break;

        case REQ_SETTIME:
          handle_settime(&rx_message, &tx_message);
          break;
        
        case REQ_LOCAL:
          handle_local(&rx_message, &tx_message);
          break;

        case REQ_MANUAL:
          handle_manual(&rx_message, &tx_message);
          break;

        case REQ_N_SOURCES:
          handle_n_sources(&rx_message, &tx_message);
          break;

        case REQ_SOURCE_DATA:
          handle_source_data(&rx_message, &tx_message);
          break;

        case REQ_REKEY:
          handle_rekey(&rx_message, &tx_message);
          break;

        case REQ_ALLOW:
          handle_allow(&rx_message, &tx_message);
          break;

        case REQ_ALLOWALL:
          handle_allowall(&rx_message, &tx_message);
          break;

        case REQ_DENY:
          handle_deny(&rx_message, &tx_message);
          break;

        case REQ_DENYALL:
          handle_denyall(&rx_message, &tx_message);
          break;

        case REQ_CMDALLOW:
          handle_cmdallow(&rx_message, &tx_message);
          break;

        case REQ_CMDALLOWALL:
          handle_cmdallowall(&rx_message, &tx_message);
          break;

        case REQ_CMDDENY:
          handle_cmddeny(&rx_message, &tx_message);
          break;

        case REQ_CMDDENYALL:
          handle_cmddenyall(&rx_message, &tx_message);
          break;

        case REQ_ACCHECK:
          handle_accheck(&rx_message, &tx_message);
          break;

        case REQ_CMDACCHECK:
          handle_cmdaccheck(&rx_message, &tx_message);
          break;

        case REQ_ADD_SERVER:
          handle_add_source(NTP_SERVER, &rx_message, &tx_message);
          break;

        case REQ_ADD_PEER:
          handle_add_source(NTP_PEER, &rx_message, &tx_message);
          break;

        case REQ_DEL_SOURCE:
          handle_del_source(&rx_message, &tx_message);
          break;

        case REQ_WRITERTC:
          handle_writertc(&rx_message, &tx_message);
          break;
          
        case REQ_DFREQ:
          handle_dfreq(&rx_message, &tx_message);
          break;

        case REQ_DOFFSET:
          handle_doffset(&rx_message, &tx_message);
          break;

        case REQ_TRACKING:
          handle_tracking(&rx_message, &tx_message);
          break;

        case REQ_SOURCESTATS:
          handle_sourcestats(&rx_message, &tx_message);
          break;

        case REQ_RTCREPORT:
          handle_rtcreport(&rx_message, &tx_message);
          break;
          
        case REQ_TRIMRTC:
          handle_trimrtc(&rx_message, &tx_message);
          break;

        case REQ_CYCLELOGS:
          handle_cyclelogs(&rx_message, &tx_message);
          break;

        case REQ_CLIENT_ACCESSES_BY_INDEX:
          handle_client_accesses_by_index(&rx_message, &tx_message);
          break;

        case REQ_MANUAL_LIST:
          handle_manual_list(&rx_message, &tx_message);
          break;

        case REQ_MANUAL_DELETE:
          handle_manual_delete(&rx_message, &tx_message);
          break;

        case REQ_MAKESTEP:
          handle_make_step(&rx_message, &tx_message);
          break;

        case REQ_ACTIVITY:
          handle_activity(&rx_message, &tx_message);
          break;

        case REQ_RESELECTDISTANCE:
          handle_reselect_distance(&rx_message, &tx_message);
          break;

        case REQ_RESELECT:
          handle_reselect(&rx_message, &tx_message);
          break;

        case REQ_MODIFY_MINSTRATUM:
          handle_modify_minstratum(&rx_message, &tx_message);
          break;

        case REQ_MODIFY_POLLTARGET:
          handle_modify_polltarget(&rx_message, &tx_message);
          break;

        default:
          assert(0);
          break;
      }
    } else {
      tx_message.status = htons(STT_UNAUTH);
    }
  }

  if (auth_ok) {
    auth_length = generate_tx_packet_auth(&tx_message);
  } else {
    auth_length = 0;
  }

  if (token_ok) {
    save_reply(&tx_message,
               rx_message_token,
               tx_message_token,
               rx_message_seq,
               rx_attempt,
               &now);
  }

  /* Transmit the response */
  {
    /* Include a simple way to lose one message in three to test resend */

    static int do_it=1;

    if (do_it) {
      transmit_reply(&tx_message, &where_from, auth_length);
    }

#if 0
    do_it = ((do_it + 1) % 3);
#endif
  }
}

/* ================================================== */

int
CAM_AddAccessRestriction(IPAddr *ip_addr, int subnet_bits, int allow, int all)
 {
  ADF_Status status;

  if (allow) {
    if (all) {
      status = ADF_AllowAll(access_auth_table, ip_addr, subnet_bits);
    } else {
      status = ADF_Allow(access_auth_table, ip_addr, subnet_bits);
    }
  } else {
    if (all) {
      status = ADF_DenyAll(access_auth_table, ip_addr, subnet_bits);
    } else {
      status = ADF_Deny(access_auth_table, ip_addr, subnet_bits);
    }
  }

  if (status == ADF_BADSUBNET) {
    return 0;
  } else if (status == ADF_SUCCESS) {
    return 1;
  } else {
    return 0;
  }
}

/* ================================================== */

int
CAM_CheckAccessRestriction(IPAddr *ip_addr)
{
  return ADF_IsAllowed(access_auth_table, ip_addr);
}


/* ================================================== */
/* ================================================== */
