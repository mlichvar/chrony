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

  NTS-KE server
  */

#include "config.h"

#include "sysincl.h"

#include "nts_ke_server.h"

#include "array.h"
#include "conf.h"
#include "clientlog.h"
#include "logging.h"
#include "memory.h"
#include "ntp_core.h"
#include "nts_ke_session.h"
#include "siv.h"
#include "socket.h"
#include "sched.h"
#include "sys.h"
#include "util.h"

#define SERVER_TIMEOUT 2.0

#define SERVER_COOKIE_SIV AEAD_AES_SIV_CMAC_256
#define SERVER_COOKIE_NONCE_LENGTH 16

#define KEY_ID_INDEX_BITS 2
#define MAX_SERVER_KEYS (1U << KEY_ID_INDEX_BITS)

#define MIN_KEY_ROTATE_INTERVAL 1.0

#define INVALID_SOCK_FD (-7)

typedef struct {
  uint32_t key_id;
  uint8_t nonce[SERVER_COOKIE_NONCE_LENGTH];
} ServerCookieHeader;

typedef struct {
  uint32_t id;
  unsigned char key[SIV_MAX_KEY_LENGTH];
  SIV_Instance siv;
} ServerKey;

typedef struct {
  uint32_t key_id;
  unsigned char key[SIV_MAX_KEY_LENGTH];
  IPAddr client_addr;
  uint16_t client_port;
  uint16_t _pad;
} HelperRequest;

/* ================================================== */

static ServerKey server_keys[MAX_SERVER_KEYS];
static int current_server_key;

static int server_sock_fd4;
static int server_sock_fd6;

static int helper_sock_fd;

static int initialised = 0;

/* Array of NKSN instances */
static ARR_Instance sessions;
static void *server_credentials;

/* ================================================== */

static int handle_message(void *arg);

/* ================================================== */

static int
handle_client(int sock_fd, IPSockAddr *addr)
{
  NKSN_Instance inst, *instp;
  int i;

  if (sock_fd > FD_SETSIZE / 2) {
    DEBUG_LOG("Rejected connection from %s (%s)",
              UTI_IPSockAddrToString(addr), "too many descriptors");
    return 0;
  }

  /* Find a slot which is free or has a stopped session */
  for (i = 0, inst = NULL; i < ARR_GetSize(sessions); i++) {
    instp = ARR_GetElement(sessions, i);
    if (!*instp) {
      /* NULL handler arg will be replaced with the session instance */
      inst = NKSN_CreateInstance(1, NULL, handle_message, NULL);
      *instp = inst;
      break;
    } else if (NKSN_IsStopped(*instp)) {
      inst = *instp;
      break;
    }
  }

  if (!inst) {
    DEBUG_LOG("Rejected connection from %s (%s)",
              UTI_IPSockAddrToString(addr), "too many connections");
    return 0;
  }

  if (!NKSN_StartSession(inst, sock_fd, UTI_IPSockAddrToString(addr),
                         server_credentials, SERVER_TIMEOUT))
    return 0;

  return 1;
}

/* ================================================== */

static void
handle_helper_request(int fd, int event, void *arg)
{
  SCK_Message *message;
  HelperRequest *req;
  IPSockAddr client_addr;
  int sock_fd;

  message = SCK_ReceiveMessage(fd, SCK_FLAG_MSG_DESCRIPTOR);
  if (!message)
    return;

  sock_fd = message->descriptor;
  if (sock_fd < 0) {
    /* Message with no descriptor is a shutdown command */
    SCH_QuitProgram();
    return;
  }

  if (message->length != sizeof (HelperRequest)) {
    DEBUG_LOG("Unexpected message length");
    SCK_CloseSocket(sock_fd);
    return;
  }

  req = message->data;

  /* Extract the server key and client address from the request */
  server_keys[current_server_key].id = ntohl(req->key_id);
  memcpy(server_keys[current_server_key].key, req->key,
         sizeof (server_keys[current_server_key].key));
  UTI_IPNetworkToHost(&req->client_addr, &client_addr.ip_addr);
  client_addr.port = ntohs(req->client_port);

  if (!SIV_SetKey(server_keys[current_server_key].siv, server_keys[current_server_key].key,
                  SIV_GetKeyLength(SERVER_COOKIE_SIV)))
    assert(0);

  if (!handle_client(sock_fd, &client_addr)) {
    SCK_CloseSocket(sock_fd);
    return;
  }

  DEBUG_LOG("Accepted helper request fd=%d", sock_fd);
}

