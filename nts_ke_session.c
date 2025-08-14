/*
  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Miroslav Lichvar  2020-2021
 * Copyright (C) Anthony Brandon  2025
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

  NTS-KE session used by server and client
  */

#include "config.h"

#include "sysincl.h"

#include "nts_ke_session.h"

#include "conf.h"
#include "local.h"
#include "logging.h"
#include "memory.h"
#include "siv.h"
#include "socket.h"
#include "sched.h"
#include "tls.h"
#include "util.h"

#define INVALID_SOCK_FD (-8)

struct RecordHeader {
  uint16_t type;
  uint16_t body_length;
};

struct Message {
  int length;
  int sent;
  int parsed;
  int complete;
  unsigned char data[NKE_MAX_MESSAGE_LENGTH];
};

typedef enum {
  KE_WAIT_CONNECT,
  KE_HANDSHAKE,
  KE_SEND,
  KE_RECEIVE,
  KE_SHUTDOWN,
  KE_STOPPED,
} KeState;

struct NKSN_Instance_Record {
  int server;
  char *server_name;
  NKSN_MessageHandler handler;
  void *handler_arg;

  KeState state;
  int sock_fd;
  char *label;
  TLS_Instance tls_session;
  SCH_TimeoutID timeout_id;
  int retry_factor;

  struct Message message;
  int new_message;
};

/* ================================================== */

static int credentials_counter = 0;

static int clock_updates = 0;

/* ================================================== */

static void
reset_message(struct Message *message)
{
  message->length = 0;
  message->sent = 0;
  message->parsed = 0;
  message->complete = 0;
}

/* ================================================== */

static int
add_record(struct Message *message, int critical, int type, const void *body, int body_length)
{
  struct RecordHeader header;

  assert(message->length <= sizeof (message->data));

  if (body_length < 0 || body_length > 0xffff || type < 0 || type > 0x7fff ||
      message->length + sizeof (header) + body_length > sizeof (message->data))
    return 0;

  header.type = htons(!!critical * NKE_RECORD_CRITICAL_BIT | type);
  header.body_length = htons(body_length);

  memcpy(&message->data[message->length], &header, sizeof (header));
  message->length += sizeof (header);

  if (body_length > 0) {
    memcpy(&message->data[message->length], body, body_length);
    message->length += body_length;
  }

  return 1;
}

/* ================================================== */

static void
reset_message_parsing(struct Message *message)
{
  message->parsed = 0;
}

/* ================================================== */

static int
get_record(struct Message *message, int *critical, int *type, int *body_length,
           void *body, int buffer_length)
{
  struct RecordHeader header;
  int blen, rlen;

  if (message->length < message->parsed + sizeof (header) ||
      buffer_length < 0)
    return 0;

  memcpy(&header, &message->data[message->parsed], sizeof (header));

  blen = ntohs(header.body_length);
  rlen = sizeof (header) + blen;
  assert(blen >= 0 && rlen > 0);

  if (message->length < message->parsed + rlen)
    return 0;

  if (critical)
    *critical = !!(ntohs(header.type) & NKE_RECORD_CRITICAL_BIT);
  if (type)
    *type = ntohs(header.type) & ~NKE_RECORD_CRITICAL_BIT;
  if (body)
    memcpy(body, &message->data[message->parsed + sizeof (header)], MIN(buffer_length, blen));
  if (body_length)
    *body_length = blen;

  message->parsed += rlen;

  return 1;
}

/* ================================================== */

static int
check_message_format(struct Message *message, int eof)
{
  int critical = 0, type = -1, length = -1, ends = 0;

  reset_message_parsing(message);
  message->complete = 0;

  while (get_record(message, &critical, &type, &length, NULL, 0)) {
    if (type == NKE_RECORD_END_OF_MESSAGE) {
      if (!critical || length != 0 || ends > 0)
        return 0;
      ends++;
    }
  }

  /* If the message cannot be fully parsed, but more data may be coming,
     consider the format to be ok */
  if (message->length == 0 || message->parsed < message->length)
    return !eof;

  if (type != NKE_RECORD_END_OF_MESSAGE)
    return !eof;

  message->complete = 1;

  return 1;
}

/* ================================================== */

static void
stop_session(NKSN_Instance inst)
{
  if (inst->state == KE_STOPPED)
    return;

  inst->state = KE_STOPPED;

  SCH_RemoveFileHandler(inst->sock_fd);
  SCK_CloseSocket(inst->sock_fd);
  inst->sock_fd = INVALID_SOCK_FD;

  Free(inst->label);
  inst->label = NULL;

  TLS_DestroyInstance(inst->tls_session);
  inst->tls_session = NULL;

  SCH_RemoveTimeout(inst->timeout_id);
  inst->timeout_id = 0;
}

