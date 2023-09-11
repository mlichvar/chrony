/*
  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Miroslav Lichvar  2014-2016
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

  Function replacements needed when optional features are disabled.

  */

#include "config.h"

#include "clientlog.h"
#include "cmac.h"
#include "cmdmon.h"
#include "keys.h"
#include "logging.h"
#include "manual.h"
#include "memory.h"
#include "nameserv.h"
#include "nameserv_async.h"
#include "ntp_core.h"
#include "ntp_io.h"
#include "ntp_sources.h"
#include "ntp_signd.h"
#include "nts_ke_client.h"
#include "nts_ke_server.h"
#include "nts_ntp_client.h"
#include "nts_ntp_server.h"
#include "privops.h"
#include "refclock.h"
#include "sched.h"
#include "util.h"

#if defined(FEAT_NTP) && !defined(FEAT_ASYNCDNS)

/* This is a blocking implementation used when asynchronous resolving is not available */

struct DNS_Async_Instance {
  const char *name;
  DNS_NameResolveHandler handler;
  void *arg;
  int pipe[2];
};

static void
resolve_name(int fd, int event, void *anything)
{
  struct DNS_Async_Instance *inst;
  IPAddr addrs[DNS_MAX_ADDRESSES];
  DNS_Status status;
  int i;

  inst = (struct DNS_Async_Instance *)anything;

  SCH_RemoveFileHandler(inst->pipe[0]);
  close(inst->pipe[0]);
  close(inst->pipe[1]);

  status = PRV_Name2IPAddress(inst->name, addrs, DNS_MAX_ADDRESSES);

  for (i = 0; status == DNS_Success && i < DNS_MAX_ADDRESSES &&
       addrs[i].family != IPADDR_UNSPEC; i++)
    ;

  (inst->handler)(status, i, addrs, inst->arg);

  Free(inst);
}

void
DNS_Name2IPAddressAsync(const char *name, DNS_NameResolveHandler handler, void *anything)
{
  struct DNS_Async_Instance *inst;

  inst = MallocNew(struct DNS_Async_Instance);
  inst->name = name;
  inst->handler = handler;
  inst->arg = anything;

  if (pipe(inst->pipe))
    LOG_FATAL("pipe() failed");

  UTI_FdSetCloexec(inst->pipe[0]);
  UTI_FdSetCloexec(inst->pipe[1]);

  SCH_AddFileHandler(inst->pipe[0], SCH_FILE_INPUT, resolve_name, inst);

  if (write(inst->pipe[1], "", 1) < 0)
    ;
}

#endif /* !FEAT_ASYNCDNS */

#ifndef FEAT_CMDMON

void
CAM_Initialise(void)
{
}

void
CAM_Finalise(void)
{
}

void
CAM_OpenUnixSocket(void)
{
}

int
CAM_AddAccessRestriction(IPAddr *ip_addr, int subnet_bits, int allow, int all)
{
  return 1;
}

void
MNL_Initialise(void)
{
}

void
MNL_Finalise(void)
{
}

#endif /* !FEAT_CMDMON */

#ifndef FEAT_NTP

void
NCR_AddBroadcastDestination(NTP_Remote_Address *addr, int interval)
{
}

void
NCR_Initialise(void)
{
}

void
NCR_Finalise(void)
{
}

int
NCR_AddAccessRestriction(IPAddr *ip_addr, int subnet_bits, int allow, int all)
{
  return 1;
}

int
NCR_CheckAccessRestriction(IPAddr *ip_addr)
{
  return 0;
}

void
NIO_Initialise(void)
{
}

void
NIO_Finalise(void)
{
}

void
NSR_Initialise(void)
{
}

void
NSR_Finalise(void)
{
}

NSR_Status
NSR_AddSource(NTP_Remote_Address *remote_addr, NTP_Source_Type type,
              SourceParameters *params, uint32_t *conf_id)
{
  return NSR_TooManySources;
}

NSR_Status
NSR_AddSourceByName(char *name, int port, int pool, NTP_Source_Type type,
                    SourceParameters *params, uint32_t *conf_id)
{
  return NSR_TooManySources;
}

const char *
NSR_StatusToString(NSR_Status status)
{
  return "NTP not supported";
}

NSR_Status
NSR_RemoveSource(IPAddr *address)
{
  return NSR_NoSuchSource;
}

void
NSR_RemoveSourcesById(uint32_t conf_id)
{
}

void
NSR_RemoveAllSources(void)
{
}

void
NSR_HandleBadSource(IPAddr *address)
{
}

void
NSR_RefreshAddresses(void)
{
}

char *
NSR_GetName(IPAddr *address)
{
  return NULL;
}

void
NSR_SetSourceResolvingEndHandler(NSR_SourceResolvingEndHandler handler)
{
  if (handler)
    (handler)();
}

void
NSR_ResolveSources(void)
{
}

void NSR_StartSources(void)
{
}

void NSR_AutoStartSources(void)
{
}

int
NSR_InitiateSampleBurst(int n_good_samples, int n_total_samples,
                        IPAddr *mask, IPAddr *address)
{
  return 0;
}

uint32_t
NSR_GetLocalRefid(IPAddr *address)
{
  return 0;
}