/* ================================================== */

static void
accept_connection(int server_fd, int event, void *arg)
{
  SCK_Message message;
  IPSockAddr addr;
  int log_index, sock_fd;
  struct timespec now;

  sock_fd = SCK_AcceptConnection(server_fd, &addr);
  if (sock_fd < 0)
    return;

  if (!NCR_CheckAccessRestriction(&addr.ip_addr)) {
    DEBUG_LOG("Rejected connection from %s (%s)",
              UTI_IPSockAddrToString(&addr), "access denied");
    SCK_CloseSocket(sock_fd);
    return;
  }

  SCH_GetLastEventTime(&now, NULL, NULL);
  log_index = CLG_LogNTPAccess(&addr.ip_addr, &now);
  if (log_index >= 0 && CLG_LimitNTPResponseRate(log_index)) {
    DEBUG_LOG("Rejected connection from %s (%s)",
              UTI_IPSockAddrToString(&addr), "rate limit");
    SCK_CloseSocket(sock_fd);
    return;
  }

  /* Pass the socket to a helper process if enabled.  Otherwise, handle the
     client in the main process. */
  if (helper_sock_fd != INVALID_SOCK_FD) {
    HelperRequest req;

    /* Include the current server key and client address in the request */
    memset(&req, 0, sizeof (req));
    req.key_id = htonl(server_keys[current_server_key].id);
    memcpy(req.key, server_keys[current_server_key].key, sizeof (req.key));
    UTI_IPHostToNetwork(&addr.ip_addr, &req.client_addr);
    req.client_port = htons(addr.port);

    SCK_InitMessage(&message, SCK_ADDR_UNSPEC);
    message.data = &req;
    message.length = sizeof (req);
    message.descriptor = sock_fd;

    if (!SCK_SendMessage(helper_sock_fd, &message, SCK_FLAG_MSG_DESCRIPTOR)) {
      SCK_CloseSocket(sock_fd);
      return;
    }

    SCK_CloseSocket(sock_fd);
  } else {
    if (!handle_client(sock_fd, &addr)) {
      SCK_CloseSocket(sock_fd);
      return;
    }
  }

  DEBUG_LOG("Accepted connection from %s fd=%d", UTI_IPSockAddrToString(&addr), sock_fd);
}

/* ================================================== */

static int
open_socket(int family, int port)
{
  IPSockAddr local_addr;
  int sock_fd;

  if (!SCK_IsFamilySupported(family))
    return INVALID_SOCK_FD;

  CNF_GetBindAddress(family, &local_addr.ip_addr);

  if (local_addr.ip_addr.family != family)
    SCK_GetAnyLocalIPAddress(family, &local_addr.ip_addr);

  local_addr.port = port;

  sock_fd = SCK_OpenTcpSocket(NULL, &local_addr, 0);
  if (sock_fd < 0) {
    LOG(LOGS_ERR, "Could not open NTS-KE socket on %s", UTI_IPSockAddrToString(&local_addr));
    return INVALID_SOCK_FD;
  }

  if (!SCK_ListenOnSocket(sock_fd, CNF_GetNtsServerConnections())) {
    SCK_CloseSocket(sock_fd);
    return INVALID_SOCK_FD;
  }

  SCH_AddFileHandler(sock_fd, SCH_FILE_INPUT, accept_connection, NULL);

  return sock_fd;
}

/* ================================================== */

static void
helper_signal(int x)
{
  SCH_QuitProgram();
}

/* ================================================== */

