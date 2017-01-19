/*
  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Miroslav Lichvar  2016-2017
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

#include <ifaddrs.h>
#include <linux/errqueue.h>
#include <linux/ethtool.h>
#include <linux/net_tstamp.h>
#include <linux/ptp_clock.h>
#include <linux/sockios.h>
#include <net/if.h>

#include "array.h"
#include "conf.h"
#include "hwclock.h"
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

struct Interface {
  char name[IF_NAMESIZE];
  int if_index;
  int phc_fd;
  /* Link speed in mbit/s */
  int link_speed;
  /* Start of UDP data at layer 2 for IPv4 and IPv6 */
  int l2_udp4_ntp_start;
  int l2_udp6_ntp_start;
  /* Compensation of errors in TX and RX timestamping */
  double tx_comp;
  double rx_comp;
  HCL_Instance clock;
};

/* Number of PHC readings per HW clock sample */
#define PHC_READINGS 10

/* Maximum acceptable offset between HW and daemon/kernel timestamp */
#define MAX_TS_DELAY 1.0

/* Array of Interfaces */
static ARR_Instance interfaces;

/* RX/TX and TX-specific timestamping socket options */
static int ts_flags;
static int ts_tx_flags;

/* Flag indicating the socket options can't be changed in control messages */
static int permanent_ts_options;

/* ================================================== */

static int
add_interface(CNF_HwTsInterface *conf_iface)
{
  struct ethtool_ts_info ts_info;
  struct hwtstamp_config ts_config;
  struct ifreq req;
  int sock_fd, if_index, phc_index, phc_fd;
  unsigned int i;
  struct Interface *iface;
  char phc_path[64];

  /* Check if the interface was not already added */
  for (i = 0; i < ARR_GetSize(interfaces); i++) {
    if (!strcmp(conf_iface->name, ((struct Interface *)ARR_GetElement(interfaces, i))->name))
      return 1;
  }

  sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock_fd < 0)
    return 0;

  memset(&req, 0, sizeof (req));
  memset(&ts_info, 0, sizeof (ts_info));

  if (snprintf(req.ifr_name, sizeof (req.ifr_name), "%s", conf_iface->name) >=
      sizeof (req.ifr_name)) {
    close(sock_fd);
    return 0;
  }

  if (ioctl(sock_fd, SIOCGIFINDEX, &req)) {
    DEBUG_LOG(LOGF_NtpIOLinux, "ioctl(%s) failed : %s", "SIOCGIFINDEX", strerror(errno));
    close(sock_fd);
    return 0;
  }

  if_index = req.ifr_ifindex;

  ts_info.cmd = ETHTOOL_GET_TS_INFO;
  req.ifr_data = (char *)&ts_info;

  if (ioctl(sock_fd, SIOCETHTOOL, &req)) {
    DEBUG_LOG(LOGF_NtpIOLinux, "ioctl(%s) failed : %s", "SIOCETHTOOL", strerror(errno));
    close(sock_fd);
    return 0;
  }

  ts_config.flags = 0;
  ts_config.tx_type = HWTSTAMP_TX_ON;
  ts_config.rx_filter = HWTSTAMP_FILTER_ALL;
  req.ifr_data = (char *)&ts_config;

  if (ioctl(sock_fd, SIOCSHWTSTAMP, &req)) {
    DEBUG_LOG(LOGF_NtpIOLinux, "ioctl(%s) failed : %s", "SIOCSHWTSTAMP", strerror(errno));
    close(sock_fd);
    return 0;
  }

  close(sock_fd);
  phc_index = ts_info.phc_index;

  if (snprintf(phc_path, sizeof (phc_path), "/dev/ptp%d", phc_index) >= sizeof (phc_path))
    return 0;

  phc_fd = open(phc_path, O_RDONLY);
  if (phc_fd < 0) {
    LOG(LOGS_ERR, LOGF_NtpIOLinux, "Could not open %s : %s", phc_path, strerror(errno));
    return 0;
  }

  UTI_FdSetCloexec(phc_fd);

  iface = ARR_GetNewElement(interfaces);

  snprintf(iface->name, sizeof (iface->name), "%s", conf_iface->name);
  iface->if_index = if_index;
  iface->phc_fd = phc_fd;

  /* Start with 1 gbit and no VLANs or IPv4/IPv6 options */
  iface->link_speed = 1000;
  iface->l2_udp4_ntp_start = 42;
  iface->l2_udp6_ntp_start = 62;

  iface->tx_comp = conf_iface->tx_comp;
  iface->rx_comp = conf_iface->rx_comp;

  iface->clock = HCL_CreateInstance();

  DEBUG_LOG(LOGF_NtpIOLinux, "Enabled HW timestamping on %s", iface->name);

  return 1;
}

