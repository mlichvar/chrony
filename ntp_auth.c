/*
  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Miroslav Lichvar  2019
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

  NTP authentication
  */

#include "config.h"

#include "sysincl.h"

#include "keys.h"
#include "logging.h"
#include "memory.h"
#include "ntp_auth.h"
#include "ntp_ext.h"
#include "ntp_signd.h"
#include "nts_ntp.h"
#include "nts_ntp_client.h"
#include "nts_ntp_server.h"
#include "srcparams.h"
#include "util.h"

/* Structure to hold authentication configuration and state */

struct NAU_Instance_Record {
  NTP_AuthMode mode;            /* Authentication mode of NTP packets */
  uint32_t key_id;              /* Identifier of a symmetric key */
  NNC_Instance nts;             /* Client NTS state */
};

/* ================================================== */

static int
generate_symmetric_auth(uint32_t key_id, NTP_Packet *packet, NTP_PacketInfo *info)
{
  int auth_len, max_auth_len;

  /* Truncate long MACs in NTPv4 packets to allow deterministic parsing
     of extension fields (RFC 7822) */
  max_auth_len = (info->version == 4 ? NTP_MAX_V4_MAC_LENGTH : NTP_MAX_MAC_LENGTH) - 4;
  max_auth_len = MIN(max_auth_len, sizeof (NTP_Packet) - info->length - 4);

  auth_len = KEY_GenerateAuth(key_id, (unsigned char *)packet, info->length,
                              (unsigned char *)packet + info->length + 4, max_auth_len);
  if (!auth_len) {
    DEBUG_LOG("Could not generate auth data with key %"PRIu32, key_id);
    return 0;
  }

  *(uint32_t *)((unsigned char *)packet + info->length) = htonl(key_id);
  info->length += 4 + auth_len;

  return 1;
}

/* ================================================== */

static int
check_symmetric_auth(NTP_Packet *packet, NTP_PacketInfo *info)
{
  int trunc_len;

  if (info->auth.mac.length < NTP_MIN_MAC_LENGTH)
    return 0;

  trunc_len = info->version == 4 && info->auth.mac.length <= NTP_MAX_V4_MAC_LENGTH ?
              NTP_MAX_V4_MAC_LENGTH : NTP_MAX_MAC_LENGTH;

  if (!KEY_CheckAuth(info->auth.mac.key_id, (void *)packet, info->auth.mac.start,
                     (unsigned char *)packet + info->auth.mac.start + 4,
                     info->auth.mac.length - 4, trunc_len - 4))
    return 0;

  return 1;
}

/* ================================================== */

static void
adjust_timestamp(NTP_AuthMode mode, uint32_t key_id, struct timespec *ts)
{
  switch (mode) {
    case NTP_AUTH_SYMMETRIC:
      ts->tv_nsec += KEY_GetAuthDelay(key_id);
      UTI_NormaliseTimespec(ts);
      break;
    case NTP_AUTH_MSSNTP:
      ts->tv_nsec += NSD_GetAuthDelay(key_id);
      UTI_NormaliseTimespec(ts);
    default:
      break;
  }
}

/* ================================================== */

static int
is_zero_data(unsigned char *data, int length)
{
  int i;

  for (i = 0; i < length; i++)
    if (data[i])
      return 0;
  return 1;
}

/* ================================================== */

static NAU_Instance
create_instance(NTP_AuthMode mode)
{
  NAU_Instance instance;

  instance = MallocNew(struct NAU_Instance_Record);
  instance->mode = mode;
  instance->key_id = INACTIVE_AUTHKEY;
  instance->nts = NULL;

  assert(sizeof (instance->key_id) == 4);

  return instance;
}

/* ================================================== */

NAU_Instance
NAU_CreateNoneInstance(void)
{
  return create_instance(NTP_AUTH_NONE);
}

/* ================================================== */

NAU_Instance
NAU_CreateSymmetricInstance(uint32_t key_id)
{
  NAU_Instance instance = create_instance(NTP_AUTH_SYMMETRIC);

  instance->key_id = key_id;

  if (!KEY_KeyKnown(key_id))
    LOG(LOGS_WARN, "Key %"PRIu32" is %s", key_id, "missing");
  else if (!KEY_CheckKeyLength(key_id))
    LOG(LOGS_WARN, "Key %"PRIu32" is %s", key_id, "too short");

  return instance;
}

/* ================================================== */

NAU_Instance
NAU_CreateNtsInstance(IPSockAddr *nts_address, const char *name, const IPSockAddr *ntp_address)
{
  NAU_Instance instance = create_instance(NTP_AUTH_NTS);

  instance->nts = NNC_CreateInstance(nts_address, name, ntp_address);

  return instance;
}

/* ================================================== */

