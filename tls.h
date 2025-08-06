/*
  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
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

  Header file for the TLS session
  */

#ifndef GOT_TLS_H
#define GOT_TLS_H

struct TLS_Instance_Record;

typedef struct TLS_Instance_Record *TLS_Instance;

typedef void *TLS_Credentials;

typedef enum {
  /* TLS operation succeeded */
  TLS_SUCCESS,
  /* TLS operation failed.
     No more operations should be called and the session should be destroyed. */
  TLS_FAILED,
  /* TLS session closed by other end */
  TLS_CLOSED,
  /* The last TLS operation should be called again when input is ready */
  TLS_AGAIN_INPUT,
  /* The last TLS operation should be called again when output is ready */
  TLS_AGAIN_OUTPUT,
} TLS_Status;

/* Initialize TLS */
extern int TLS_Initialise(time_t (*get_time)(time_t *t));

/* Deinitialize TLS */
extern void TLS_Finalise(void);

/* Create new TLS credentials instance */
extern TLS_Credentials TLS_CreateCredentials(const char **certs, const char **keys,
                                             int n_certs_keys, const char **trusted_certs,
                                             uint32_t * trusted_certs_ids, int n_trusted_certs,
                                             uint32_t trusted_cert_set);

/* Destroy TLS credentials instance */
extern void TLS_DestroyCredentials(TLS_Credentials credentials);

/* Create new TLS session instance */
extern TLS_Instance TLS_CreateInstance(int server_mode, int sock_fd, const char *server_name,
                                       const char *label, const char *alpn_name,
                                       TLS_Credentials credentials, int disable_time_checks);

/* Destroy TLS instance */
extern void TLS_DestroyInstance(TLS_Instance inst);

/* Perform TLS handshake */
extern TLS_Status TLS_DoHandshake(TLS_Instance inst);

/* Send data over TLS */
extern TLS_Status TLS_Send(TLS_Instance inst, const void *data, int length, int *sent);

/* Receive data over TLS */
extern TLS_Status TLS_Receive(TLS_Instance inst, void *data, int length, int *received);

/* Check if there is data pending to read */
extern int TLS_CheckPending(TLS_Instance inst);

/* Perform TLS shutdown */
extern TLS_Status TLS_Shutdown(TLS_Instance inst);

/* Export key from TLS instance */
extern int TLS_ExportKey(TLS_Instance inst, int label_length, const char *label,
                         int context_length, const void *context, int key_length,
                         unsigned char *key);

#endif