static int
prepare_response(NKSN_Instance session, int error, int next_protocol, int aead_algorithm)
{
  NKE_Context context;
  NKE_Cookie cookie;
  uint16_t datum;
  int i;

  DEBUG_LOG("NTS KE response: error=%d next=%d aead=%d", error, next_protocol, aead_algorithm);

  NKSN_BeginMessage(session);

  if (error >= 0) {
    datum = htons(error);
    if (!NKSN_AddRecord(session, 1, NKE_RECORD_ERROR, &datum, sizeof (datum)))
      return 0;
  } else {
    datum = htons(next_protocol);
    if (!NKSN_AddRecord(session, 1, NKE_RECORD_NEXT_PROTOCOL, &datum, sizeof (datum)))
      return 0;

    datum = htons(aead_algorithm);
    if (!NKSN_AddRecord(session, 1, NKE_RECORD_AEAD_ALGORITHM, &datum, sizeof (datum)))
      return 0;

    if (CNF_GetNTPPort() != NTP_PORT) {
      datum = htons(CNF_GetNTPPort());
      if (!NKSN_AddRecord(session, 1, NKE_RECORD_NTPV4_PORT_NEGOTIATION, &datum, sizeof (datum)))
        return 0;
    }

    /* This should be configurable */
    if (0) {
      const char server[] = "::1";
      if (!NKSN_AddRecord(session, 1, NKE_RECORD_NTPV4_SERVER_NEGOTIATION, server,
                          sizeof (server) - 1))
        return 0;
    }

    context.algorithm = aead_algorithm;

    if (!NKSN_GetKeys(session, aead_algorithm, &context.c2s, &context.s2c))
      return 0;

    for (i = 0; i < NKE_MAX_COOKIES; i++) {
      if (!NKS_GenerateCookie(&context, &cookie))
        return 0;
      if (!NKSN_AddRecord(session, 0, NKE_RECORD_COOKIE, cookie.cookie, cookie.length))
        return 0;
    }
  }

  if (!NKSN_EndMessage(session))
    return 0;

  return 1;
}

/* ================================================== */

static int
process_request(NKSN_Instance session)
{
  int next_protocol = -1, aead_algorithm = -1, error = -1;
  int i, critical, type, length;
  uint16_t data[NKE_MAX_RECORD_BODY_LENGTH / sizeof (uint16_t)];

  assert(NKE_MAX_RECORD_BODY_LENGTH % sizeof (uint16_t) == 0);
  assert(sizeof (uint16_t) == 2);

  while (error == -1) {
    if (!NKSN_GetRecord(session, &critical, &type, &length, &data, sizeof (data)))
      break;

    switch (type) {
      case NKE_RECORD_NEXT_PROTOCOL:
        if (!critical || length < 2 || length % 2 != 0) {
          error = NKE_ERROR_BAD_REQUEST;
          break;
        }
        for (i = 0; i < MIN(length, sizeof (data)) / 2; i++) {
          if (ntohs(data[i]) == NKE_NEXT_PROTOCOL_NTPV4)
            next_protocol = NKE_NEXT_PROTOCOL_NTPV4;
        }
        break;
      case NKE_RECORD_AEAD_ALGORITHM:
        if (length < 2 || length % 2 != 0) {
          error = NKE_ERROR_BAD_REQUEST;
          break;
        }
        for (i = 0; i < MIN(length, sizeof (data)) / 2; i++) {
          if (ntohs(data[i]) == AEAD_AES_SIV_CMAC_256)
            aead_algorithm = AEAD_AES_SIV_CMAC_256;
        }
        break;
      case NKE_RECORD_ERROR:
      case NKE_RECORD_WARNING:
      case NKE_RECORD_COOKIE:
        error = NKE_ERROR_BAD_REQUEST;
        break;
      default:
        if (critical)
          error = NKE_ERROR_UNRECOGNIZED_CRITICAL_RECORD;
    }
  }

  if (aead_algorithm < 0 || next_protocol < 0)
    error = NKE_ERROR_BAD_REQUEST;

  if (!prepare_response(session, error, next_protocol, aead_algorithm))
    return 0;

  return 1;
}