/* ================================================== */

static void
session_timeout(void *arg)
{
  NKSN_Instance inst = arg;

  LOG(inst->server ? LOGS_DEBUG : LOGS_ERR, "NTS-KE session with %s timed out", inst->label);

  inst->timeout_id = 0;
  stop_session(inst);
}

/* ================================================== */

static void
set_input_output(NKSN_Instance inst, int output)
{
  SCH_SetFileHandlerEvent(inst->sock_fd, SCH_FILE_INPUT, !output);
  SCH_SetFileHandlerEvent(inst->sock_fd, SCH_FILE_OUTPUT, output);
}

/* ================================================== */

static void
change_state(NKSN_Instance inst, KeState state)
{
  int output;

  switch (state) {
    case KE_HANDSHAKE:
      output = !inst->server;
      break;
    case KE_WAIT_CONNECT:
    case KE_SEND:
    case KE_SHUTDOWN:
      output = 1;
      break;
    case KE_RECEIVE:
      output = 0;
      break;
    default:
      assert(0);
  }

  set_input_output(inst, output);

  inst->state = state;
}

/* ================================================== */

static int
handle_event(NKSN_Instance inst, int event)
{
  struct Message *message = &inst->message;
  TLS_Status s;
  int r;

  DEBUG_LOG("Session event %d fd=%d state=%d", event, inst->sock_fd, (int)inst->state);

  switch (inst->state) {
    case KE_WAIT_CONNECT:
      /* Check if connect() succeeded */
      if (event != SCH_FILE_OUTPUT)
        return 0;

      /* Get the socket error */
      if (!SCK_GetIntOption(inst->sock_fd, SOL_SOCKET, SO_ERROR, &r))
        r = EINVAL;

      if (r != 0) {
        LOG(LOGS_ERR, "Could not connect to %s : %s", inst->label, strerror(r));
        stop_session(inst);
        return 0;
      }

      DEBUG_LOG("Connected to %s", inst->label);

      change_state(inst, KE_HANDSHAKE);
      return 0;

    case KE_HANDSHAKE:
      s = TLS_DoHandshake(inst->tls_session);

      switch (s) {
        case TLS_SUCCESS:
          break;
        case TLS_AGAIN_OUTPUT:
        case TLS_AGAIN_INPUT:
          set_input_output(inst, s == TLS_AGAIN_OUTPUT);
          return 0;
        default:
          stop_session(inst);

          /* Increase the retry interval if the handshake did not fail due
             to the other end closing the connection */
          if (s != TLS_CLOSED)
            inst->retry_factor = NKE_RETRY_FACTOR2_TLS;

          return 0;
      }

      inst->retry_factor = NKE_RETRY_FACTOR2_TLS;

      /* Client will send a request to the server */
      change_state(inst, inst->server ? KE_RECEIVE : KE_SEND);
      return 0;

    case KE_SEND:
      assert(inst->new_message && message->complete);
      assert(message->length <= sizeof (message->data) && message->length > message->sent);

      s = TLS_Send(inst->tls_session, &message->data[message->sent],
                   message->length - message->sent, &r);

      switch (s) {
        case TLS_SUCCESS:
          break;
        case TLS_AGAIN_OUTPUT:
          return 0;
        default:
          stop_session(inst);
          return 0;
      }

      DEBUG_LOG("Sent %d bytes to %s", r, inst->label);

      message->sent += r;
      if (message->sent < message->length)
        return 0;

      /* Client will receive a response */
      change_state(inst, inst->server ? KE_SHUTDOWN : KE_RECEIVE);
      reset_message(&inst->message);
      inst->new_message = 0;
      return 0;

    case KE_RECEIVE:
      do {
        if (message->length >= sizeof (message->data)) {
          DEBUG_LOG("Message is too long");
          stop_session(inst);
          return 0;
        }

        s = TLS_Receive(inst->tls_session, &message->data[message->length],
                        sizeof (message->data) - message->length, &r);

        switch (s) {
          case TLS_SUCCESS:
            break;
          case TLS_AGAIN_INPUT:
            return 0;
          default:
            stop_session(inst);
            return 0;
        }

        DEBUG_LOG("Received %d bytes from %s", r, inst->label);

        message->length += r;

      } while (TLS_CheckPending(inst->tls_session));

      if (!check_message_format(message, r == 0)) {
        LOG(inst->server ? LOGS_DEBUG : LOGS_ERR,
            "Received invalid NTS-KE message from %s", inst->label);
        stop_session(inst);
        return 0;
      }

      /* Wait for more data if the message is not complete yet */
      if (!message->complete)
        return 0;

      /* Server will send a response to the client */
      change_state(inst, inst->server ? KE_SEND : KE_SHUTDOWN);

      /* Return success to process the received message */
      return 1;

    case KE_SHUTDOWN:
      s = TLS_Shutdown(inst->tls_session);

      switch (s) {
        case TLS_SUCCESS:
          break;
        case TLS_AGAIN_OUTPUT:
        case TLS_AGAIN_INPUT:
          set_input_output(inst, s == TLS_AGAIN_OUTPUT);
          return 0;
        default:
          stop_session(inst);
          return 0;
      }

      SCK_ShutdownConnection(inst->sock_fd);
      stop_session(inst);

      DEBUG_LOG("Shutdown completed");
      return 0;

    default:
      assert(0);
      return 0;
  }
}