/* ================================================== */

static int
add_all_interfaces(CNF_HwTsInterface *conf_iface_all)
{
  CNF_HwTsInterface conf_iface;
  struct ifaddrs *ifaddr, *ifa;
  int r;

  conf_iface = *conf_iface_all;

  if (getifaddrs(&ifaddr)) {
    DEBUG_LOG(LOGF_NtpIOLinux, "getifaddrs() failed : %s", strerror(errno));
    return 0;
  }

  for (r = 0, ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
    conf_iface.name = ifa->ifa_name;
    if (add_interface(&conf_iface))
      r = 1;
  }
  
  freeifaddrs(ifaddr);

  /* Return success if at least one interface was added */
  return r;
}

/* ================================================== */

static void
update_interface_speed(struct Interface *iface)
{
  struct ethtool_cmd cmd;
  struct ifreq req;
  int sock_fd;

  sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock_fd < 0)
    return;

  memset(&req, 0, sizeof (req));
  memset(&cmd, 0, sizeof (cmd));

  snprintf(req.ifr_name, sizeof (req.ifr_name), "%s", iface->name);
  cmd.cmd = ETHTOOL_GSET;
  req.ifr_data = (char *)&cmd;

  if (ioctl(sock_fd, SIOCETHTOOL, &req)) {
    DEBUG_LOG(LOGF_NtpIOLinux, "ioctl(%s) failed : %s", "SIOCETHTOOL", strerror(errno));
    close(sock_fd);
    return;
  }

  close(sock_fd);

  iface->link_speed = ethtool_cmd_speed(&cmd);
}

/* ================================================== */

void
NIO_Linux_Initialise(void)
{
  CNF_HwTsInterface *conf_iface;
  unsigned int i;
  int hwts;

  interfaces = ARR_CreateInstance(sizeof (struct Interface));

  /* Enable HW timestamping on specified interfaces.  If "*" was specified, try
     all interfaces.  If no interface was specified, enable SW timestamping. */

  for (i = hwts = 0; CNF_GetHwTsInterface(i, &conf_iface); i++) {
    if (!strcmp("*", conf_iface->name))
      continue;
    if (!add_interface(conf_iface))
      LOG_FATAL(LOGF_NtpIO, "Could not enable HW timestamping on %s", conf_iface->name);
    hwts = 1;
  }

  for (i = 0; CNF_GetHwTsInterface(i, &conf_iface); i++) {
    if (strcmp("*", conf_iface->name))
      continue;
    if (add_all_interfaces(conf_iface))
      hwts = 1;
    break;
  }

  if (hwts) {
    ts_flags = SOF_TIMESTAMPING_RAW_HARDWARE | SOF_TIMESTAMPING_RX_HARDWARE;
    ts_tx_flags = SOF_TIMESTAMPING_TX_HARDWARE;
  } else {
    ts_flags = SOF_TIMESTAMPING_SOFTWARE | SOF_TIMESTAMPING_RX_SOFTWARE;
    ts_tx_flags = SOF_TIMESTAMPING_TX_SOFTWARE;
  }

  /* Enable IP_PKTINFO in messages looped back to the error queue */
  ts_flags |= SOF_TIMESTAMPING_OPT_CMSG;

  /* Kernels before 4.7 ignore timestamping flags set in control messages */
  permanent_ts_options = !SYS_Linux_CheckKernelVersion(4, 7);
}