/* ================================================== */

static int
handle_message(void *arg)
{
  NKSN_Instance session = arg;

  return process_request(session);
}

/* ================================================== */

static void
generate_key(int index)
{
  int key_length;

  assert(index < MAX_SERVER_KEYS);

  key_length = SIV_GetKeyLength(SERVER_COOKIE_SIV);
  if (key_length > sizeof (server_keys[index].key))
    assert(0);

  UTI_GetRandomBytesUrandom(server_keys[index].key, key_length);
  if (!SIV_SetKey(server_keys[index].siv, server_keys[index].key, key_length))
    assert(0);

  UTI_GetRandomBytes(&server_keys[index].id, sizeof (server_keys[index].id));

  server_keys[index].id &= -1U << KEY_ID_INDEX_BITS;
  server_keys[index].id |= index;

  DEBUG_LOG("Generated server key %"PRIX32, server_keys[index].id);
}

/* ================================================== */

static void
save_keys(void)
{
  char hex_key[SIV_MAX_KEY_LENGTH * 2 + 1];
  int i, index, key_length;
  char *cachedir;
  FILE *f;

  cachedir = CNF_GetNtsCacheDir();
  if (!cachedir)
    return;

  f = UTI_OpenFile(cachedir, "ntskeys", ".tmp", 'w', 0600);
  if (!f)
    return;

  key_length = SIV_GetKeyLength(SERVER_COOKIE_SIV);

  for (i = 0; i < MAX_SERVER_KEYS; i++) {
    index = (current_server_key + i + 1) % MAX_SERVER_KEYS;

    if (key_length > sizeof (server_keys[index].key) ||
        !UTI_BytesToHex(server_keys[index].key, key_length, hex_key, sizeof (hex_key))) {
      assert(0);
      break;
    }

    fprintf(f, "%08"PRIX32" %s\n", server_keys[index].id, hex_key);
  }

  fclose(f);

  if (!UTI_RenameTempFile(cachedir, "ntskeys", ".tmp", NULL))
    ;
}

/* ================================================== */

static void
load_keys(void)
{
  int i, index, line_length, key_length, n;
  char *cachedir, line[1024];
  FILE *f;
  uint32_t id;

  cachedir = CNF_GetNtsCacheDir();
  if (!cachedir)
    return;

  f = UTI_OpenFile(cachedir, "ntskeys", NULL, 'r', 0);
  if (!f)
    return;

  key_length = SIV_GetKeyLength(SERVER_COOKIE_SIV);

  for (i = 0; i < MAX_SERVER_KEYS; i++) {
    if (!fgets(line, sizeof (line), f))
      break;

    line_length = strlen(line);
    if (line_length < 10)
      break;
    /* Drop '\n' */
    line[line_length - 1] = '\0';

    if (sscanf(line, "%"PRIX32"%n", &id, &n) != 1 || line[n] != ' ')
      break;

    index = id % MAX_SERVER_KEYS;

    if (UTI_HexToBytes(line + n + 1, server_keys[index].key,
                       sizeof (server_keys[index].key)) != key_length)
      break;

    server_keys[index].id = id;
    if (!SIV_SetKey(server_keys[index].siv, server_keys[index].key, key_length))
      assert(0);

    DEBUG_LOG("Loaded key %"PRIX32, id);

    current_server_key = index;
  }

  fclose(f);
}

/* ================================================== */

static void
key_timeout(void *arg)
{
  current_server_key = (current_server_key + 1) % MAX_SERVER_KEYS;
  generate_key(current_server_key);
  save_keys();

  SCH_AddTimeoutByDelay(MAX(CNF_GetNtsRotate(), MIN_KEY_ROTATE_INTERVAL),
                        key_timeout, NULL);
}

/* ================================================== */

