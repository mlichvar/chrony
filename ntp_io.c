/*
  $Header: /cvs/src/chrony/ntp_io.c,v 1.24 2003/09/22 21:22:30 richard Exp $

  =======================================================================

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
 * 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA
 * 
 **********************************************************************

  =======================================================================

  This file deals with the IO aspects of reading and writing NTP packets
  */

#include "sysincl.h"

#include "ntp_io.h"
#include "ntp_core.h"
#include "ntp_sources.h"
#include "sched.h"
#include "local.h"
#include "logging.h"
#include "conf.h"
#include "util.h"

#include <fcntl.h>

/* The file descriptor for the socket */
static int sock_fd;

/* Flag indicating that we have been initialised */
static int initialised=0;

/* ================================================== */

/* Forward prototypes */
static void read_from_socket(void *anything);

/* ================================================== */

static void
do_size_checks(void)
{
  /* Assertions to check the sizes of certain data types
     and the positions of certain record fields */

  /* Check that certain invariants are true */
  assert(sizeof(NTP_int32) == 4);
  assert(sizeof(NTP_int64) == 8);

  /* Check offsets of all fields in the NTP packet format */
  assert(offsetof(NTP_Packet, lvm)             ==  0);
  assert(offsetof(NTP_Packet, stratum)         ==  1);
  assert(offsetof(NTP_Packet, poll)            ==  2);
  assert(offsetof(NTP_Packet, precision)       ==  3);
  assert(offsetof(NTP_Packet, root_delay)      ==  4);
  assert(offsetof(NTP_Packet, root_dispersion) ==  8);
  assert(offsetof(NTP_Packet, reference_id)    == 12);
  assert(offsetof(NTP_Packet, reference_ts)    == 16);
  assert(offsetof(NTP_Packet, originate_ts)    == 24);
  assert(offsetof(NTP_Packet, receive_ts)      == 32);
  assert(offsetof(NTP_Packet, transmit_ts)     == 40);

}

/* ================================================== */

void
NIO_Initialise(void)
{
  struct sockaddr_in my_addr;
  unsigned short port_number;
  unsigned long bind_address;
  int on_off = 1;
  
  assert(!initialised);
  initialised = 1;

  do_size_checks();

  port_number = CNF_GetNTPPort();

  /* Open Internet domain UDP socket for NTP message transmissions */

#if 0
  sock_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#else
  sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
#endif 
  if (sock_fd < 0) {
    LOG_FATAL(LOGF_NtpIO, "Could not open socket : %s", strerror(errno));
  }

  /* Make the socket capable of re-using an old address */
  if (setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, (char *)&on_off, sizeof(on_off)) < 0) {
    LOG(LOGS_ERR, LOGF_NtpIO, "Could not set reuseaddr socket options");
    /* Don't quit - we might survive anyway */
  }
  
  /* Make the socket capable of sending broadcast pkts - needed for NTP broadcast mode */
  if (setsockopt(sock_fd, SOL_SOCKET, SO_BROADCAST, (char *)&on_off, sizeof(on_off)) < 0) {
    LOG(LOGS_ERR, LOGF_NtpIO, "Could not set broadcast socket options");
    /* Don't quit - we might survive anyway */
  }

  /* Bind the port */
  my_addr.sin_family = AF_INET;
  my_addr.sin_port = htons(port_number);

  CNF_GetBindAddress(&bind_address);

  if (bind_address != 0UL) {
    my_addr.sin_addr.s_addr = htonl(bind_address);
  } else {
    my_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  }

#if 0
  LOG(LOGS_INFO, LOGF_NtpIO, "Initialising, socket fd=%d", sock_fd);
#endif

  if (bind(sock_fd, (struct sockaddr *) &my_addr, sizeof(my_addr)) < 0) {
    LOG_FATAL(LOGF_NtpIO, "Could not bind socket : %s", strerror(errno));
  }

  /* Register handler for read events on the socket */
  SCH_AddInputFileHandler(sock_fd, read_from_socket, NULL);

#if 0
  if (fcntl(sock_fd, F_SETFL, O_NONBLOCK | O_NDELAY) < 0) {
    LOG(LOGS_ERR, LOGF_NtpIO, "Could not make socket non-blocking");
  }

  if (ioctl(sock_fd, I_SETSIG, S_INPUT) < 0) {
    LOG(LOGS_ERR, LOGF_NtpIO, "Could not enable signal");
  }