/* ================================================== */

void
NIO_Linux_Finalise(void)
{
  struct Interface *iface;
  unsigned int i;

  for (i = 0; i < ARR_GetSize(interfaces); i++) {
    iface = ARR_GetElement(interfaces, i);
    HCL_DestroyInstance(iface->clock);
    close(iface->phc_fd);
  }

  ARR_DestroyInstance(interfaces);
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

static int
get_phc_sample(int phc_fd, struct timespec *phc_ts, struct timespec *local_ts, double *p_delay)
{
  struct ptp_sys_offset sys_off;
  struct timespec ts1, ts2, ts3, phc_tss[PHC_READINGS], sys_tss[PHC_READINGS];
  double min_delay = 0.0, delays[PHC_READINGS], phc_sum, local_sum, local_prec;
  int i, n;

  /* Silence valgrind */
  memset(&sys_off, 0, sizeof (sys_off));

  sys_off.n_samples = PHC_READINGS;

  if (ioctl(phc_fd, PTP_SYS_OFFSET, &sys_off)) {
    DEBUG_LOG(LOGF_NtpIOLinux, "ioctl(%s) failed : %s", "PTP_SYS_OFFSET", strerror(errno));
    return 0;
  }

  for (i = 0; i < PHC_READINGS; i++) {
    ts1.tv_sec = sys_off.ts[i * 2].sec;
    ts1.tv_nsec = sys_off.ts[i * 2].nsec;
    ts2.tv_sec = sys_off.ts[i * 2 + 1].sec;
    ts2.tv_nsec = sys_off.ts[i * 2 + 1].nsec;
    ts3.tv_sec = sys_off.ts[i * 2 + 2].sec;
    ts3.tv_nsec = sys_off.ts[i * 2 + 2].nsec;

    sys_tss[i] = ts1;
    phc_tss[i] = ts2;
    delays[i] = UTI_DiffTimespecsToDouble(&ts3, &ts1);

    if (delays[i] <= 0.0)
      /* Step in the middle of a PHC reading? */
      return 0;

    if (!i || delays[i] < min_delay)
      min_delay = delays[i];
  }

  local_prec = LCL_GetSysPrecisionAsQuantum();

  /* Combine best readings */
  for (i = n = 0, phc_sum = local_sum = 0.0; i < PHC_READINGS; i++) {
    if (delays[i] > min_delay + local_prec)
      continue;

    phc_sum += UTI_DiffTimespecsToDouble(&phc_tss[i], &phc_tss[0]);
    local_sum += UTI_DiffTimespecsToDouble(&sys_tss[i], &sys_tss[0]) + delays[i] / 2.0;
    n++;
  }

  assert(n);

  UTI_AddDoubleToTimespec(&phc_tss[0], phc_sum / n, phc_ts);
  UTI_AddDoubleToTimespec(&sys_tss[0], local_sum / n, &ts1);
  LCL_CookTime(&ts1, local_ts, NULL);
  *p_delay = min_delay;

  return 1;
}

/* ================================================== */

static struct Interface *
get_interface(int if_index)
{
  struct Interface *iface;
  unsigned int i;

  for (i = 0; i < ARR_GetSize(interfaces); i++) {
    iface = ARR_GetElement(interfaces, i);
    if (iface->if_index != if_index)
      continue;

    return iface;
  }

  return NULL;
}

/* ================================================== */

static void
process_hw_timestamp(struct Interface *iface, struct timespec *hw_ts,
                     NTP_Local_Timestamp *local_ts, int rx_ntp_length, int family)
{
  struct timespec sample_phc_ts, sample_local_ts, ts;
  double sample_delay, rx_correction, ts_delay, err;
  int l2_length;

  if (HCL_NeedsNewSample(iface->clock, &local_ts->ts)) {
    if (!get_phc_sample(iface->phc_fd, &sample_phc_ts, &sample_local_ts, &sample_delay))
      return;

    HCL_AccumulateSample(iface->clock, &sample_phc_ts, &sample_local_ts,
                         sample_delay / 2.0);

    update_interface_speed(iface);
  }

  /* We need to transpose RX timestamps as hardware timestamps are normally
     preamble timestamps and RX timestamps in NTP are supposed to be trailer
     timestamps.  Without raw sockets we don't know the length of the packet
     at layer 2, so we make an assumption that UDP data start at the same
     position as in the last transmitted packet which had a HW TX timestamp. */
  if (rx_ntp_length && iface->link_speed) {
    l2_length = (family == IPADDR_INET4 ? iface->l2_udp4_ntp_start :
                 iface->l2_udp6_ntp_start) + rx_ntp_length + 4;
    rx_correction = l2_length / (1.0e6 / 8 * iface->link_speed);

    UTI_AddDoubleToTimespec(hw_ts, rx_correction, hw_ts);
  }

  if (!rx_ntp_length && iface->tx_comp)
    UTI_AddDoubleToTimespec(hw_ts, iface->tx_comp, hw_ts);
  else if (rx_ntp_length && iface->rx_comp)
    UTI_AddDoubleToTimespec(hw_ts, -iface->rx_comp, hw_ts);

  if (!HCL_CookTime(iface->clock, hw_ts, &ts, &err))
    return;

  ts_delay = UTI_DiffTimespecsToDouble(&local_ts->ts, &ts);

  if (fabs(ts_delay) > MAX_TS_DELAY) {
    DEBUG_LOG(LOGF_NtpIOLinux, "Unacceptable timestamp delay %.9f", ts_delay);
    return;
  }

  local_ts->ts = ts;
  local_ts->err = err;
  local_ts->source = NTP_TS_HARDWARE;
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
  struct Interface *iface;
  struct cmsghdr *cmsg;
  int is_tx, l2_length;

  is_tx = hdr->msg_flags & MSG_ERRQUEUE;
  iface = NULL;

  for (cmsg = CMSG_FIRSTHDR(hdr); cmsg; cmsg = CMSG_NXTHDR(hdr, cmsg)) {
    if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_TIMESTAMPING) {
      struct scm_timestamping ts3;

      memcpy(&ts3, CMSG_DATA(cmsg), sizeof (ts3));

      if (!UTI_IsZeroTimespec(&ts3.ts[0])) {
        LCL_CookTime(&ts3.ts[0], &local_ts->ts, &local_ts->err);
        local_ts->source = NTP_TS_KERNEL;
      } else if (!UTI_IsZeroTimespec(&ts3.ts[2])) {
        iface = get_interface(if_index);
        if (iface) {
          process_hw_timestamp(iface, &ts3.ts[2], local_ts, !is_tx ? length : 0,
                               remote_addr->ip_addr.family);
        } else {
          DEBUG_LOG(LOGF_NtpIOLinux, "HW clock not found for interface %d", if_index);
        }
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
  if (!is_tx)
    return 0;

  /* The data from the error queue includes all layers up to UDP.  We have to
     extract the UDP data and also the destination address with port as there
     currently doesn't seem to be a better way to get them both. */
  l2_length = length;
  length = extract_udp_data(hdr->msg_iov[0].iov_base, remote_addr, length);

  DEBUG_LOG(LOGF_NtpIOLinux, "Received %d (%d) bytes from error queue for %s:%d fd=%d if=%d tss=%d",
            l2_length, length, UTI_IPToString(&remote_addr->ip_addr), remote_addr->port,
            sock_fd, if_index, local_ts->source);

  /* Update assumed position of UDP data at layer 2 for next received packet */
  if (iface && length) {
    if (remote_addr->ip_addr.family == IPADDR_INET4)
      iface->l2_udp4_ntp_start = l2_length - length;
    else if (remote_addr->ip_addr.family == IPADDR_INET6)
      iface->l2_udp6_ntp_start = l2_length - length;
  }

  /* Drop the message if HW timestamp is missing or its processing failed */
  if ((ts_flags & SOF_TIMESTAMPING_RAW_HARDWARE) && local_ts->source != NTP_TS_HARDWARE) {
    DEBUG_LOG(LOGF_NtpIOLinux, "Missing HW timestamp");
    return 1;
  }

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