void
NAU_DestroyInstance(NAU_Instance instance)
{
  if (instance->nts)
    NNC_DestroyInstance(instance->nts);
  Free(instance);
}

/* ================================================== */

int
NAU_IsAuthEnabled(NAU_Instance instance)
{
  return instance->mode != NTP_AUTH_NONE;
}

/* ================================================== */

int
NAU_GetSuggestedNtpVersion(NAU_Instance instance)
{
  /* If the MAC in NTPv4 packets would be truncated, prefer NTPv3 for
     compatibility with older chronyd servers */
  if (instance->mode == NTP_AUTH_SYMMETRIC &&
      KEY_GetAuthLength(instance->key_id) + sizeof (instance->key_id) > NTP_MAX_V4_MAC_LENGTH)
    return 3;

  return NTP_VERSION;
}

/* ================================================== */

int
NAU_PrepareRequestAuth(NAU_Instance instance)
{
  switch (instance->mode) {
    case NTP_AUTH_NTS:
      if (!NNC_PrepareForAuth(instance->nts))
        return 0;
      break;
    default:
      break;
  }

  return 1;
}

/* ================================================== */

void
NAU_AdjustRequestTimestamp(NAU_Instance instance, struct timespec *ts)
{
  adjust_timestamp(instance->mode, instance->key_id, ts);
}

/* ================================================== */

int
NAU_GenerateRequestAuth(NAU_Instance instance, NTP_Packet *request, NTP_PacketInfo *info)
{
  switch (instance->mode) {
    case NTP_AUTH_NONE:
      break;
    case NTP_AUTH_SYMMETRIC:
      if (!generate_symmetric_auth(instance->key_id, request, info))
        return 0;
      break;
    case NTP_AUTH_NTS:
      if (!NNC_GenerateRequestAuth(instance->nts, request, info))
        return 0;
      break;
    default:
      assert(0);
  }

  return 1;
}

/* ================================================== */

int
NAU_ParsePacket(NTP_Packet *packet, NTP_PacketInfo *info)
{
  int parsed, remainder, ef_length, ef_type;
  unsigned char *data;

  data = (void *)packet;
  parsed = NTP_HEADER_LENGTH;
  remainder = info->length - parsed;

  info->ext_fields = 0;

  /* Check if this is a plain NTP packet with no extension fields or MAC */
  if (remainder <= 0)
    return 1;

  /* In NTPv3 and older packets don't have extension fields.  Anything after
     the header is assumed to be a MAC. */
  if (info->version <= 3) {
    info->auth.mode = NTP_AUTH_SYMMETRIC;
    info->auth.mac.start = parsed;
    info->auth.mac.length = remainder;
    info->auth.mac.key_id = ntohl(*(uint32_t *)(data + parsed));

    /* Check if it is an MS-SNTP authenticator field or extended authenticator
       field with zeroes as digest */
    if (info->version == 3 && info->auth.mac.key_id) {
      if (remainder == 20 && is_zero_data(data + parsed + 4, remainder - 4))
        info->auth.mode = NTP_AUTH_MSSNTP;
      else if (remainder == 72 && is_zero_data(data + parsed + 8, remainder - 8))
        info->auth.mode = NTP_AUTH_MSSNTP_EXT;
    }

    return 1;
  }

  /* Check for a crypto NAK */
  if (remainder == 4 && ntohl(*(uint32_t *)(data + parsed)) == 0) {
    info->auth.mode = NTP_AUTH_SYMMETRIC;
    info->auth.mac.start = parsed;
    info->auth.mac.length = remainder;
    info->auth.mac.key_id = 0;
    return 1;
  }

  /* Parse the rest of the NTPv4 packet */

  while (remainder > 0) {
    /* Check if the remaining data is a MAC */
    if (remainder >= NTP_MIN_MAC_LENGTH && remainder <= NTP_MAX_V4_MAC_LENGTH)
      break;

    /* The NTPv4-specific limit for MAC length enables deterministic parsing of
       packets with extension fields (RFC 7822), but we support longer MACs in
       packets with no extension fields for compatibility with older chrony
       clients.  Check if the longer MAC would authenticate the packet before
       trying to parse the data as an extension field. */
    if (parsed == NTP_HEADER_LENGTH &&
        remainder > NTP_MAX_V4_MAC_LENGTH && remainder <= NTP_MAX_MAC_LENGTH &&
        KEY_CheckAuth(ntohl(*(uint32_t *)(data + parsed)), data, parsed,
                      (void *)(data + parsed + 4), remainder - 4, NTP_MAX_MAC_LENGTH - 4))
      break;

    /* Check if this is a valid NTPv4 extension field and skip it */
    if (!NEF_ParseField(packet, info->length, parsed, &ef_length, &ef_type, NULL, NULL)) {
      /* Invalid MAC or format error */
      DEBUG_LOG("Invalid format or MAC");
      return 0;
    }

    assert(ef_length > 0);

    switch (ef_type) {
      case NTP_EF_NTS_UNIQUE_IDENTIFIER:
      case NTP_EF_NTS_COOKIE:
      case NTP_EF_NTS_COOKIE_PLACEHOLDER:
      case NTP_EF_NTS_AUTH_AND_EEF:
        info->auth.mode = NTP_AUTH_NTS;
        break;
      default:
        DEBUG_LOG("Unknown extension field type=%x", (unsigned int)ef_type);
    }

    info->ext_fields++;
    parsed += ef_length;
    remainder = info->length - parsed;
  }

  if (remainder == 0) {
    /* No MAC */
    return 1;
  } else if (remainder >= NTP_MIN_MAC_LENGTH) {
    /* This is not 100% reliable as a MAC could fail to authenticate and could
       pass as an extension field, leaving reminder smaller than the minimum MAC
       length */
    info->auth.mode = NTP_AUTH_SYMMETRIC;
    info->auth.mac.start = parsed;
    info->auth.mac.length = remainder;
    info->auth.mac.key_id = ntohl(*(uint32_t *)(data + parsed));
    return 1;
  }

  DEBUG_LOG("Invalid format");
  return 0;
}