/* ================================================== */

static void
read_write_socket(int fd, int event, void *arg)
{
  NKSN_Instance inst = arg;

  if (!handle_event(inst, event))
    return;

  /* A valid message was received.  Call the handler to process the message,
     and prepare a response if it is a server. */

  reset_message_parsing(&inst->message);

  if (!(inst->handler)(inst->handler_arg)) {
    stop_session(inst);
    return;
  }
}

/* ================================================== */

static time_t
get_time(time_t *t)
{
  struct timespec now;

  LCL_ReadCookedTime(&now, NULL);
  if (t)
    *t = now.tv_sec;

  return now.tv_sec;
}

/* ================================================== */

static void
handle_step(struct timespec *raw, struct timespec *cooked, double dfreq,
            double doffset, LCL_ChangeType change_type, void *anything)
{
  if (change_type != LCL_ChangeUnknownStep && clock_updates < INT_MAX)
    clock_updates++;
}

/* ================================================== */

static int tls_initialised = 0;

static int
init_tls(void)
{
  if (tls_initialised)
    return 1;

  if (!TLS_Initialise(&get_time))
    return 0;

  tls_initialised = 1;
  DEBUG_LOG("Initialised");

  LCL_AddParameterChangeHandler(handle_step, NULL);

  return 1;
}

/* ================================================== */

static void
deinit_tls(void)
{
  if (!tls_initialised || credentials_counter > 0)
    return;

  LCL_RemoveParameterChangeHandler(handle_step, NULL);

  TLS_Finalise();
  tls_initialised = 0;
  DEBUG_LOG("Deinitialised");
}

/* ================================================== */

static NKSN_Credentials
create_credentials(const char **certs, const char **keys, int n_certs_keys,
                   const char **trusted_certs, uint32_t *trusted_certs_ids,
                   int n_trusted_certs, uint32_t trusted_cert_set)
{
  TLS_Credentials credentials;

  if (!init_tls())
    return NULL;

  credentials = TLS_CreateCredentials(certs, keys, n_certs_keys, trusted_certs,
                                      trusted_certs_ids, n_trusted_certs, trusted_cert_set);
  if (!credentials) {
    deinit_tls();
    return NULL;
  }

  credentials_counter++;

  return credentials;
}

/* ================================================== */

NKSN_Credentials
NKSN_CreateServerCertCredentials(const char **certs, const char **keys, int n_certs_keys)
{
  return create_credentials(certs, keys, n_certs_keys, NULL, NULL, 0, 0);
}

/* ================================================== */

NKSN_Credentials
NKSN_CreateClientCertCredentials(const char **certs, uint32_t *ids,
                                 int n_certs_ids, uint32_t trusted_cert_set)
{
  return create_credentials(NULL, NULL, 0, certs, ids, n_certs_ids, trusted_cert_set);
}

/* ================================================== */

void
NKSN_DestroyCertCredentials(NKSN_Credentials credentials)
{
  TLS_DestroyCredentials(credentials);
  credentials_counter--;
  deinit_tls();
}

/* ================================================== */

NKSN_Instance
NKSN_CreateInstance(int server_mode, const char *server_name,
                    NKSN_MessageHandler handler, void *handler_arg)
{
  NKSN_Instance inst;

  inst = MallocNew(struct NKSN_Instance_Record);

  inst->server = server_mode;
  inst->server_name = server_name ? Strdup(server_name) : NULL;
  inst->handler = handler;
  inst->handler_arg = handler_arg;
  /* Replace a NULL argument with the session itself */
  if (!inst->handler_arg)
    inst->handler_arg = inst;

  inst->state = KE_STOPPED;
  inst->sock_fd = INVALID_SOCK_FD;
  inst->label = NULL;
  inst->tls_session = NULL;
  inst->timeout_id = 0;
  inst->retry_factor = NKE_RETRY_FACTOR2_CONNECT;

  return inst;
}

