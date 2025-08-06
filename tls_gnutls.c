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

  Routines implementing TLS session handling using the gnutls library.
  */

#include "config.h"

#include "sysincl.h"

#include "tls.h"

#include "conf.h"
#include "logging.h"
#include "memory.h"
#include "util.h"

#include <gnutls/gnutls.h>
#include <gnutls/x509.h>

struct TLS_Instance_Record {
  gnutls_session_t session;
  int server;
  char *label;
  char *alpn_name;
};

/* ================================================== */

static gnutls_priority_t priority_cache;

/* ================================================== */

int
TLS_Initialise(time_t (*get_time)(time_t *t))
{
  int r = gnutls_global_init();

  if (r < 0)
    LOG_FATAL("Could not initialise %s : %s", "gnutls", gnutls_strerror(r));

  /* Prepare a priority cache for server and client NTS-KE sessions
     (the NTS specification requires TLS1.3 or later) */
  r = gnutls_priority_init2(&priority_cache,
                            "-VERS-SSL3.0:-VERS-TLS1.0:-VERS-TLS1.1:-VERS-TLS1.2:-VERS-DTLS-ALL",
                            NULL, GNUTLS_PRIORITY_INIT_DEF_APPEND);
  if (r < 0) {
    LOG(LOGS_ERR, "Could not initialise %s : %s",
        "priority cache for TLS", gnutls_strerror(r));
    gnutls_global_deinit();
    return 0;
  }

  /* Use our clock instead of the system clock in certificate verification */
  gnutls_global_set_time_function(get_time);

  return 1;
}

/* ================================================== */

void
TLS_Finalise(void)
{
  gnutls_priority_deinit(priority_cache);
  gnutls_global_deinit();
}

/* ================================================== */

TLS_Credentials
TLS_CreateCredentials(const char **certs, const char **keys, int n_certs_keys,
                      const char **trusted_certs, uint32_t *trusted_certs_ids,
                      int n_trusted_certs, uint32_t trusted_cert_set)
{
  gnutls_certificate_credentials_t credentials = NULL;
  int i, r;

  r = gnutls_certificate_allocate_credentials(&credentials);
  if (r < 0)
    goto error;

  if (certs && keys) {
    BRIEF_ASSERT(!trusted_certs && !trusted_certs_ids);

    for (i = 0; i < n_certs_keys; i++) {
      if (!UTI_CheckFilePermissions(keys[i], 0771))
        ;
      r = gnutls_certificate_set_x509_key_file(credentials, certs[i], keys[i],
                                               GNUTLS_X509_FMT_PEM);
      if (r < 0)
        goto error;
    }
  } else {
    BRIEF_ASSERT(!certs && !keys && n_certs_keys <= 0);

    if (trusted_cert_set == 0 && !CNF_GetNoSystemCert()) {
      r = gnutls_certificate_set_x509_system_trust(credentials);
      if (r < 0)
        goto error;
    }

    if (trusted_certs && trusted_certs_ids) {
      for (i = 0; i < n_trusted_certs; i++) {
        struct stat buf;

        if (trusted_certs_ids[i] != trusted_cert_set)
          continue;

        if (stat(trusted_certs[i], &buf) == 0 && S_ISDIR(buf.st_mode))
          r = gnutls_certificate_set_x509_trust_dir(credentials, trusted_certs[i],
                                                    GNUTLS_X509_FMT_PEM);
        else
          r = gnutls_certificate_set_x509_trust_file(credentials, trusted_certs[i],
                                                     GNUTLS_X509_FMT_PEM);
        if (r < 0)
          goto error;

        DEBUG_LOG("Added %d trusted certs from %s", r, trusted_certs[i]);
      }
    }
  }

  return credentials;

error:
  LOG(LOGS_ERR, "Could not set credentials : %s", gnutls_strerror(r));
  if (credentials)
    gnutls_certificate_free_credentials(credentials);
  return NULL;
}

/* ================================================== */

void
TLS_DestroyCredentials(TLS_Credentials credentials)
{
  gnutls_certificate_free_credentials((gnutls_certificate_credentials_t)credentials);
}

/* ================================================== */

TLS_Instance
TLS_CreateInstance(int server_mode, int sock_fd, const char *server_name, const char *label,
                   const char *alpn_name, TLS_Credentials credentials, int disable_time_checks)
{
  gnutls_datum_t alpn;
  unsigned int flags;
  int r;

  TLS_Instance inst = MallocNew(struct TLS_Instance_Record);

  inst->session = NULL;
  inst->server = server_mode;
  inst->label = Strdup(label);
  inst->alpn_name = Strdup(alpn_name);

  r = gnutls_init(&inst->session, GNUTLS_NONBLOCK | GNUTLS_NO_TICKETS |
                                  (server_mode ? GNUTLS_SERVER : GNUTLS_CLIENT));
  if (r < 0) {
    LOG(LOGS_ERR, "Could not %s TLS session : %s", "create", gnutls_strerror(r));
    inst->session = NULL;
    goto error;
  }

  if (!server_mode) {
    assert(server_name);

    if (!UTI_IsStringIP(server_name)) {
      r = gnutls_server_name_set(inst->session, GNUTLS_NAME_DNS, server_name,
                                 strlen(server_name));
      if (r < 0)
        goto error;
    }

    flags = 0;

    if (disable_time_checks) {
      flags |= GNUTLS_VERIFY_DISABLE_TIME_CHECKS | GNUTLS_VERIFY_DISABLE_TRUSTED_TIME_CHECKS;
      DEBUG_LOG("Disabled time checks");
    }

    gnutls_session_set_verify_cert(inst->session, server_name, flags);
  }

  r = gnutls_priority_set(inst->session, priority_cache);
  if (r < 0)
    goto error;

  r = gnutls_credentials_set(inst->session, GNUTLS_CRD_CERTIFICATE, credentials);
  if (r < 0)
    goto error;

  alpn.data = (unsigned char *)inst->alpn_name;
  alpn.size = strlen(inst->alpn_name);

  r = gnutls_alpn_set_protocols(inst->session, &alpn, 1, 0);
  if (r < 0)
    goto error;

  gnutls_transport_set_int(inst->session, sock_fd);

  return inst;

error:
  LOG(LOGS_ERR, "Could not %s TLS session : %s", "set", gnutls_strerror(r));
  TLS_DestroyInstance(inst);
  return NULL;
}

