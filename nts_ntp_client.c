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

  Client NTS-NTP authentication
  */

#include "config.h"

#include "sysincl.h"

#include "nts_ntp_client.h"

#include "conf.h"
#include "logging.h"
#include "memory.h"
#include "ntp.h"
#include "ntp_ext.h"
#include "ntp_sources.h"
#include "nts_ke_client.h"
#include "nts_ntp.h"
#include "nts_ntp_auth.h"
#include "sched.h"
#include "siv.h"
#include "util.h"

#define MAX_TOTAL_COOKIE_LENGTH (8 * 108)
#define MIN_NKE_RETRY_INTERVAL 1000

struct NNC_Instance_Record {
  const IPSockAddr *ntp_address;
  IPSockAddr nts_address;
  char *name;
  SIV_Instance siv_c2s;
  SIV_Instance siv_s2c;
  NKC_Instance nke;

  int nke_attempts;
  double next_nke_attempt;
  double last_nke_success;
  NKE_Cookie cookies[NTS_MAX_COOKIES];
  int num_cookies;
  int cookie_index;
  int nak_response;
  int ok_response;
  unsigned char nonce[NTS_MIN_UNPADDED_NONCE_LENGTH];
  unsigned char uniq_id[NTS_MIN_UNIQ_ID_LENGTH];
};

/* ================================================== */

static void
reset_instance(NNC_Instance inst)
{
  inst->nke_attempts = 0;
  inst->next_nke_attempt = 0.0;
  inst->last_nke_success = 0.0;
  inst->num_cookies = 0;
  inst->cookie_index = 0;
  inst->nak_response = 0;
  inst->ok_response = 1;
  memset(inst->nonce, 0, sizeof (inst->nonce));
  memset(inst->uniq_id, 0, sizeof (inst->uniq_id));
}

/* ================================================== */

NNC_Instance
NNC_CreateInstance(IPSockAddr *nts_address, const char *name, const IPSockAddr *ntp_address)
{
  NNC_Instance inst;

  inst = MallocNew(struct NNC_Instance_Record);

  inst->ntp_address = ntp_address;
  inst->nts_address = *nts_address;
  inst->name = name ? Strdup(name) : NULL;
  inst->siv_c2s = NULL;
  inst->siv_s2c = NULL;
  inst->nke = NULL;

  reset_instance(inst);

  return inst;
}

/* ================================================== */

void
NNC_DestroyInstance(NNC_Instance inst)
{
  if (inst->nke)
    NKC_DestroyInstance(inst->nke);
  if (inst->siv_c2s)
    SIV_DestroyInstance(inst->siv_c2s);
  if (inst->siv_s2c)
    SIV_DestroyInstance(inst->siv_s2c);

  Free(inst->name);
  Free(inst);
}

/* ================================================== */

static int
is_nke_needed(NNC_Instance inst)
{
  /* Force NKE if a NAK was received since last valid auth */
  if (inst->nak_response && !inst->ok_response && inst->num_cookies > 0) {
    inst->num_cookies = 0;
    DEBUG_LOG("Dropped cookies");
  }

  /* Force NKE if the keys encrypting the cookies are too old */
  if (inst->num_cookies > 0 &&
      SCH_GetLastEventMonoTime() - inst->last_nke_success > CNF_GetNtsRefresh())
    inst->num_cookies = 0;

  return inst->num_cookies == 0;
}

/* ================================================== */

static int
set_ntp_address(NNC_Instance inst, NTP_Remote_Address *negotiated_address)
{
  NTP_Remote_Address old_address, new_address;

  old_address = *inst->ntp_address;
  new_address = *negotiated_address;

  if (new_address.ip_addr.family == IPADDR_UNSPEC)
    new_address.ip_addr = old_address.ip_addr;
  if (new_address.port == 0)
    new_address.port = old_address.port;

  if (UTI_CompareIPs(&old_address.ip_addr, &new_address.ip_addr, NULL) == 0 &&
      old_address.port == new_address.port)
    /* Nothing to do */
    return 1;

  if (NSR_UpdateSourceNtpAddress(&old_address, &new_address) != NSR_Success) {
    LOG(LOGS_ERR, "Could not change %s to negotiated address %s",
        UTI_IPToString(&old_address.ip_addr), UTI_IPToString(&new_address.ip_addr));
    return 0;
  }

  return 1;
}

/* ================================================== */