static void
start_helper(int id, int scfilter_level, int main_fd, int helper_fd)
{
  pid_t pid;

  pid = fork();

  if (pid < 0)
    LOG_FATAL("fork() failed : %s", strerror(errno));

  if (pid > 0)
    return;

  SCK_CloseSocket(main_fd);

  LOG_CloseParentFd();
  SCH_Reset();
  SCH_AddFileHandler(helper_fd, SCH_FILE_INPUT, handle_helper_request, NULL);
  UTI_SetQuitSignalsHandler(helper_signal, 1);
  if (scfilter_level != 0)
    SYS_EnableSystemCallFilter(scfilter_level, SYS_NTSKE_HELPER);

  initialised = 1;

  DEBUG_LOG("NTS-KE helper #%d started", id);

  SCH_MainLoop();

  NKS_Finalise();

  DEBUG_LOG("NTS-KE helper #%d exiting", id);

  exit(0);
}

/* ================================================== */

void
NKS_Initialise(int scfilter_level)
{
  char *cert, *key;
  int i, processes;

  server_sock_fd4 = INVALID_SOCK_FD;
  server_sock_fd6 = INVALID_SOCK_FD;
  helper_sock_fd = INVALID_SOCK_FD;

  cert = CNF_GetNtsServerCertFile();
  key = CNF_GetNtsServerKeyFile();

  if (!cert || !key)
    return;

  server_credentials = NKSN_CreateCertCredentials(cert, key, NULL);
  if (!server_credentials)
    return;

  sessions = ARR_CreateInstance(sizeof (NKSN_Instance));
  for (i = 0; i < CNF_GetNtsServerConnections(); i++)
    *(NKSN_Instance *)ARR_GetNewElement(sessions) = NULL;
  for (i = 0; i < MAX_SERVER_KEYS; i++)
    server_keys[i].siv = NULL;

  server_sock_fd4 = open_socket(IPADDR_INET4, CNF_GetNtsServerPort());
  server_sock_fd6 = open_socket(IPADDR_INET6, CNF_GetNtsServerPort());

  for (i = 0; i < MAX_SERVER_KEYS; i++) {
    server_keys[i].siv = SIV_CreateInstance(SERVER_COOKIE_SIV);
    generate_key(i);
  }

  current_server_key = MAX_SERVER_KEYS - 1;

  load_keys();

  key_timeout(NULL);

  processes = CNF_GetNtsServerProcesses();

  if (processes > 0) {
    int sock_fd1, sock_fd2;

    sock_fd1 = SCK_OpenUnixSocketPair(0, &sock_fd2);

    for (i = 0; i < processes; i++)
      start_helper(i + 1, scfilter_level, sock_fd1, sock_fd2);

    SCK_CloseSocket(sock_fd2);
    helper_sock_fd = sock_fd1;
  }

  initialised = 1;
}

/* ================================================== */

void
NKS_Finalise(void)
{
  int i;

  if (!initialised)
    return;

  if (helper_sock_fd != INVALID_SOCK_FD) {
    for (i = 0; i < CNF_GetNtsServerProcesses(); i++) {
      if (!SCK_Send(helper_sock_fd, "", 1, 0))
        ;
    }
    SCK_CloseSocket(helper_sock_fd);
  }
  if (server_sock_fd4 != INVALID_SOCK_FD)
    SCK_CloseSocket(server_sock_fd4);
  if (server_sock_fd6 != INVALID_SOCK_FD)
    SCK_CloseSocket(server_sock_fd6);

  save_keys();
  for (i = 0; i < MAX_SERVER_KEYS; i++) {
    if (server_keys[i].siv != NULL)
      SIV_DestroyInstance(server_keys[i].siv);
  }

  for (i = 0; i < ARR_GetSize(sessions); i++) {
    NKSN_Instance session = *(NKSN_Instance *)ARR_GetElement(sessions, i);
    if (session)
      NKSN_DestroyInstance(session);
  }
  ARR_DestroyInstance(sessions);

  NKSN_DestroyCertCredentials(server_credentials);
}

/* ================================================== */

/* A server cookie consists of key ID, nonce, and encrypted C2S+S2C keys */