/* ================================================== */

void
TLS_DestroyInstance(TLS_Instance inst)
{
  if (inst->session)
    gnutls_deinit(inst->session);

  Free(inst->label);
  Free(inst->alpn_name);

  Free(inst);
}

/* ================================================== */

static int
check_alpn(TLS_Instance inst)
{
  gnutls_datum_t alpn;
  int length = strlen(inst->alpn_name);

  if (gnutls_alpn_get_selected_protocol(inst->session, &alpn) < 0 ||
      alpn.size != length || memcmp(alpn.data, inst->alpn_name, length) != 0)
    return 0;

  return 1;
}

/* ================================================== */

TLS_Status
TLS_DoHandshake(TLS_Instance inst)
{
  int r = gnutls_handshake(inst->session);

  if (r < 0) {
    if (gnutls_error_is_fatal(r)) {
      gnutls_datum_t cert_error;

      /* Get a description of verification errors */
      if (r != GNUTLS_E_CERTIFICATE_VERIFICATION_ERROR ||
          gnutls_certificate_verification_status_print(
              gnutls_session_get_verify_cert_status(inst->session),
              gnutls_certificate_type_get(inst->session), &cert_error, 0) < 0)
        cert_error.data = NULL;

      LOG(inst->server ? LOGS_DEBUG : LOGS_ERR,
          "TLS handshake with %s failed : %s%s%s", inst->label, gnutls_strerror(r),
          cert_error.data ? " " : "", cert_error.data ? (const char *)cert_error.data : "");

      if (cert_error.data)
        gnutls_free(cert_error.data);

      /* Increase the retry interval if the handshake did not fail due
         to the other end closing the connection */
      if (r != GNUTLS_E_PULL_ERROR && r != GNUTLS_E_PREMATURE_TERMINATION)
        return TLS_FAILED;

      return TLS_CLOSED;
    }

    return gnutls_record_get_direction(inst->session) ? TLS_AGAIN_OUTPUT : TLS_AGAIN_INPUT;
  }

  if (DEBUG) {
    char *description = gnutls_session_get_desc(inst->session);
    DEBUG_LOG("Handshake with %s completed %s", inst->label, description ? description : "");
    gnutls_free(description);
  }

  if (!check_alpn(inst)) {
    LOG(inst->server ? LOGS_DEBUG : LOGS_ERR, "NTS-KE not supported by %s", inst->label);
    return TLS_FAILED;
  }

  return TLS_SUCCESS;
}

/* ================================================== */

TLS_Status
TLS_Send(TLS_Instance inst, const void *data, int length, int *sent)
{
  int r;

  if (length < 0)
    return TLS_FAILED;

  r = gnutls_record_send(inst->session, data, length);

  if (r < 0) {
    if (gnutls_error_is_fatal(r)) {
      LOG(inst->server ? LOGS_DEBUG : LOGS_ERR,
          "Could not send NTS-KE message to %s : %s", inst->label, gnutls_strerror(r));
      return TLS_FAILED;
    }

    return TLS_AGAIN_OUTPUT;
  }

  *sent = r;

  return TLS_SUCCESS;
}

/* ================================================== */

TLS_Status
TLS_Receive(TLS_Instance inst, void *data, int length, int *received)
{
  int r;

  if (length < 0)
    return TLS_FAILED;

  r = gnutls_record_recv(inst->session, data, length);

  if (r < 0) {
    /* Handle a renegotiation request on both client and server as
       a protocol error */
    if (gnutls_error_is_fatal(r) || r == GNUTLS_E_REHANDSHAKE) {
      LOG(inst->server ? LOGS_DEBUG : LOGS_ERR,
          "Could not receive NTS-KE message from %s : %s", inst->label, gnutls_strerror(r));
      return TLS_FAILED;
    }

    return TLS_AGAIN_INPUT;
  }

  *received = r;

  return TLS_SUCCESS;
}

/* ================================================== */

int
TLS_CheckPending(TLS_Instance inst)
{
  return gnutls_record_check_pending(inst->session) > 0;
}

/* ================================================== */

TLS_Status
TLS_Shutdown(TLS_Instance inst)
{
  int r = gnutls_bye(inst->session, GNUTLS_SHUT_RDWR);

  if (r < 0) {
    if (gnutls_error_is_fatal(r)) {
      DEBUG_LOG("Shutdown with %s failed : %s", inst->label, gnutls_strerror(r));
      return TLS_FAILED;
    }

    return gnutls_record_get_direction(inst->session) ? TLS_AGAIN_OUTPUT : TLS_AGAIN_INPUT;
  }

  return TLS_SUCCESS;
}

/* ================================================== */

int
TLS_ExportKey(TLS_Instance inst, int label_length, const char *label, int context_length,
              const void *context, int key_length, unsigned char *key)
{
  int r;

  if (label_length < 0 || context_length < 0 || key_length < 0)
    return 0;

  r = gnutls_prf_rfc5705(inst->session, label_length, label, context_length, (char *)context,
                         key_length, (char *)key);

  return r >= 0;
}
