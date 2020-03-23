/*
  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Miroslav Lichvar  2020
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

  NTS-KE client
  */

#include "config.h"

#include "sysincl.h"

#include "nts_ke_client.h"

#include "conf.h"
#include "logging.h"
#include "memory.h"
#include "nameserv_async.h"
#include "nts_ke_session.h"
#include "siv.h"
#include "socket.h"
#include "util.h"

#define CLIENT_TIMEOUT 16.0

struct NKC_Instance_Record {
  char *name;
  IPSockAddr address;
  NKSN_Instance session;
  int destroying;
  int got_response;
  int resolving_name;

  SIV_Algorithm siv_algorithm;
  NKE_Key c2s, s2c;
  NKE_Cookie cookies[NKE_MAX_COOKIES];
  int num_cookies;
  char server_name[NKE_MAX_RECORD_BODY_LENGTH + 1];
  IPSockAddr ntp_address;
};

/* ================================================== */

static void *client_credentials;

/* ================================================== */

static void
name_resolve_handler(DNS_Status status, int n_addrs, IPAddr *ip_addrs, void *arg)
{
  NKC_Instance inst = arg;
  int i;

  inst->resolving_name = 0;

  if (inst->destroying) {
    NKC_DestroyInstance(inst);
    return;
  }

  if (status != DNS_Success || n_addrs < 1) {
    LOG(LOGS_ERR, "Could not resolve NTP server %s from %s", inst->server_name, inst->name);
    /* Force restart */
    inst->got_response = 0;
    return;
  }

  inst->ntp_address.ip_addr = ip_addrs[0];

  /* Prefer an address of the same family as NTS-KE */
  for (i = 0; i < n_addrs; i++) {
    DEBUG_LOG("%s resolved to %s", inst->server_name, UTI_IPToString(&ip_addrs[i]));
    if (ip_addrs[i].family == inst->address.ip_addr.family) {
      inst->ntp_address.ip_addr = ip_addrs[i];
      break;
    }
  }
}

/* ================================================== */

static int
prepare_request(NKC_Instance inst)
{
  NKSN_Instance session = inst->session;
  uint16_t datum;

  NKSN_BeginMessage(session);

  datum = htons(NKE_NEXT_PROTOCOL_NTPV4);
  if (!NKSN_AddRecord(session, 1, NKE_RECORD_NEXT_PROTOCOL, &datum, sizeof (datum)))
    return 0;

  datum = htons(AEAD_AES_SIV_CMAC_256);
  if (!NKSN_AddRecord(session, 1, NKE_RECORD_AEAD_ALGORITHM, &datum, sizeof (datum)))
    return 0;

  if (!NKSN_EndMessage(session))
    return 0;

  return 1;
}

/* ================================================== */

static int
process_response(NKC_Instance inst)
{
  int next_protocol = -1, aead_algorithm = -1, error = 0;
  int i, critical, type, length;
  uint16_t data[NKE_MAX_COOKIE_LENGTH / sizeof (uint16_t)];

  assert(NKE_MAX_COOKIE_LENGTH % sizeof (uint16_t) == 0);
  assert(sizeof (uint16_t) == 2);

  inst->num_cookies = 0;
  inst->ntp_address.ip_addr.family = IPADDR_UNSPEC;
  inst->ntp_address.port = 0;
  inst->server_name[0] = '\0';

  while (!error) {
    if (!NKSN_GetRecord(inst->session, &critical, &type, &length, &data, sizeof (data)))
      break;

    switch (type) {
      case NKE_RECORD_NEXT_PROTOCOL:
        if (!critical || length != 2 || ntohs(data[0]) != NKE_NEXT_PROTOCOL_NTPV4) {
          DEBUG_LOG("Unexpected NTS-KE next protocol");
          error = 1;
          break;
        }
        next_protocol = NKE_NEXT_PROTOCOL_NTPV4;
        break;
      case NKE_RECORD_AEAD_ALGORITHM:
        if (length != 2 || ntohs(data[0]) != AEAD_AES_SIV_CMAC_256) {
          DEBUG_LOG("Unexpected NTS-KE AEAD algorithm");
          error = 1;
          break;
        }
        aead_algorithm = AEAD_AES_SIV_CMAC_256;
        inst->siv_algorithm = aead_algorithm;
        break;
      case NKE_RECORD_ERROR:
        if (length == 2)
          DEBUG_LOG("NTS-KE error %d", ntohs(data[0]));
        error = 1;
        break;
      case NKE_RECORD_WARNING:
        if (length == 2)
          DEBUG_LOG("NTS-KE warning %d", ntohs(data[0]));
        error = 1;
        break;
      case NKE_RECORD_COOKIE:
        DEBUG_LOG("Got cookie #%d length=%d", inst->num_cookies + 1, length);
        assert(NKE_MAX_COOKIE_LENGTH == sizeof (inst->cookies[inst->num_cookies].cookie));
        if (length <= NKE_MAX_COOKIE_LENGTH && inst->num_cookies < NKE_MAX_COOKIES) {
          inst->cookies[inst->num_cookies].length = length;
          memcpy(inst->cookies[inst->num_cookies].cookie, data, length);
          inst->num_cookies++;
        }
        break;
      case NKE_RECORD_NTPV4_SERVER_NEGOTIATION:
        if (length < 1 || length >= sizeof (inst->server_name)) {
          DEBUG_LOG("Invalid server name");
          error = 1;
          break;
        }

        memcpy(inst->server_name, data, length);
        inst->server_name[length] = '\0';

        /* Make sure the name is printable and has no spaces */
        for (i = 0; i < length && isgraph(inst->server_name[i]); i++)
          ;
        if (i != length) {
          DEBUG_LOG("Invalid server name");
          error = 1;
          break;
        }

        DEBUG_LOG("Negotiated server %s", inst->server_name);
        break;
      case NKE_RECORD_NTPV4_PORT_NEGOTIATION:
        if (length != 2) {
          DEBUG_LOG("Invalid port");
          error = 1;
          break;
        }
        inst->ntp_address.port = ntohs(data[0]);
        DEBUG_LOG("Negotiated port %d", inst->ntp_address.port);
        break;
      default:
        DEBUG_LOG("Unknown record type=%d length=%d critical=%d", type, length, critical);
        if (critical)
          error = 1;
    }
  }

  DEBUG_LOG("NTS-KE response: error=%d next=%d aead=%d",
            error, next_protocol, aead_algorithm);

  if (error || inst->num_cookies == 0 ||
      next_protocol != NKE_NEXT_PROTOCOL_NTPV4 ||
      aead_algorithm != AEAD_AES_SIV_CMAC_256)
    return 0;

  return 1;
}