#endif

  return;
}

/* ================================================== */

void
NIO_Finalise(void)
{
  if (sock_fd >= 0) {
    SCH_RemoveInputFileHandler(sock_fd);
    close(sock_fd);
  }
  sock_fd = -1;
  initialised = 0;
  return;
}

/* ================================================== */


/* ================================================== */

static void
read_from_socket(void *anything)
{
  /* This should only be called when there is something
     to read, otherwise it will block. */

  int status;
  ReceiveBuffer message;
  int message_length;
  struct sockaddr_in where_from;
  socklen_t from_length;
  unsigned int flags = 0;
  struct timeval now;
  NTP_Remote_Address remote_addr;
  double local_clock_err;

  assert(initialised);

  from_length = sizeof(where_from);
  message_length = sizeof(message);

  LCL_ReadCookedTime(&now, &local_clock_err);
  status = recvfrom(sock_fd, (char *)&message, message_length, flags,
                    (struct sockaddr *)&where_from, &from_length);

  /* Don't bother checking if read failed or why if it did.  More
     likely than not, it will be connection refused, resulting from a
     previous sendto() directing a datagram at a port that is not
     listening (which appears to generate an ICMP response, and on
     some architectures e.g. Linux this is translated into an error
     reponse on a subsequent recvfrom). */

  if (status > 0) {
    remote_addr.ip_addr = ntohl(where_from.sin_addr.s_addr);
    remote_addr.port = ntohs(where_from.sin_port);

    if (status == NTP_NORMAL_PACKET_SIZE) {

      NSR_ProcessReceive((NTP_Packet *) &message.ntp_pkt, &now, &remote_addr);

    } else if (status == sizeof(NTP_Packet)) {

      NSR_ProcessAuthenticatedReceive((NTP_Packet *) &message.ntp_pkt, &now, &remote_addr);

    } else {

      /* Just ignore the packet if it's not of a recognized length */

    }
  }
  
  return;
}

/* ================================================== */
/* Send an unauthenticated packet to a given address */

void
NIO_SendNormalPacket(NTP_Packet *packet, NTP_Remote_Address *remote_addr)
{
  struct sockaddr_in remote;

  assert(initialised);

  remote.sin_family = AF_INET;
  remote.sin_port = htons(remote_addr->port);
  remote.sin_addr.s_addr = htonl(remote_addr->ip_addr);

  if (sendto(sock_fd, (void *) packet, NTP_NORMAL_PACKET_SIZE, 0,
             (struct sockaddr *) &remote, sizeof(remote)) < 0 &&
      !LOG_RateLimited()) {
    LOG(LOGS_WARN, LOGF_NtpIO, "Could not send to %s:%d : %s",
        UTI_IPToDottedQuad(remote_addr->ip_addr), remote_addr->port, strerror(errno));
  }

  return;
}

/* ================================================== */
/* Send an authenticated packet to a given address */

void
NIO_SendAuthenticatedPacket(NTP_Packet *packet, NTP_Remote_Address *remote_addr)
{
  struct sockaddr_in remote;

  assert(initialised);

  remote.sin_family = AF_INET;
  remote.sin_port = htons(remote_addr->port);
  remote.sin_addr.s_addr = htonl(remote_addr->ip_addr);

  if (sendto(sock_fd, (void *) packet, sizeof(NTP_Packet), 0,
             (struct sockaddr *) &remote, sizeof(remote)) < 0 &&
      !LOG_RateLimited()) {
    LOG(LOGS_WARN, LOGF_NtpIO, "Could not send to %s:%d : %s",
        UTI_IPToDottedQuad(remote_addr->ip_addr), remote_addr->port, strerror(errno));
  }

  return;
}

/* ================================================== */

/* We ought to use getservbyname, but I can't really see this changing */
#define ECHO_PORT 7

void
NIO_SendEcho(NTP_Remote_Address *remote_addr)
{
  unsigned long magic_message = 0xbe7ab1e7UL;
  struct sockaddr_in addr;

  addr.sin_family = AF_INET;
  addr.sin_port = htons(ECHO_PORT);
  addr.sin_addr.s_addr = htonl(remote_addr->ip_addr);

  /* Just ignore error status on send - this is not a big deal anyway */
  sendto(sock_fd, (void *) &magic_message, sizeof(unsigned long), 0,
         (struct sockaddr *) &addr, sizeof(addr));


}
