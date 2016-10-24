/*
  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Miroslav Lichvar  2016
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

  Functions for NTP I/O specific to Linux
  */

#include "config.h"

#include "sysincl.h"

#include <linux/errqueue.h>
#include <linux/ethtool.h>
#include <linux/net_tstamp.h>
#include <linux/sockios.h>

#include "array.h"
#include "conf.h"
#include "local.h"
#include "logging.h"
#include "ntp_core.h"
#include "ntp_io.h"
#include "ntp_io_linux.h"
#include "ntp_sources.h"
#include "sched.h"
#include "sys_linux.h"
#include "util.h"

union sockaddr_in46 {
  struct sockaddr_in in4;
#ifdef FEAT_IPV6
  struct sockaddr_in6 in6;
#endif
  struct sockaddr u;
};

/* RX/TX and TX-specific timestamping socket options */
static int ts_flags;
static int ts_tx_flags;

/* Flag indicating the socket options can't be changed in control messages */
static int permanent_ts_options;

/* ================================================== */

void
NIO_Linux_Initialise(void)
{
  ts_flags = SOF_TIMESTAMPING_SOFTWARE | SOF_TIMESTAMPING_RX_SOFTWARE;
  ts_tx_flags = SOF_TIMESTAMPING_TX_SOFTWARE;

  /* Enable IP_PKTINFO in messages looped back to the error queue */
  ts_flags |= SOF_TIMESTAMPING_OPT_CMSG;

  /* Kernels before 4.7 ignore timestamping flags set in control messages */
  permanent_ts_options = !SYS_Linux_CheckKernelVersion(4, 7);
}

/* ================================================== */

void
NIO_Linux_Finalise(void)
{
}

/* ================================================== */

int
NIO_Linux_SetTimestampSocketOptions(int sock_fd, int client_only, int *events)
{
  int val, flags;

  if (!ts_flags)
    return 0;

  /* Enable SCM_TIMESTAMPING control messages and the socket's error queue in
     order to receive our transmitted packets with more accurate timestamps */

  val = 1;
  flags = ts_flags;

  if (client_only || permanent_ts_options)
    flags |= ts_tx_flags;

  if (setsockopt(sock_fd, SOL_SOCKET, SO_SELECT_ERR_QUEUE, &val, sizeof (val)) < 0) {
    LOG(LOGS_ERR, LOGF_NtpIOLinux, "Could not set %s socket option", "SO_SELECT_ERR_QUEUE");
    ts_flags = 0;
    return 0;
  }

  if (setsockopt(sock_fd, SOL_SOCKET, SO_TIMESTAMPING, &flags, sizeof (flags)) < 0) {
    LOG(LOGS_ERR, LOGF_NtpIOLinux, "Could not set %s socket option", "SO_TIMESTAMPING");
    ts_flags = 0;
    return 0;
  }

  *events |= SCH_FILE_EXCEPTION;
  return 1;
}

/* ================================================== */
/* Extract UDP data from a layer 2 message.  Supported is Ethernet
   with optional VLAN tags. */

static int
extract_udp_data(unsigned char *msg, NTP_Remote_Address *remote_addr, int len)
{
  unsigned char *msg_start = msg;
  union sockaddr_in46 addr;

  remote_addr->ip_addr.family = IPADDR_UNSPEC;
  remote_addr->port = 0;

  /* Skip MACs */
  if (len < 12)
    return 0;
  len -= 12, msg += 12;

  /* Skip VLAN tag(s) if present */
  while (len >= 4 && msg[0] == 0x81 && msg[1] == 0x00)
    len -= 4, msg += 4;

  /* Skip IPv4 or IPv6 ethertype */
  if (len < 2 || !((msg[0] == 0x08 && msg[1] == 0x00) ||
                   (msg[0] == 0x86 && msg[1] == 0xdd)))
    return 0;
  len -= 2, msg += 2;

  /* Parse destination address and port from IPv4/IPv6 and UDP headers */
  if (len >= 20 && msg[0] >> 4 == 4) {
    int ihl = (msg[0] & 0xf) * 4;

    if (len < ihl + 8 || msg[9] != 17)
      return 0;

    memcpy(&addr.in4.sin_addr.s_addr, msg + 16, sizeof (uint32_t));
    addr.in4.sin_port = *(uint16_t *)(msg + ihl + 2);
    addr.in4.sin_family = AF_INET;
    len -= ihl + 8, msg += ihl + 8;
#ifdef FEAT_IPV6
  } else if (len >= 48 && msg[0] >> 4 == 6) {
    /* IPv6 extension headers are not supported */
    if (msg[6] != 17)
      return 0;

    memcpy(&addr.in6.sin6_addr.s6_addr, msg + 24, 16);
    addr.in6.sin6_port = *(uint16_t *)(msg + 40 + 2);
    addr.in6.sin6_family = AF_INET6;
    len -= 48, msg += 48;
#endif
  } else {
    return 0;
  }

  UTI_SockaddrToIPAndPort(&addr.u, &remote_addr->ip_addr, &remote_addr->port);

  /* Move the message to fix alignment of its fields */
  if (len > 0)
    memmove(msg_start, msg, len);

  return len;
}