/* ================================================== */

void
NKSN_DestroyInstance(NKSN_Instance inst)
{
  stop_session(inst);

  Free(inst->server_name);
  Free(inst);
}

/* ================================================== */

int
NKSN_StartSession(NKSN_Instance inst, int sock_fd, const char *label,
                  NKSN_Credentials credentials, double timeout)
{
  assert(inst->state == KE_STOPPED);

  inst->tls_session = TLS_CreateInstance(inst->server, sock_fd, inst->server_name,
                                         label, NKE_ALPN_NAME, credentials,
                                         clock_updates < CNF_GetNoCertTimeCheck());
  if (!inst->tls_session)
    return 0;

  inst->sock_fd = sock_fd;
  SCH_AddFileHandler(sock_fd, SCH_FILE_INPUT, read_write_socket, inst);

  inst->label = Strdup(label);
  inst->timeout_id = SCH_AddTimeoutByDelay(timeout, session_timeout, inst);
  inst->retry_factor = NKE_RETRY_FACTOR2_CONNECT;

  reset_message(&inst->message);
  inst->new_message = 0;

  change_state(inst, inst->server ? KE_HANDSHAKE : KE_WAIT_CONNECT);

  return 1;
}

/* ================================================== */

void
NKSN_BeginMessage(NKSN_Instance inst)
{
  reset_message(&inst->message);
  inst->new_message = 1;
}

/* ================================================== */

int
NKSN_AddRecord(NKSN_Instance inst, int critical, int type, const void *body, int body_length)
{
  assert(inst->new_message && !inst->message.complete);
  assert(type != NKE_RECORD_END_OF_MESSAGE);

  return add_record(&inst->message, critical, type, body, body_length);
}

/* ================================================== */

int
NKSN_EndMessage(NKSN_Instance inst)
{
  assert(!inst->message.complete);

  /* Terminate the message */
  if (!add_record(&inst->message, 1, NKE_RECORD_END_OF_MESSAGE, NULL, 0))
    return 0;

  inst->message.complete = 1;

  return 1;
}

/* ================================================== */

int
NKSN_GetRecord(NKSN_Instance inst, int *critical, int *type, int *body_length,
               void *body, int buffer_length)
{
  int type2;

  assert(inst->message.complete);

  if (body_length)
    *body_length = 0;

  if (!get_record(&inst->message, critical, &type2, body_length, body, buffer_length))
    return 0;

  /* Hide the end-of-message record */
  if (type2 == NKE_RECORD_END_OF_MESSAGE)
    return 0;

  if (type)
    *type = type2;

  return 1;
}

/* ================================================== */

int
NKSN_GetKeys(NKSN_Instance inst, SIV_Algorithm algorithm, SIV_Algorithm exporter_algorithm,
             int next_protocol, NKE_Key *c2s, NKE_Key *s2c)
{
  int length = SIV_GetKeyLength(algorithm);
  struct {
    uint16_t next_protocol;
    uint16_t algorithm;
    uint8_t is_s2c;
    uint8_t _pad;
  } context;

  if (!inst->tls_session)
    return 0;

  if (length <= 0 || length > sizeof (c2s->key) || length > sizeof (s2c->key)) {
    DEBUG_LOG("Invalid algorithm");
    return 0;
  }

  assert(sizeof (context) == 6);
  context.next_protocol = htons(next_protocol);
  context.algorithm = htons(exporter_algorithm);

  context.is_s2c = 0;
  if (!TLS_ExportKey(inst->tls_session, sizeof (NKE_EXPORTER_LABEL) - 1, NKE_EXPORTER_LABEL,
                     sizeof (context) - 1, &context, length, c2s->key)) {
    DEBUG_LOG("Could not export key");
    return 0;
  }

  context.is_s2c = 1;
  if (!TLS_ExportKey(inst->tls_session, sizeof (NKE_EXPORTER_LABEL) - 1, NKE_EXPORTER_LABEL,
                     sizeof (context) - 1, &context, length, s2c->key)) {
    DEBUG_LOG("Could not export key");
    return 0;
  }

  c2s->length = length;
  s2c->length = length;

  return 1;
}

/* ================================================== */

int
NKSN_IsStopped(NKSN_Instance inst)
{
  return inst->state == KE_STOPPED;
}

/* ================================================== */

void
NKSN_StopSession(NKSN_Instance inst)
{
  stop_session(inst);
}

/* ================================================== */

int
NKSN_GetRetryFactor(NKSN_Instance inst)
{
  return inst->retry_factor;
}