/* ================================================== */

static int
handle_message(void *arg)
{
  NKC_Instance inst = arg;

  if (!process_response(inst)) {
    LOG(LOGS_ERR, "Received invalid NTS-KE response from %s", inst->name);
    return 0;
  }

  if (!NKSN_GetKeys(inst->session, inst->siv_algorithm, &inst->c2s, &inst->s2c))
    return 0;

  if (inst->server_name[0] != '\0') {
    if (inst->resolving_name)
      return 0;
    if (!UTI_StringToIP(inst->server_name, &inst->ntp_address.ip_addr)) {
      DNS_Name2IPAddressAsync(inst->server_name, name_resolve_handler, inst);
      inst->resolving_name = 1;
    }
  }

  inst->got_response = 1;

  return 1;
}

/* ================================================== */

void
NKC_Initialise(void)
{
  client_credentials = NULL;
}

/* ================================================== */

void
NKC_Finalise(void)
{
  if (client_credentials)
    NKSN_DestroyCertCredentials(client_credentials);
}

/* ================================================== */

NKC_Instance
NKC_CreateInstance(IPSockAddr *address, const char *name)
{
  NKC_Instance inst;

  inst = MallocNew(struct NKC_Instance_Record);

  inst->address = *address;
  inst->name = Strdup(name);
  inst->session = NKSN_CreateInstance(0, inst->name, handle_message, inst);
  inst->resolving_name = 0;
  inst->destroying = 0;
  inst->got_response = 0;

  /* Create the credentials with the first client instance and share them
     with other instances */
  if (!client_credentials)
    client_credentials = NKSN_CreateCertCredentials(NULL, NULL, CNF_GetNtsTrustedCertFile());

  return inst;
}

/* ================================================== */

void
NKC_DestroyInstance(NKC_Instance inst)
{
  /* If the resolver is running, destroy the instance later when finished */
  if (inst->resolving_name) {
    inst->destroying = 1;
    return;
  }

  NKSN_DestroyInstance(inst->session);

  Free(inst->name);
  Free(inst);
}

/* ================================================== */

int
NKC_Start(NKC_Instance inst)
{
  IPSockAddr local_addr;
  int sock_fd;

  assert(!NKC_IsActive(inst));

  if (!client_credentials) {
    DEBUG_LOG("Missing client credentials");
    return 0;
  }

  /* Follow the bindacqaddress setting */
  CNF_GetBindAcquisitionAddress(inst->address.ip_addr.family, &local_addr.ip_addr);
  if (local_addr.ip_addr.family != inst->address.ip_addr.family)
    SCK_GetAnyLocalIPAddress(inst->address.ip_addr.family, &local_addr.ip_addr);

  local_addr.port = 0;

  sock_fd = SCK_OpenTcpSocket(&inst->address, &local_addr, 0);
  if (sock_fd < 0)
    return 0;

  /* Start a NTS-KE session */
  if (!NKSN_StartSession(inst->session, sock_fd, inst->name,
                         client_credentials, CLIENT_TIMEOUT)) {
    SCK_CloseSocket(sock_fd);
    return 0;
  }

  /* Send a request */
  if (!prepare_request(inst)) {
    DEBUG_LOG("Could not prepare NTS-KE request");
    NKSN_StopSession(inst->session);
    return 0;
  }

  return 1;
}

/* ================================================== */

int
NKC_IsActive(NKC_Instance inst)
{
  return !NKSN_IsStopped(inst->session) || inst->resolving_name;
}

/* ================================================== */

int
NKC_GetNtsData(NKC_Instance inst,
               SIV_Algorithm *siv_algorithm, NKE_Key *c2s, NKE_Key *s2c,
               NKE_Cookie *cookies, int *num_cookies, int max_cookies,
               IPSockAddr *ntp_address)
{
  int i;

  if (!inst->got_response || inst->resolving_name)
    return 0;

  *siv_algorithm = inst->siv_algorithm;
  *c2s = inst->c2s;
  *s2c = inst->s2c;

  for (i = 0; i < inst->num_cookies && i < max_cookies; i++)
    cookies[i] = inst->cookies[i];
  *num_cookies = i;

  *ntp_address = inst->ntp_address;

  return i;
}