/* ================================================== */

int
NIO_Linux_ProcessMessage(NTP_Remote_Address *remote_addr, NTP_Local_Address *local_addr,
                         NTP_Local_Timestamp *local_ts, struct msghdr *hdr,
                         int length, int sock_fd, int if_index)
{
  struct cmsghdr *cmsg;

  for (cmsg = CMSG_FIRSTHDR(hdr); cmsg; cmsg = CMSG_NXTHDR(hdr, cmsg)) {
    if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_TIMESTAMPING) {
      struct scm_timestamping ts3;

      memcpy(&ts3, CMSG_DATA(cmsg), sizeof (ts3));

      if (!UTI_IsZeroTimespec(&ts3.ts[0])) {
        LCL_CookTime(&ts3.ts[0], &local_ts->ts, &local_ts->err);
        local_ts->source = NTP_TS_KERNEL;
      }
    }

    if ((cmsg->cmsg_level == SOL_IP && cmsg->cmsg_type == IP_RECVERR) ||
        (cmsg->cmsg_level == SOL_IPV6 && cmsg->cmsg_type == IPV6_RECVERR)) {
      struct sock_extended_err err;

      memcpy(&err, CMSG_DATA(cmsg), sizeof (err));

      if (err.ee_errno != ENOMSG || err.ee_info != SCM_TSTAMP_SND ||
          err.ee_origin != SO_EE_ORIGIN_TIMESTAMPING) {
        DEBUG_LOG(LOGF_NtpIOLinux, "Unknown extended error");
        /* Drop the message */
        return 1;
      }
    }
  }

  /* Return the message if it's not received from the error queue */
  if (!(hdr->msg_flags & MSG_ERRQUEUE))
    return 0;

  /* The data from the error queue includes all layers up to UDP.  We have to
     extract the UDP data and also the destination address with port as there
     currently doesn't seem to be a better way to get them both. */
  length = extract_udp_data(hdr->msg_iov[0].iov_base, remote_addr, length);

  DEBUG_LOG(LOGF_NtpIOLinux, "Received %d bytes from error queue for %s:%d fd=%d if=%d tss=%d",
            length, UTI_IPToString(&remote_addr->ip_addr), remote_addr->port,
            sock_fd, if_index, local_ts->source);

  if (length < NTP_NORMAL_PACKET_LENGTH)
    return 1;

  NSR_ProcessTx(remote_addr, local_addr, local_ts,
                (NTP_Packet *)hdr->msg_iov[0].iov_base, length);

  return 1;
}

/* ================================================== */

int
NIO_Linux_RequestTxTimestamp(struct msghdr *msg, int cmsglen, int sock_fd)
{
  struct cmsghdr *cmsg;

  /* Check if TX timestamping is disabled on this socket */
  if (permanent_ts_options || !NIO_IsServerSocket(sock_fd))
    return cmsglen;

  /* Add control message that will enable TX timestamping for this message.
     Don't use CMSG_NXTHDR as the one in glibc is buggy for creating new
     control messages. */
  cmsg = (struct cmsghdr *)((char *)CMSG_FIRSTHDR(msg) + cmsglen);
  memset(cmsg, 0, CMSG_SPACE(sizeof (ts_tx_flags)));
  cmsglen += CMSG_SPACE(sizeof (ts_tx_flags));

  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type = SO_TIMESTAMPING;
  cmsg->cmsg_len = CMSG_LEN(sizeof (ts_tx_flags));

  memcpy(CMSG_DATA(cmsg), &ts_tx_flags, sizeof (ts_tx_flags));

  return cmsglen;
}