/* ================================================== */

int
NAU_CheckRequestAuth(NTP_Packet *request, NTP_PacketInfo *info, uint32_t *kod)
{
  *kod = 0;

  switch (info->auth.mode) {
    case NTP_AUTH_NONE:
      break;
    case NTP_AUTH_SYMMETRIC:
      if (!check_symmetric_auth(request, info))
        return 0;
      break;
    case NTP_AUTH_MSSNTP:
      /* MS-SNTP requests are not authenticated */
      break;
    case NTP_AUTH_NTS:
      if (!NNS_CheckRequestAuth(request, info, kod))
        return 0;
      break;
    default:
      return 0;
  }

  return 1;
}

/* ================================================== */

void
NAU_AdjustResponseTimestamp(NTP_Packet *request, NTP_PacketInfo *info, struct timespec *ts)
{
  adjust_timestamp(info->auth.mode, info->auth.mac.key_id, ts);
}

/* ================================================== */

int
NAU_GenerateResponseAuth(NTP_Packet *request, NTP_PacketInfo *request_info,
                         NTP_Packet *response, NTP_PacketInfo *response_info,
                         NTP_Remote_Address *remote_addr, NTP_Local_Address *local_addr,
                         uint32_t kod)
{
  switch (request_info->auth.mode) {
    case NTP_AUTH_NONE:
      break;
    case NTP_AUTH_SYMMETRIC:
      if (!generate_symmetric_auth(request_info->auth.mac.key_id, response, response_info))
        return 0;
      break;
    case NTP_AUTH_MSSNTP:
      /* Sign the packet asynchronously by ntp_signd */
      if (!NSD_SignAndSendPacket(request_info->auth.mac.key_id, response, response_info,
                                 remote_addr, local_addr))
        return 0;
      /* Don't send the original packet */
      return 0;
    case NTP_AUTH_NTS:
      if (!NNS_GenerateResponseAuth(request, request_info, response, response_info, kod))
        return 0;
      break;
    default:
      DEBUG_LOG("Could not authenticate response auth_mode=%d", (int)request_info->auth.mode);
      return 0;
  }

  return 1;
}

/* ================================================== */

int
NAU_CheckResponseAuth(NAU_Instance instance, NTP_Packet *response, NTP_PacketInfo *info)
{
  /* The authentication must match the expected mode */
  if (info->auth.mode != instance->mode)
    return 0;

  switch (info->auth.mode) {
    case NTP_AUTH_NONE:
      break;
    case NTP_AUTH_SYMMETRIC:
      /* Check if it is authenticated with the specified key */
      if (info->auth.mac.key_id != instance->key_id)
        return 0;
      /* and that the MAC is valid */
      if (!check_symmetric_auth(response, info))
        return 0;
      break;
    case NTP_AUTH_NTS:
      if (!NNC_CheckResponseAuth(instance->nts, response, info))
        return 0;
      break;
    default:
      return 0;
  }

  return 1;
}

/* ================================================== */

void
NAU_ChangeAddress(NAU_Instance instance, IPAddr *address)
{
  switch (instance->mode) {
    case NTP_AUTH_NONE:
    case NTP_AUTH_SYMMETRIC:
      break;
    case NTP_AUTH_NTS:
      NNC_ChangeAddress(instance->nts, address);
      break;
    default:
      assert(0);
  }
}