int
NKS_GenerateCookie(NKE_Context *context, NKE_Cookie *cookie)
{
  unsigned char plaintext[2 * NKE_MAX_KEY_LENGTH], *ciphertext;
  int plaintext_length, tag_length;
  ServerCookieHeader *header;
  ServerKey *key;

  if (!initialised) {
    DEBUG_LOG("NTS server disabled");
    return 0;
  }

  /* The algorithm is hardcoded for now */
  if (context->algorithm != AEAD_AES_SIV_CMAC_256) {
    DEBUG_LOG("Unexpected SIV algorithm");
    return 0;
  }

  if (context->c2s.length < 0 || context->c2s.length > NKE_MAX_KEY_LENGTH ||
      context->s2c.length < 0 || context->s2c.length > NKE_MAX_KEY_LENGTH) {
    DEBUG_LOG("Invalid key length");
    return 0;
  }

  key = &server_keys[current_server_key];

  header = (ServerCookieHeader *)cookie->cookie;

  /* Keep the fields in the host byte order */
  header->key_id = key->id;
  UTI_GetRandomBytes(header->nonce, sizeof (header->nonce));

  plaintext_length = context->c2s.length + context->s2c.length;
  assert(plaintext_length <= sizeof (plaintext));
  memcpy(plaintext, context->c2s.key, context->c2s.length);
  memcpy(plaintext + context->c2s.length, context->s2c.key, context->s2c.length);

  tag_length = SIV_GetTagLength(key->siv);
  cookie->length = sizeof (*header) + plaintext_length + tag_length;
  assert(cookie->length <= sizeof (cookie->cookie));
  ciphertext = cookie->cookie + sizeof (*header);

  if (!SIV_Encrypt(key->siv, header->nonce, sizeof (header->nonce),
                   "", 0,
                   plaintext, plaintext_length,
                   ciphertext, plaintext_length + tag_length)) {
    DEBUG_LOG("Could not encrypt cookie");
    return 0;
  }

  return 1;
}

/* ================================================== */

int
NKS_DecodeCookie(NKE_Cookie *cookie, NKE_Context *context)
{
  unsigned char plaintext[2 * NKE_MAX_KEY_LENGTH], *ciphertext;
  int ciphertext_length, plaintext_length, tag_length;
  ServerCookieHeader *header;
  ServerKey *key;

  if (!initialised) {
    DEBUG_LOG("NTS server disabled");
    return 0;
  }

  if (cookie->length <= sizeof (*header)) {
    DEBUG_LOG("Invalid cookie length");
    return 0;
  }

  header = (ServerCookieHeader *)cookie->cookie;
  ciphertext = cookie->cookie + sizeof (*header);
  ciphertext_length = cookie->length - sizeof (*header);

  key = &server_keys[header->key_id % MAX_SERVER_KEYS];
  if (header->key_id != key->id) {
    DEBUG_LOG("Unknown key %"PRIX32, header->key_id);
    return 0;
  }

  tag_length = SIV_GetTagLength(key->siv);
  if (tag_length >= ciphertext_length) {
    DEBUG_LOG("Invalid cookie length");
    return 0;
  }

  plaintext_length = ciphertext_length - tag_length;
  if (plaintext_length > sizeof (plaintext) || plaintext_length % 2 != 0) {
    DEBUG_LOG("Invalid cookie length");
    return 0;
  }

  if (!SIV_Decrypt(key->siv, header->nonce, sizeof (header->nonce),
                   "", 0,
                   ciphertext, ciphertext_length,
                   plaintext, plaintext_length)) {
    DEBUG_LOG("Could not decrypt cookie");
    return 0;
  }

  context->algorithm = AEAD_AES_SIV_CMAC_256;

  context->c2s.length = plaintext_length / 2;
  context->s2c.length = plaintext_length / 2;
  assert(context->c2s.length <= sizeof (context->c2s.key));

  memcpy(context->c2s.key, plaintext, context->c2s.length);
  memcpy(context->s2c.key, plaintext + context->c2s.length, context->s2c.length);

  return 1;
}