static void
update_next_nke_attempt(NNC_Instance inst, double now)
{
  int factor, interval;

  if (!inst->nke)
    return;

  factor = NKC_GetRetryFactor(inst->nke);
  interval = MIN(factor + inst->nke_attempts - 1, NKE_MAX_RETRY_INTERVAL2);
  inst->next_nke_attempt = now + UTI_Log2ToDouble(interval);
}

/* ================================================== */

static int
get_nke_data(NNC_Instance inst)
{
  NTP_Remote_Address ntp_address;
  SIV_Algorithm siv;
  NKE_Key c2s, s2c;
  double now;
  int got_data;

  assert(is_nke_needed(inst));

  now = SCH_GetLastEventMonoTime();

  if (!inst->nke) {
    if (now < inst->next_nke_attempt) {
      DEBUG_LOG("Limiting NTS-KE request rate (%f seconds)",
                inst->next_nke_attempt - now);
      return 0;
    }

    if (!inst->name) {
      LOG(LOGS_ERR, "Missing name of %s for NTS-KE",
          UTI_IPToString(&inst->nts_address.ip_addr));
      return 0;
    }

    inst->nke = NKC_CreateInstance(&inst->nts_address, inst->name);

    inst->nke_attempts++;
    update_next_nke_attempt(inst, now);

    if (!NKC_Start(inst->nke))
      return 0;
  }

  update_next_nke_attempt(inst, now);

  if (NKC_IsActive(inst->nke))
    return 0;

  got_data = NKC_GetNtsData(inst->nke, &siv, &c2s, &s2c,
                            inst->cookies, &inst->num_cookies, NTS_MAX_COOKIES,
                            &ntp_address);

  NKC_DestroyInstance(inst->nke);
  inst->nke = NULL;
  
  if (!got_data)
    return 0;

  if (!set_ntp_address(inst, &ntp_address)) {
    inst->num_cookies = 0;
    return 0;
  }

  inst->cookie_index = 0;

  if (inst->siv_c2s)
    SIV_DestroyInstance(inst->siv_c2s);
  if (inst->siv_s2c)
    SIV_DestroyInstance(inst->siv_s2c);

  inst->siv_c2s = SIV_CreateInstance(siv);
  inst->siv_s2c = SIV_CreateInstance(siv);

  if (!inst->siv_c2s || !inst->siv_s2c ||
      !SIV_SetKey(inst->siv_c2s, c2s.key, c2s.length) ||
      !SIV_SetKey(inst->siv_s2c, s2c.key, s2c.length)) {
    DEBUG_LOG("Could not initialise SIV");
    inst->num_cookies = 0;
    return 0;
  }

  inst->nak_response = 0;

  inst->last_nke_success = now;

  return 1;
}

/* ================================================== */

int
NNC_PrepareForAuth(NNC_Instance inst)
{
  if (is_nke_needed(inst)) {
    if (!get_nke_data(inst))
      return 0;
  }

  UTI_GetRandomBytes(&inst->uniq_id, sizeof (inst->uniq_id));
  UTI_GetRandomBytes(&inst->nonce, sizeof (inst->nonce));

  return 1;
}

/* ================================================== */

int
NNC_GenerateRequestAuth(NNC_Instance inst, NTP_Packet *packet,
                        NTP_PacketInfo *info)
{
  NKE_Cookie *cookie;
  int i, req_cookies;

  if (inst->num_cookies == 0 || !inst->siv_c2s)
    return 0;

  if (info->mode != MODE_CLIENT)
    return 0;

  cookie = &inst->cookies[inst->cookie_index];
  req_cookies = MIN(NTS_MAX_COOKIES - inst->num_cookies + 1,
                    MAX_TOTAL_COOKIE_LENGTH / (cookie->length + 4));

  if (!NEF_AddField(packet, info, NTP_EF_NTS_UNIQUE_IDENTIFIER,
                    &inst->uniq_id, sizeof (inst->uniq_id)))
    return 0;

  if (!NEF_AddField(packet, info, NTP_EF_NTS_COOKIE,
                    cookie->cookie, cookie->length))
    return 0;

  for (i = 0; i < req_cookies - 1; i++) {
    if (!NEF_AddField(packet, info, NTP_EF_NTS_COOKIE_PLACEHOLDER,
                      cookie->cookie, cookie->length))
      return 0;
  }

  if (!NNA_GenerateAuthEF(packet, info, inst->siv_c2s, inst->nonce, sizeof (inst->nonce),
                          (const unsigned char *)"", 0, NTP_MAX_V4_MAC_LENGTH + 4))
    return 0;

  inst->num_cookies--;
  inst->cookie_index = (inst->cookie_index + 1) % NTS_MAX_COOKIES;
  inst->ok_response = 0;

  return 1;
}