int
NSR_SetConnectivity(IPAddr *mask, IPAddr *address, SRC_Connectivity connectivity)
{
  return 0;
}

int
NSR_ModifyMinpoll(IPAddr *address, int new_minpoll)
{
  return 0;
}

int
NSR_ModifyMaxpoll(IPAddr *address, int new_maxpoll)
{
  return 0;
}

int
NSR_ModifyMaxdelay(IPAddr *address, double new_max_delay)
{
  return 0;
}

int
NSR_ModifyMaxdelayratio(IPAddr *address, double new_max_delay_ratio)
{
  return 0;
}

int
NSR_ModifyMaxdelaydevratio(IPAddr *address, double new_max_delay_dev_ratio)
{
  return 0;
}

int
NSR_ModifyMinstratum(IPAddr *address, int new_min_stratum)
{
  return 0;
}

int
NSR_ModifyPolltarget(IPAddr *address, int new_poll_target)
{
  return 0;
}

void
NSR_ReportSource(RPT_SourceReport *report, struct timespec *now)
{
  memset(report, 0, sizeof (*report));
}
  
int
NSR_GetAuthReport(IPAddr *address, RPT_AuthReport *report)
{
  return 0;
}

int
NSR_GetNTPReport(RPT_NTPReport *report)
{
  return 0;
}

void
NSR_GetActivityReport(RPT_ActivityReport *report)
{
  memset(report, 0, sizeof (*report));
}

void
NSR_DumpAuthData(void)
{
}

#ifndef FEAT_CMDMON

void
CLG_Initialise(void)
{
}

void
CLG_Finalise(void)
{
}

void
DNS_SetAddressFamily(int family)
{
}

DNS_Status
DNS_Name2IPAddress(const char *name, IPAddr *ip_addrs, int max_addrs)
{
  return DNS_Failure;
}

void
KEY_Initialise(void)
{
}

void
KEY_Finalise(void)
{
}

#endif /* !FEAT_CMDMON */
#endif /* !FEAT_NTP */

#ifndef FEAT_REFCLOCK
void
RCL_Initialise(void)
{
}

void
RCL_Finalise(void)
{
}

int
RCL_AddRefclock(RefclockParameters *params)
{
  return 0;
}

void
RCL_StartRefclocks(void)
{
}

void
RCL_ReportSource(RPT_SourceReport *report, struct timespec *now)
{
  memset(report, 0, sizeof (*report));
}

#endif /* !FEAT_REFCLOCK */

#ifndef FEAT_SIGND

void
NSD_Initialise(void)
{
}

void
NSD_Finalise(void)
{
}

int
NSD_SignAndSendPacket(uint32_t key_id, NTP_Packet *packet, NTP_PacketInfo *info,
                      NTP_Remote_Address *remote_addr, NTP_Local_Address *local_addr)
{
  return 0;
}

#endif /* !FEAT_SIGND */

#ifndef HAVE_CMAC

int
CMC_GetKeyLength(CMC_Algorithm algorithm)
{
  return 0;
}

CMC_Instance
CMC_CreateInstance(CMC_Algorithm algorithm, const unsigned char *key, int length)
{
  return NULL;
}

int
CMC_Hash(CMC_Instance inst, const void *in, int in_len, unsigned char *out, int out_len)
{
  return 0;
}

void
CMC_DestroyInstance(CMC_Instance inst)
{
}

#endif /* !HAVE_CMAC */

#ifndef FEAT_NTS

void
NNS_Initialise(void)
{
}

void
NNS_Finalise(void)
{
}

int
NNS_CheckRequestAuth(NTP_Packet *packet, NTP_PacketInfo *info, uint32_t *kod)
{
  *kod = 0;
  return 0;
}

int
NNS_GenerateResponseAuth(NTP_Packet *request, NTP_PacketInfo *req_info,
                         NTP_Packet *response, NTP_PacketInfo *res_info,
                         uint32_t kod)
{
  return 0;
}

NNC_Instance
NNC_CreateInstance(IPSockAddr *nts_address, const char *name, uint32_t cert_set,
                   uint16_t ntp_port)
{
  return NULL;
}

void
NNC_DestroyInstance(NNC_Instance inst)
{
}

int
NNC_PrepareForAuth(NNC_Instance inst)
{
  return 1;
}

int
NNC_GenerateRequestAuth(NNC_Instance inst, NTP_Packet *packet, NTP_PacketInfo *info)
{
  static int logged = 0;

  LOG(logged ? LOGS_DEBUG : LOGS_WARN, "Missing NTS support");
  logged = 1;
  return 0;
}

int
NNC_CheckResponseAuth(NNC_Instance inst, NTP_Packet *packet, NTP_PacketInfo *info)
{
  return 0;
}

void
NNC_ChangeAddress(NNC_Instance inst, IPAddr *address)
{
}

void
NNC_DumpData(NNC_Instance inst)
{
}

void
NNC_GetReport(NNC_Instance inst, RPT_AuthReport *report)
{
}

void
NKS_PreInitialise(uid_t uid, gid_t gid, int scfilter_level)
{
}

void
NKS_Initialise(void)
{
}

void
NKS_Finalise(void)
{
}

void
NKS_DumpKeys(void)
{
}

void
NKS_ReloadKeys(void)
{
}

#endif /* !FEAT_NTS */