/* ================================================== */

static int
extract_cookies(NNC_Instance inst, unsigned char *plaintext, int length)
{
  int ef_type, ef_body_length, ef_length, parsed, index, acceptable, saved;
  void *ef_body;

  acceptable = saved = 0;

  for (parsed = 0; parsed < length; parsed += ef_length) {
    if (!NEF_ParseSingleField(plaintext, length, parsed,
                              &ef_length, &ef_type, &ef_body, &ef_body_length))
      break;

    if (ef_type != NTP_EF_NTS_COOKIE)
      continue;

    if (ef_length < NTP_MIN_EF_LENGTH || ef_body_length > sizeof (inst->cookies[0].cookie)) {
      DEBUG_LOG("Unexpected cookie length %d", ef_body_length);
      continue;
    }

    acceptable++;

    if (inst->num_cookies >= NTS_MAX_COOKIES)
      continue;

    index = (inst->cookie_index + inst->num_cookies) % NTS_MAX_COOKIES;
    memcpy(inst->cookies[index].cookie, ef_body, ef_body_length);
    inst->cookies[index].length = ef_body_length;
    inst->num_cookies++;

    saved++;
  }

  DEBUG_LOG("Extracted %d cookies (saved %d)", acceptable, saved);

  return acceptable > 0;
}

/* ================================================== */

int
NNC_CheckResponseAuth(NNC_Instance inst, NTP_Packet *packet,
                      NTP_PacketInfo *info)
{
  int ef_type, ef_body_length, ef_length, parsed, plaintext_length;
  int has_valid_uniq_id = 0, has_valid_auth = 0;
  unsigned char plaintext[NTP_MAX_EXTENSIONS_LENGTH];
  void *ef_body;

  if (info->ext_fields == 0 || info->mode != MODE_SERVER)
    return 0;

  /* Accept only one response per request */
  if (inst->ok_response)
    return 0;

  if (!inst->siv_s2c)
    return 0;

  for (parsed = NTP_HEADER_LENGTH; parsed < info->length; parsed += ef_length) {
    if (!NEF_ParseField(packet, info->length, parsed,
                        &ef_length, &ef_type, &ef_body, &ef_body_length))
      break;

    switch (ef_type) {
      case NTP_EF_NTS_UNIQUE_IDENTIFIER:
        if (ef_body_length != sizeof (inst->uniq_id) ||
            memcmp(ef_body, inst->uniq_id, sizeof (inst->uniq_id)) != 0) {
          DEBUG_LOG("Invalid uniq id");
          return 0;
        }
        has_valid_uniq_id = 1;
        break;
      case NTP_EF_NTS_COOKIE:
        DEBUG_LOG("Unencrypted cookie");
        break;
      case NTP_EF_NTS_AUTH_AND_EEF:
        if (parsed + ef_length != info->length) {
          DEBUG_LOG("Auth not last EF");
          return 0;
        }

        if (!NNA_DecryptAuthEF(packet, info, inst->siv_s2c, parsed,
                               plaintext, sizeof (plaintext), &plaintext_length))
          return 0;

        has_valid_auth = 1;
        break;
      default:
        break;
    }
  }

  if (!has_valid_uniq_id || !has_valid_auth) {
    if (has_valid_uniq_id && packet->stratum == NTP_INVALID_STRATUM &&
        ntohl(packet->reference_id) == NTP_KOD_NTS_NAK) {
      DEBUG_LOG("NTS NAK");
      inst->nak_response = 1;
      return 0;
    }

    DEBUG_LOG("Missing NTS EF");
    return 0;
  }

  if (!extract_cookies(inst, plaintext, plaintext_length))
    return 0;

  inst->ok_response = 1;

  /* At this point we know the client interoperates with the server.  Allow a
     new NTS-KE session to be started as soon as the cookies run out. */
  inst->nke_attempts = 0;
  inst->next_nke_attempt = 0.0;

  return 1;
}

/* ================================================== */

void
NNC_ChangeAddress(NNC_Instance inst, IPAddr *address)
{
  if (inst->nke)
    NKC_DestroyInstance(inst->nke);

  inst->nke = NULL;
  inst->num_cookies = 0;
  inst->nts_address.ip_addr = *address;

  reset_instance(inst);

  DEBUG_LOG("NTS reset");
}
