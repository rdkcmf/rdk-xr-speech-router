/*
##########################################################################
# If not stated otherwise in this file or this component's LICENSE
# file the following copyright and licenses apply:
#
# Copyright 2019 RDK Management
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
##########################################################################
*/
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <mqueue.h>
#include "xrsr_private.h"
#include "xrsr_protocol_ws_sm.h"

static void xrsr_ws_event(xrsr_state_ws_t *ws, tStEventID id, bool from_state_handler);
static void xrsr_ws_reset(xrsr_state_ws_t *ws);
static void xrsr_ws_sm_init(xrsr_state_ws_t *ws);

static void xrsr_ws_on_msg(xrsr_state_ws_t *ws, noPollConn *conn, noPollMsg *msg);
static void xrsr_ws_on_close(noPollCtx *ctx,  noPollConn *conn, noPollPtr user_data);
static void xrsr_ws_nopoll_log(noPollCtx * ctx, noPollDebugLevel level, const char * log_msg, noPollPtr user_data);
static void xrsr_ws_process_timeout(void *data);
static void xrsr_ws_speech_stream_end(xrsr_state_ws_t *ws, xrsr_stream_end_reason_t reason, bool detect_resume);
static bool xrsr_ws_connect_new(xrsr_state_ws_t *ws);
static noPollConnOpts *xrsr_conn_opts_get(const char *sat_token);

static bool xrsr_ws_queue_msg_out(xrsr_state_ws_t *ws, const char *msg, uint32_t length);
static bool xrsr_ws_is_msg_out(xrsr_state_ws_t *ws);
static bool xrsr_ws_get_msg_out(xrsr_state_ws_t *ws, char **msg, uint32_t *length);
static void xrsr_ws_clear_msg_out(xrsr_state_ws_t *ws);

// This function kicks off the session
void xrsr_protocol_handler_ws(xrsr_src_t src, bool retry, bool user_initiated, xraudio_input_format_t xraudio_format, xraudio_keyword_detector_result_t *detector_result, const char* transcription_in) {
   xrsr_queue_msg_session_begin_t msg;
   msg.header.type     = XRSR_QUEUE_MSG_TYPE_SESSION_BEGIN;
   msg.src             = src;
   msg.retry           = retry;
   msg.user_initiated  = user_initiated;
   msg.xraudio_format  = xraudio_format;
   if(detector_result == NULL) {
      msg.has_result = false;
      memset(&msg.detector_result, 0, sizeof(msg.detector_result));
   } else {
      msg.has_result      = true;
      msg.detector_result = *detector_result;
   }
   rdkx_timestamp_get_realtime(&msg.timestamp);

   if (transcription_in != NULL) {
      strncpy(msg.transcription_in, transcription_in, sizeof(msg.transcription_in)-1);
      msg.transcription_in[sizeof(msg.transcription_in)-1] = '\0';
   } else {
      msg.transcription_in[0] = '\0';
   }

   xrsr_queue_msg_push(xrsr_msgq_fd_get(), (const char *)&msg, sizeof(msg));
}

bool xrsr_ws_init(xrsr_state_ws_t *ws, xrsr_ws_params_t *params) {
   if(ws == NULL || params == NULL) {
      XLOGD_ERROR("invalid params - ws <%p> params <%p>", ws, params);
      return(false);
   }
   
   memset(ws, 0, sizeof(*ws));
   ws->obj_ctx = nopoll_ctx_new();
   
   if(ws->obj_ctx == NULL) {
      XLOGD_ERROR("unable to create context");
      return(false);
   }
   ws->pending_msg   = NULL;

   sem_init(&ws->msg_out_semaphore, 0, 1);
   ws->msg_out_count = 0;
   memset(ws->msg_out, 0, sizeof(ws->msg_out));

   xrsr_ws_update_dst_params(ws, params->dst_params);
   ws->timer_obj          = params->timer_obj;
   ws->prot               = params->prot;
   ws->audio_pipe_fd_read = -1;
   xrsr_ws_reset(ws);

   xrsr_ws_host_name_set(ws, params->host_name);

   xrsr_ws_sm_init(ws);

   XLOGD_INFO("host name <%s>", params->host_name ? params->host_name : "");

   return(true);
}

bool xrsr_ws_update_dst_params(xrsr_state_ws_t *ws, xrsr_dst_param_ptrs_t *params) {
   bool ret = false;
   if(ws) {
      if(params->debug != NULL) { // debug parameter was specified
         if(*params->debug) {
            nopoll_log_enable(ws->obj_ctx, nopoll_true);
            nopoll_log_set_handler(ws->obj_ctx, xrsr_ws_nopoll_log, NULL);
            ws->debug_enabled = true;
         } else {
            nopoll_log_set_handler(ws->obj_ctx, NULL, NULL); // Remove log handler
            nopoll_log_enable(ws->obj_ctx, nopoll_false); // disable logging
            ws->debug_enabled = false;
         }
      } else if(JSON_BOOL_VALUE_WS_DEBUG) { // debug enabled by default
         nopoll_log_enable(ws->obj_ctx, nopoll_true);
         nopoll_log_set_handler(ws->obj_ctx, xrsr_ws_nopoll_log, NULL);
         ws->debug_enabled = true;
      } else {
         nopoll_log_set_handler(ws->obj_ctx, NULL, NULL); // Remove log handler
         nopoll_log_enable(ws->obj_ctx, nopoll_false); // disable logging
         ws->debug_enabled = false;
      }

      if(params->connect_check_interval != NULL) {
         ws->connect_check_interval = *params->connect_check_interval;
      } else {
         ws->connect_check_interval = JSON_INT_VALUE_WS_FPM_CONNECT_CHECK_INTERVAL;
      }
      if(params->timeout_connect != NULL) {
         ws->timeout_connect = *params->timeout_connect;
      } else {
         ws->timeout_connect = JSON_INT_VALUE_WS_FPM_TIMEOUT_CONNECT;
      }
      if(params->timeout_inactivity != NULL) {
         ws->timeout_inactivity = *params->timeout_inactivity;
      } else {
         ws->timeout_inactivity = JSON_INT_VALUE_WS_FPM_TIMEOUT_INACTIVITY;
      }
      if(params->timeout_session != NULL) {
         ws->timeout_session = *params->timeout_session;
      } else {
         ws->timeout_session = JSON_INT_VALUE_WS_FPM_TIMEOUT_SESSION;
      }
      if(params->ipv4_fallback != NULL) {
         ws->ipv4_fallback = *params->ipv4_fallback;
      } else {
         ws->ipv4_fallback = JSON_BOOL_VALUE_WS_FPM_IPV4_FALLBACK;
      }
      if(params->backoff_delay != NULL) {
         ws->backoff_delay = *params->backoff_delay;
      } else {
         ws->backoff_delay = JSON_INT_VALUE_WS_FPM_BACKOFF_DELAY;
      }

      XLOGD_INFO("debug <%s> connect <%u, %u> inactivity <%u> session <%u> ipv4 fallback <%s> backoff delay <%u>", ws->debug_enabled ? "YES" : "NO", ws->connect_check_interval, ws->timeout_connect, ws->timeout_inactivity, ws->timeout_session, ws->ipv4_fallback ? "YES" : "NO", ws->backoff_delay);
   } else {
      XLOGD_WARN("ws state NULL");
   }
   return(ret);
}

void xrsr_ws_nopoll_log(noPollCtx * ctx, noPollDebugLevel level, const char * log_msg, noPollPtr user_data) {
   xlog_args_t args;
   args.options  = XLOG_OPTS_DEFAULT;
   args.color    = XLOG_COLOR_NONE;
   args.function = XLOG_FUNCTION_NONE;
   args.line     = XLOG_LINE_NONE;
   args.id       = XLOG_MODULE_ID;
   switch(level) {
      case NOPOLL_LEVEL_DEBUG:    { args.level = XLOG_LEVEL_DEBUG; break; }
      case NOPOLL_LEVEL_INFO:     { args.level = XLOG_LEVEL_INFO;  break; }
      case NOPOLL_LEVEL_WARNING:  { args.level = XLOG_LEVEL_WARN;  break; }
      case NOPOLL_LEVEL_CRITICAL: { args.level = XLOG_LEVEL_ERROR; break; }
      default:                    { args.level = XLOG_LEVEL_INFO;  break; }
   }
   int errsv = errno;
   xlog_printf(&args, "%s", log_msg);
   errno = errsv;
}

void xrsr_ws_term(xrsr_state_ws_t *ws) {
   XLOGD_INFO("");
   if(ws == NULL || ws->obj_ctx == NULL) {
      XLOGD_ERROR("NULL context");
      return;
   }
   if(nopoll_ctx_ref_count(ws->obj_ctx) > 1) {
      XLOGD_WARN("ws context reference count <%d>", nopoll_ctx_ref_count(ws->obj_ctx));
   }
   
   xrsr_ws_event(ws, SM_EVENT_TERMINATE, false);
   sem_destroy(&ws->msg_out_semaphore);

   nopoll_ctx_unref(ws->obj_ctx);
   ws->obj_ctx = NULL;
}

void xrsr_ws_host_name_set(xrsr_state_ws_t *ws, const char *host_name) {
   int rc = snprintf(ws->local_host_name, sizeof(ws->local_host_name), "%s", host_name ? host_name : "");
   if(rc >= sizeof(ws->local_host_name)) {
      XLOGD_ERROR("host name truncated");
   }
}

void xrsr_ws_fd_set(xrsr_state_ws_t *ws, int *nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds) {
   if(xrsr_ws_is_established(ws) && ws->socket >= 0) {
      // Always check for incoming messages if ws is established
      FD_SET(ws->socket, readfds);
      if(ws->socket >= *nfds) {
         *nfds = ws->socket + 1;
      }

      // If we need to send an outgoing message or waiting on data to go out
      if(ws->write_pending_bytes || xrsr_ws_is_msg_out(ws)) {
         FD_SET(ws->socket, writefds);
      }

      // We don't want to wake up for the audio pipe if we can't write it to the pipe
      if(ws->audio_pipe_fd_read >= 0 && !ws->write_pending_bytes) {
         FD_SET(ws->audio_pipe_fd_read, readfds);
         if(ws->audio_pipe_fd_read >= *nfds) {
            *nfds = ws->audio_pipe_fd_read + 1;
         }
      }
   }
}

void xrsr_ws_handle_fds(xrsr_state_ws_t *ws, fd_set *readfds, fd_set *writefds, fd_set *exceptfds) {
   // First, let's check if we have received a message over the websocket
   if(ws->socket >= 0 && FD_ISSET(ws->socket, readfds)) {
      XLOGD_INFO("data available for read");
      xrsr_ws_read_pending(ws);
   }

   // Now let's send any outgoing messages or pending data over the websocket
   if(ws->socket >= 0 && FD_ISSET(ws->socket, writefds)) {
      // First check if we are trying to send pending bytes
      if(ws->write_pending_bytes) {
         int bytes = nopoll_conn_pending_write_bytes(ws->obj_conn);
         if(bytes != (nopoll_conn_complete_pending_write(ws->obj_conn))) {
            XLOGD_WARN("still waiting to write pending bytes...");
            ws->write_pending_retries++;
            if(ws->write_pending_retries > XRSR_WS_WRITE_PENDING_RETRY_MAX) {
               xrsr_ws_event(ws, SM_EVENT_WS_ERROR, false);
            }
            // No point in continuing, as we haven't sent the pending data yet.
            return;
         }
         XLOGD_INFO("pending bytes written successfully");
         ws->write_pending_bytes   = false;
         ws->write_pending_retries = 0;
      }

      // Now lets see if we have a message to send out
      if(xrsr_ws_is_msg_out(ws)) {
         int bytes    = 0;
         char *buf    = NULL;
         uint32_t len = 0;

         if(xrsr_ws_get_msg_out(ws, &buf, &len)) {
            if(buf) {
               XLOGD_INFO("sending outgoing message");
               bytes = nopoll_conn_send_text(ws->obj_conn, (const char *)buf, (long)len);
               // NoPoll now has the data copied into an internal buffer
               free(buf);
               buf = NULL;
               if(bytes == 0 || bytes == -1) {
                  XLOGD_ERROR("failed to write to websocket");
                  xrsr_ws_event(ws, SM_EVENT_WS_ERROR, false);
               } else if(bytes == -2 || bytes != len) {
                  if(bytes == -2) {
                     XLOGD_WARN("websocket would block sending outgoing message");
                  } else {
                     XLOGD_WARN("partial message sent");
                  }
                  ws->write_pending_bytes = true;
                  // No point in continuing, as we haven't sent this message yet.
                  return;
               }
            }
         }
      }
   }

   // Finally let's check if we have audio data available to send
   if(ws->audio_pipe_fd_read >= 0 && FD_ISSET(ws->audio_pipe_fd_read, readfds)) {
      // Read the audio data and write to websocket
      int rc = read(ws->audio_pipe_fd_read, ws->buffer, sizeof(ws->buffer));
      if(rc < 0) {
         int errsv = errno;
         if(errsv == EAGAIN || errsv == EWOULDBLOCK) {
            XLOGD_INFO("read would block");
            xrsr_ws_event(ws, SM_EVENT_AUDIO_ERROR, false);
         } else {
            XLOGD_ERROR("pipe read error <%s>", strerror(errsv));
            xrsr_ws_event(ws, SM_EVENT_AUDIO_ERROR, false);
         }
      } else if(rc == 0) { // EOF
         XLOGD_INFO("pipe read EOF");
         xrsr_ws_event(ws, SM_EVENT_EOS_PIPE, false);
      } else {
         XLOGD_INFO("pipe read <%d>", rc);
         uint32_t bytes_read = (uint32_t)rc;

         rc = nopoll_conn_send_binary(ws->obj_conn, (const char *)ws->buffer, (long)bytes_read);
         if(rc == -2) { // NOPOLL_EWOULDBLOCK
            XLOGD_ERROR("websocket would block");
            // Set flag to wait for socket write ready
            ws->write_pending_bytes = true;
         } else if(rc == 0) { // no bytes sent (see errno indication)
            int errsv = errno;
            XLOGD_ERROR("websocket failure <%s>", strerror(errsv));
            xrsr_ws_event(ws, SM_EVENT_WS_ERROR, false);
         } else if(rc < 0) { // failure found
            XLOGD_ERROR("websocket failure <%d>", rc);
            xrsr_ws_event(ws, SM_EVENT_WS_ERROR, false);
         } else if(rc != bytes_read) { // partial bytes sent
            XLOGD_WARN("websocket size mismatch req <%u> sent <%d>", bytes_read, rc);
            // Set flag to wait for socket write ready
            ws->write_pending_bytes = true;
            ws->audio_txd_bytes    += (uint32_t) rc;
         } else {
            ws->audio_txd_bytes += bytes_read;
         }
         if(!ws->audio_kwd_notified && (ws->audio_txd_bytes >= ws->audio_kwd_bytes)) {
            if(!xrsr_speech_stream_kwd(ws->uuid,  ws->audio_src, ws->dst_index)) {
               XLOGD_ERROR("xrsr_speech_stream_kwd failed");
            }
            ws->audio_kwd_notified = true;
         }
      }
   }
}

void xrsr_ws_process_timeout(void *data) {
   XLOGD_INFO("");
   xrsr_state_ws_t *ws = (xrsr_state_ws_t *)data;
   xrsr_ws_event(ws, SM_EVENT_TIMEOUT, false);
}

bool xrsr_ws_connect(xrsr_state_ws_t *ws, xrsr_url_parts_t *url_parts, xrsr_src_t audio_src, xraudio_input_format_t xraudio_format, bool user_initiated, bool is_retry, bool deferred, const char **query_strs) {
   XLOGD_INFO("");
   if(ws == NULL) {
      XLOGD_ERROR("NULL xrsr_state_ws_t");
      return(false);
   } else if(ws->obj_ctx == NULL) {
      XLOGD_ERROR("NULL param");
      return(false);
   } 

   rdkx_timestamp_get(&ws->retry_timestamp_end);
   rdkx_timestamp_add_ms(&ws->retry_timestamp_end, ws->timeout_session);

   ws->audio_src      = audio_src;
   ws->xraudio_format = xraudio_format;

   strncpy(ws->url, url_parts->urle, sizeof(ws->url)); // Copy main url

   if(query_strs != NULL && *query_strs != NULL) { // add attribute-value pairs to the query string
      bool delimit = true;
      if(!url_parts->has_query) {
         strlcat(ws->url, "?", sizeof(ws->url));
         delimit = false;
      }

      do {
         if(delimit) {
            strlcat(ws->url, "&", sizeof(ws->url));
         }
         strlcat(ws->url, *query_strs, sizeof(ws->url));
         delimit = true;
         query_strs++;
      } while(*query_strs != NULL);
   }

   XLOGD_INFO("local host <%s> remote host <%s> port <%s> url <%s> deferred <%s> family <%s> retry period <%u> ms", ws->local_host_name, url_parts->host, url_parts->port_str, ws->url, (deferred) ? "YES" : "NO", xrsr_address_family_str(url_parts->family), ws->timeout_session);

   nopoll_conn_connect_timeout(ws->obj_ctx, ws->timeout_connect * 1000);  // wait no more than N milliseconds

   ws->url_parts          = url_parts;
   ws->user_initiated     = user_initiated;
   ws->audio_kwd_notified = true; // if keyword is present in the stream, xraudio will inform
   ws->audio_kwd_bytes    = 0;
   ws->audio_txd_bytes    = 0;
   ws->connect_wait_time  = ws->timeout_connect;
   ws->on_close           = false;
   memset(&ws->stats, 0, sizeof(ws->stats));
   memset(&ws->audio_stats, 0, sizeof(ws->audio_stats));

   if(!deferred) {
      xrsr_ws_event(ws, SM_EVENT_SESSION_BEGIN, false);
      return(true);
   }
   xrsr_ws_event(ws, SM_EVENT_SESSION_BEGIN_STM, false);
   return(true);
}

bool xrsr_ws_connect_new(xrsr_state_ws_t *ws) {
   xrsr_url_parts_t *url_parts = ws->url_parts;
   noPollConnOpts *nopoll_opts = xrsr_conn_opts_get(ws->session_config_in.ws.sat_token);

   const char *origin_fmt = "http://%s:%s";
   uint32_t origin_size = strlen(url_parts->host) + strlen(url_parts->port_str) + strlen(origin_fmt) - 3;
   char origin[origin_size];

   snprintf(origin, sizeof(origin), origin_fmt, url_parts->host, url_parts->port_str);

   XLOGD_INFO("attempt <%u>", ws->retry_cnt);

   if(ws->prot == XRSR_PROTOCOL_WSS) {
      ws->obj_conn = nopoll_conn_tls_new_auto(ws->obj_ctx, nopoll_opts, url_parts->host, url_parts->port_str, NULL, ws->url, NULL, origin);
   } else {
      ws->obj_conn = nopoll_conn_new_opts_auto(ws->obj_ctx, nopoll_opts, url_parts->host, url_parts->port_str, NULL, ws->url, NULL, origin);
   }
   
   if(ws->obj_conn == NULL) {
      XLOGD_ERROR("conn new");
      return(false);
   }
   nopoll_conn_set_on_close(ws->obj_conn, xrsr_ws_on_close, ws);
   return(true);
}

noPollConnOpts *xrsr_conn_opts_get(const char *sat_token) {
   noPollConnOpts *nopoll_opts = NULL;
   if(sat_token != NULL) {
      nopoll_opts = nopoll_conn_opts_new();
      if(nopoll_opts == NULL) {
         XLOGD_ERROR("NULL nopoll opts");
      } else {
         char sat_token_str[24 + XRSR_SAT_TOKEN_LEN_MAX] = {'\0'};
         // String must match the format: "\r\nheader:value\r\nheader2:value2" with no trailing \r\n.
         snprintf(sat_token_str, sizeof(sat_token_str), "\r\nAuthorization: Bearer %s", sat_token);
         nopoll_conn_opts_set_extra_headers(nopoll_opts, sat_token_str);
      }
   }
   return(nopoll_opts);
}

bool xrsr_ws_conn_is_ready(xrsr_state_ws_t *ws) {
   if(ws == NULL) {
      XLOGD_ERROR("NULL xrsr_state_ws_t");
      return(false);
   } else if(ws->obj_conn == NULL) {
      XLOGD_ERROR("NULL param");
      return(false);
   }
   if(nopoll_true != nopoll_conn_is_ready(ws->obj_conn)) {
      return(false);
   }

   ws->socket          = nopoll_conn_socket(ws->obj_conn);
   
   if(nopoll_true != nopoll_conn_set_sock_block(ws->socket, nopoll_false)) {
      XLOGD_WARN("unable to set non-blocking");
   }
   return(true);
}

void xrsr_ws_terminate(xrsr_state_ws_t *ws) {
   XLOGD_INFO("");
   if(ws == NULL) {
      XLOGD_ERROR("NULL xrsr_state_ws_t");
      return;
   } else if(ws->obj_ctx == NULL) {
      XLOGD_ERROR("NULL context");
      return;
   } 

   xrsr_ws_event(ws, SM_EVENT_TERMINATE, false);
}

// TODO, work on this function
bool xrsr_ws_audio_stream(xrsr_state_ws_t *ws, xrsr_src_t src) {
   if(ws == NULL) {
      XLOGD_ERROR("NULL xrsr_state_ws_t");
      return(false);
   } else if(ws->obj_ctx == NULL) {
      XLOGD_ERROR("NULL context");
      return(false);
   }

   ws->audio_src = src;

   // Continue streaming audio to the websocket
   int pipe_fd_read = -1;
   if(!xrsr_speech_stream_begin(ws->uuid, ws->audio_src, ws->dst_index, ws->xraudio_format, ws->user_initiated, &pipe_fd_read)) {
      XLOGD_ERROR("xrsr_speech_stream_begin failed");
      // perform clean up of the session
      xrsr_ws_speech_session_end(ws, XRSR_SESSION_END_REASON_ERROR_AUDIO_BEGIN);
      return(false);
   }

   ws->audio_pipe_fd_read = pipe_fd_read;

   char uuid_str[37] = {'\0'};
   uuid_unparse_lower(ws->uuid, uuid_str);
   xrsr_session_stream_begin(ws->uuid, uuid_str, ws->audio_src, ws->dst_index);

   return(true);
}

int xrsr_ws_read_pending(xrsr_state_ws_t *ws) {
   if(ws == NULL) {
      XLOGD_ERROR("NULL xrsr_state_ws_t");
      return(-1);
   }

   noPollMsg *msg = nopoll_conn_get_msg(ws->obj_conn);

   if(msg == NULL) {
      XLOGD_DEBUG("nopoll_conn_get_msg returned NULL");
   } else {
      xrsr_ws_on_msg(ws, ws->obj_conn, msg);
   }

   return(0);
}

int xrsr_ws_send_binary(xrsr_state_ws_t *ws, const uint8_t *buffer, uint32_t length) {
   if(ws == NULL) {
      XLOGD_ERROR("NULL xrsr_state_ws_t");
      return(-1);
   } else if(!xrsr_ws_is_established(ws)) {
      XLOGD_ERROR("invalid state");
      return(-1);
   }
   XLOGD_DEBUG("length <%u>", length);
   errno = 0;
   int rc = nopoll_conn_send_binary(ws->obj_conn, (const char *)buffer, (long)length);
   if(rc <= 0) { // failure found
      int errsv = errno;
      XLOGD_ERROR("websocket failure <%d>, errno (%d) <%s>, setting ws->socket = -1;", rc, errsv, strerror(errsv));
      ws->socket = -1;
   } else if(rc != length) { // partial bytes sent
      XLOGD_ERROR("websocket size mismatch req <%u> sent <%d>", length, rc);
   }
   return rc;
}

int xrsr_ws_send_text(xrsr_state_ws_t *ws, const uint8_t *buffer, uint32_t length) {
   if(ws == NULL) {
      XLOGD_ERROR("NULL xrsr_state_ws_t");
      return(-1);
   } else if(!xrsr_ws_is_established(ws)) {
      XLOGD_ERROR("invalid state");
      return(-1);
   }
   XLOGD_DEBUG("length <%u>", length);
   errno = 0;
   bool ret = xrsr_ws_queue_msg_out(ws, (const char *)buffer, length);
   return (ret ? 1 : 0);
}

void xrsr_ws_on_msg(xrsr_state_ws_t *ws, noPollConn *conn, noPollMsg *msg) {
   XLOGD_INFO("");
   xrsr_recv_msg_t msg_type      = XRSR_RECV_MSG_INVALID;
   xrsr_recv_event_t recv_event  = XRSR_RECV_EVENT_NONE;

   // Check if we are building up a message
   if(ws->pending_msg != NULL && nopoll_msg_is_final(msg) == nopoll_true) {
      XLOGD_INFO("Final Fragment received");
      ws->pending_msg = nopoll_msg_join(ws->pending_msg, msg);
      nopoll_msg_unref(msg);
      msg = ws->pending_msg;
      ws->pending_msg = NULL;
   } else if(nopoll_msg_is_fragment(msg) == nopoll_true) {
      XLOGD_INFO("Fragment received");
      ws->pending_msg = nopoll_msg_join(ws->pending_msg, msg);
      nopoll_msg_unref(msg);
      return;
   }

   noPollOpCode opcode = nopoll_msg_opcode(msg);
   switch(opcode) {
      case NOPOLL_TEXT_FRAME: {
         msg_type = XRSR_RECV_MSG_TEXT;
         break;
      }
      case NOPOLL_BINARY_FRAME: {
         msg_type = XRSR_RECV_MSG_BINARY;
         break;
      }
      default: {
         XLOGD_ERROR("invalid opcode <%s>", xrsr_ws_opcode_str(opcode));
         break;
      }
   }
   if(msg_type == XRSR_RECV_MSG_INVALID) {
      return;
   }
   const unsigned char *payload = nopoll_msg_get_payload(msg);
   int size = nopoll_msg_get_payload_size(msg);

   xrsr_ws_event(ws, SM_EVENT_MSG_RECV, false);

   // Call recv msg handler
   if(ws->handlers.recv_msg == NULL) {
      XLOGD_ERROR("recv msg handler not available");
   } else {
      if((*ws->handlers.recv_msg)(ws->handlers.data, msg_type, payload, size, &recv_event)) {
         // Close the connection
         xrsr_ws_event(ws, SM_EVENT_APP_CLOSE, false);
      }
   }
   nopoll_msg_unref(msg);

  if((unsigned int)recv_event < XRSR_RECV_EVENT_NONE) {
     ws->stream_end_reason  = (recv_event == XRSR_RECV_EVENT_EOS_SERVER ? XRSR_STREAM_END_REASON_AUDIO_EOF : XRSR_STREAM_END_REASON_DISCONNECT_REMOTE);
     ws->session_end_reason = (recv_event == XRSR_RECV_EVENT_EOS_SERVER ? XRSR_SESSION_END_REASON_EOS      : XRSR_SESSION_END_REASON_ERROR_WS_SEND);
     XLOGD_INFO("recv_event %s", xrsr_recv_event_str(recv_event));
     xrsr_ws_event(ws, SM_EVENT_EOS_PIPE, true);
  }
}

void xrsr_ws_on_close(noPollCtx *ctx, noPollConn *conn, noPollPtr user_data) {
   xrsr_state_ws_t *ws = (xrsr_state_ws_t *)user_data;
   XLOGD_INFO("");

   ws->on_close = true;
   if(ws->pending_msg) { // This shouldn't ever happen, but for sanity.
      nopoll_msg_unref(ws->pending_msg);
      ws->pending_msg = NULL;
   }
   xrsr_ws_event(ws, SM_EVENT_WS_CLOSE, false);
}

void xrsr_ws_speech_stream_end(xrsr_state_ws_t *ws, xrsr_stream_end_reason_t reason, bool detect_resume) {
   XLOGD_INFO("fd <%d> reason <%s>", ws->audio_pipe_fd_read, xrsr_stream_end_reason_str(reason));

   xrsr_speech_stream_end(ws->uuid, ws->audio_src, ws->dst_index, reason, detect_resume, &ws->audio_stats);

   if(ws->audio_pipe_fd_read >= 0) {
      close(ws->audio_pipe_fd_read);
      ws->audio_pipe_fd_read = -1;
   }
}

void xrsr_ws_speech_session_end(xrsr_state_ws_t *ws, xrsr_session_end_reason_t reason) {
   XLOGD_INFO("fd <%d> reason <%s>", ws->audio_pipe_fd_read, xrsr_session_end_reason_str(reason));

   ws->stats.reason = reason;

   char uuid_str[37] = {'\0'};
   uuid_unparse_lower(ws->uuid, uuid_str);
   xrsr_session_end(ws->uuid, uuid_str, ws->audio_src, ws->dst_index, &ws->stats);
}

const char *xrsr_ws_opcode_str(noPollOpCode type) {
   switch(type) {
      case NOPOLL_UNKNOWN_OP_CODE:    return("UNKNOWN_OP_CODE");
      case NOPOLL_CONTINUATION_FRAME: return("CONTINUATION_FRAME");
      case NOPOLL_TEXT_FRAME:         return("TEXT_FRAME");
      case NOPOLL_BINARY_FRAME:       return("BINARY_FRAME");
      case NOPOLL_CLOSE_FRAME:        return("CLOSE_FRAME");
      case NOPOLL_PING_FRAME:         return("PING_FRAME");
      case NOPOLL_PONG_FRAME:         return("PONG_FRAME");
   }
   return("INVALID");
}

void xrsr_ws_handle_speech_event(xrsr_state_ws_t *ws, xrsr_speech_event_t *event) {
   if(NULL == event) {
      XLOGD_ERROR("speech event is NULL");
      return;
   }

   switch(event->event) {
      case XRSR_EVENT_EOS: {
         xrsr_ws_event(ws, SM_EVENT_EOS, false);
         break;
      }
      case XRSR_EVENT_STREAM_KWD_INFO: {
         ws->audio_kwd_notified = false;
         ws->audio_kwd_bytes    = event->data.byte_qty;
         break;
      }
      case XRSR_EVENT_STREAM_TIME_MINIMUM: {
         ws->stream_time_min_rxd = true;
         xrsr_ws_event(ws, SM_EVENT_STM, false);
         break;
      }
      default: {
         XLOGD_WARN("unhandled speech event <%s>", xrsr_event_str(event->event));
         break;
      }
   }
}

bool xrsr_ws_queue_msg_out(xrsr_state_ws_t *ws, const char *msg, uint32_t length) {
   bool ret = false;
   sem_wait(&ws->msg_out_semaphore);
   if(ws->msg_out_count < XRSR_WS_MSG_OUT_MAX) {
      uint32_t buf_len = length + 1;
      ws->msg_out[ws->msg_out_count] = (char *)malloc(sizeof(char) * buf_len);
      if(ws->msg_out[ws->msg_out_count] == NULL) {
         XLOGD_ERROR("failed to allocate msg_out buffer");
      } else {
         snprintf(ws->msg_out[ws->msg_out_count], buf_len, "%s", msg);
         ws->msg_out_count++;
         ret = true;
      }
   }
   sem_post(&ws->msg_out_semaphore);
   return(ret);
}

bool xrsr_ws_is_msg_out(xrsr_state_ws_t *ws) {
   bool ret = false;
   sem_wait(&ws->msg_out_semaphore);
   ret = (ws->msg_out_count > 0);
   sem_post(&ws->msg_out_semaphore);
   return(ret);
}

bool xrsr_ws_get_msg_out(xrsr_state_ws_t *ws, char **msg, uint32_t *length) {
   bool ret = false;
   if(msg != NULL && length != NULL) {
      sem_wait(&ws->msg_out_semaphore);
      if(ws->msg_out_count > 0) {
         uint8_t i = 0;

         *msg    = ws->msg_out[0];
         *length = strlen(*msg);
         ret     = true;

         // Clean up
         ws->msg_out_count--;
         for(i = 0; i < ws->msg_out_count; i++) {
            ws->msg_out[i] = ws->msg_out[i+1];
         }
         ws->msg_out[i] = NULL;
      } else {
         XLOGD_WARN("No outgoing messages available");
      }
      sem_post(&ws->msg_out_semaphore);
   } else {
      XLOGD_ERROR("NULL parameters");
   }
   return(ret);
}

void xrsr_ws_clear_msg_out(xrsr_state_ws_t *ws) {
   uint8_t i = 0;
   sem_wait(&ws->msg_out_semaphore);
   for(i = 0; i < XRSR_WS_MSG_OUT_MAX; i++) {
      if(ws->msg_out[i] != NULL) {
         free(ws->msg_out[i]);
         ws->msg_out[i] = NULL;
      }
   }
   ws->msg_out_count = 0;
   sem_post(&ws->msg_out_semaphore);
}

void xrsr_ws_reset(xrsr_state_ws_t *ws) {
   if(ws) {
      ws->socket                = -1;
      ws->timer_id              = RDXK_TIMER_ID_INVALID;
      ws->audio_src             = XRSR_SRC_INVALID;
      ws->write_pending_bytes   = false;
      ws->write_pending_retries = 0;
      ws->detect_resume         = true;
      ws->on_close              = false;
      ws->retry_cnt             = 1;
      ws->is_session_by_text    = false;
      if(ws->audio_pipe_fd_read > -1) {
         close(ws->audio_pipe_fd_read);
         ws->audio_pipe_fd_read = -1;
      }
      xrsr_ws_clear_msg_out(ws);
   }
}

void xrsr_ws_sm_init(xrsr_state_ws_t *ws) {
   if(ws) {
      ws->state_machine.mInstanceName = "wsSM";
      ws->state_machine.bInitFinished = false;

      ws->state_machine.bInitFinished = FALSE; 
      ws->state_machine.activeEvtQueue.mpQData = ws->state_machine_events_active; 
      ws->state_machine.activeEvtQueue.mQSize = XRSR_WS_SM_EVENTS_MAX; 
      ws->state_machine.deferredEvtQueue.mpQData = NULL; 
      ws->state_machine.deferredEvtQueue.mQSize = 0;
    
      SmInit( &ws->state_machine, &St_Ws_Disconnected_Info );
   }
}

void xrsr_ws_event(xrsr_state_ws_t *ws, tStEventID id, bool from_state_handler) {
   if(ws) {
      SmEnqueueEvent(&ws->state_machine, id, (void *)ws);
      if(!from_state_handler) {
         SmProcessEvents(&ws->state_machine);
      }
   }
}

void St_Ws_Disconnected(tStateEvent *pEvent, eStateAction eAction, BOOL *bGuardResponse) {
   xrsr_state_ws_t *ws = (xrsr_state_ws_t *)pEvent->mData;
   switch(eAction) {
      case ACT_GUARD: {
         if(bGuardResponse) {
            *bGuardResponse = true;
         }
         break;
      }
      case ACT_ENTER: {
         rdkx_timestamp_t timestamp;
         rdkx_timestamp_get_realtime(&timestamp);
         if(ws->handlers.disconnected == NULL) {
            XLOGD_INFO("disconnected handler not available");
         } else {
            (*ws->handlers.disconnected)(ws->handlers.data, ws->uuid, ws->session_end_reason, false, &ws->detect_resume, &timestamp);
         }
         xrsr_ws_speech_session_end(ws, ws->session_end_reason);
         xrsr_ws_reset(ws);
         break;
      }
      default: {
         break;
      }
   }
}

void St_Ws_Disconnecting(tStateEvent *pEvent, eStateAction eAction, BOOL *bGuardResponse) {
   xrsr_state_ws_t *ws = (xrsr_state_ws_t *)pEvent->mData;
   switch(eAction) {
      case ACT_GUARD: {
         if(bGuardResponse) {
            *bGuardResponse = true;
         }
         break;
      }
      case ACT_ENTER: {
         if(ws->obj_conn != NULL) {
            // Remove on_close handler
            nopoll_conn_set_on_close(ws->obj_conn, NULL, NULL);

            // only call close if network is available
            XLOG_DEBUG("nopoll ref count %d, should be 2...", nopoll_conn_ref_count(ws->obj_conn));
            if(ws->on_close == false) {
               nopoll_conn_close(ws->obj_conn);
            } else {
               XLOG_DEBUG("server closed the connection");
            }
            ws->obj_conn = NULL;
         }
         xrsr_ws_event(ws, SM_EVENT_DISCONNECTED, true);
         break;
      }
      default: {
         break;
      }
   }
}

void St_Ws_Buffering(tStateEvent *pEvent, eStateAction eAction, BOOL *bGuardResponse) {
   xrsr_state_ws_t *ws = (xrsr_state_ws_t *)pEvent->mData;
   switch(eAction) {
      case ACT_GUARD: {
         if(bGuardResponse) {
            *bGuardResponse = true;
         }
         break;
      }
      case ACT_EXIT: {
         switch(pEvent->mID) {
            case SM_EVENT_EOS: {
               ws->stream_end_reason  = XRSR_STREAM_END_REASON_AUDIO_EOF;
               ws->session_end_reason = XRSR_SESSION_END_REASON_ERROR_AUDIO_DURATION;
               xrsr_ws_speech_stream_end(ws, ws->stream_end_reason, ws->detect_resume);
               break;
            }
            case SM_EVENT_TERMINATE: {
               ws->stream_end_reason  = XRSR_STREAM_END_REASON_DID_NOT_BEGIN;
               ws->session_end_reason = XRSR_SESSION_END_REASON_TERMINATE;
               xrsr_ws_speech_stream_end(ws, ws->stream_end_reason, ws->detect_resume);
               break;
            }
            default: {
               break;
            }
         }
         break;
      }
      default: {
         break;
      }
   }
}

void St_Ws_Connecting(tStateEvent *pEvent, eStateAction eAction, BOOL *bGuardResponse) {
   xrsr_state_ws_t *ws = (xrsr_state_ws_t *)pEvent->mData;
   switch(eAction) {
      case ACT_GUARD: {
         if(bGuardResponse) {
            *bGuardResponse = true;
         }
         break;
      }
      case ACT_ENTER: {
         if(!xrsr_ws_connect_new(ws)) {
            rdkx_timestamp_t timestamp;
            rdkx_timestamp_get(&timestamp);
            if(rdkx_timestamp_cmp(timestamp, ws->retry_timestamp_end) >= 0) {
               xrsr_ws_event(ws, SM_EVENT_CONNECT_TIMEOUT, true);
            } else {
               xrsr_ws_event(ws, SM_EVENT_RETRY, true);
            }
         } else {
            rdkx_timestamp_t timeout;
            rdkx_timestamp_get(&timeout);
            rdkx_timestamp_add_ms(&timeout, ws->connect_check_interval);

            ws->timer_id = rdkx_timer_insert(ws->timer_obj, timeout, xrsr_ws_process_timeout, ws);
         }
         break;
      }
      case ACT_INTERNAL: {
         switch(pEvent->mID) {
            case SM_EVENT_TIMEOUT: {
               if(!nopoll_conn_is_ok(ws->obj_conn)) {
                  if(ws->connect_wait_time <= 0) { // overall timeout reached
                     rdkx_timestamp_t timestamp;
                     rdkx_timestamp_get(&timestamp);
                     if(rdkx_timestamp_cmp(timestamp, ws->retry_timestamp_end) >= 0) {
                        xrsr_ws_event(ws, SM_EVENT_CONNECT_TIMEOUT, true);
                     } else {
                        xrsr_ws_event(ws, SM_EVENT_RETRY, true);
                     }
                  } else { // Set next timeout
                     rdkx_timestamp_t timeout;
                     rdkx_timestamp_get(&timeout);
                     rdkx_timestamp_add_ms(&timeout, ws->connect_check_interval);
                     ws->connect_wait_time -= ws->connect_check_interval;

                     if(ws->timer_obj && ws->timer_id >= 0) {
                        if(!rdkx_timer_update(ws->timer_obj, ws->timer_id, timeout)) {
                           XLOGD_ERROR("timer update");
                        }
                     }
                  }
               } else {
                  xrsr_ws_event(ws, SM_EVENT_CONNECTED, true);
               }
               break;
            }
            default: {
               break;
            }
         }
         break;
      }
      case ACT_EXIT: {
         switch(pEvent->mID) {
            case SM_EVENT_XRSR_ERROR: {
               //TODO
               break;
            }
            case SM_EVENT_CONNECT_TIMEOUT: {
               // After attempting to connect until connect timeout, we failed. Consider this a failure.
               ws->stream_end_reason  = XRSR_STREAM_END_REASON_DID_NOT_BEGIN;
               ws->session_end_reason = XRSR_SESSION_END_REASON_ERROR_CONNECT_FAILURE;
               xrsr_ws_speech_stream_end(ws, ws->stream_end_reason, ws->detect_resume);
               break;
            }
            case SM_EVENT_TERMINATE: {
               ws->stream_end_reason  = XRSR_STREAM_END_REASON_DID_NOT_BEGIN;
               ws->session_end_reason = XRSR_SESSION_END_REASON_TERMINATE;
               xrsr_ws_speech_stream_end(ws, ws->stream_end_reason, ws->detect_resume);
               break;
            }
            default: {
               break;
            }
         }
         if(ws->timer_obj != NULL && ws->timer_id >= 0) {
            if(!rdkx_timer_remove(ws->timer_obj, ws->timer_id)) {
               XLOGD_ERROR("timer remove");
            }
            ws->timer_id = RDXK_TIMER_ID_INVALID;
         }
         break;
      }
      default: {
         break;
      }
   }
}

void St_Ws_Connected(tStateEvent *pEvent, eStateAction eAction, BOOL *bGuardResponse) {
   xrsr_state_ws_t *ws = (xrsr_state_ws_t *)pEvent->mData;
   switch(eAction) {
      case ACT_GUARD: {
         if(bGuardResponse) {
            *bGuardResponse = true;
         }
         break;
      }
      case ACT_ENTER: {
         rdkx_timestamp_t timeout;
         rdkx_timestamp_get(&timeout);
         rdkx_timestamp_add_ms(&timeout, ws->connect_check_interval);

         ws->timer_id = rdkx_timer_insert(ws->timer_obj, timeout, xrsr_ws_process_timeout, ws);
         ws->connect_wait_time = ws->timeout_connect;
         break;
      }
      case ACT_INTERNAL: {
         switch(pEvent->mID) {
            case SM_EVENT_TIMEOUT: {
               if(!xrsr_ws_conn_is_ready(ws)) {
                  XLOGD_WARN("websocket is not ready");
                  if(ws->connect_wait_time <= 0) {
                     XLOGD_ERROR("server hang on HTTP upgrade request");
                     xrsr_ws_event(ws, SM_EVENT_ESTABLISH_TIMEOUT, true);
                  } else {
                     rdkx_timestamp_t timeout;
                     rdkx_timestamp_get(&timeout);
                     rdkx_timestamp_add_ms(&timeout, ws->connect_check_interval);
                     ws->connect_wait_time -= ws->connect_check_interval;

                     if(ws->timer_obj && ws->timer_id >= 0) {
                        if(!rdkx_timer_update(ws->timer_obj, ws->timer_id, timeout)) {
                           XLOGD_ERROR("timer update");
                        }
                     }
                  }
               } else {
                  xrsr_ws_event(ws, SM_EVENT_ESTABLISHED, true);
               }
               break;
            }
            default: {
               break;
            }
         }
         break;
      }
      case ACT_EXIT: {
         switch(pEvent->mID) {
            case SM_EVENT_WS_CLOSE: {
               ws->stream_end_reason  = XRSR_STREAM_END_REASON_DISCONNECT_REMOTE;
               ws->session_end_reason = XRSR_SESSION_END_REASON_ERROR_CONNECT_FAILURE;
               xrsr_ws_speech_stream_end(ws, ws->stream_end_reason, ws->detect_resume);
               break;
            }
            case SM_EVENT_ESTABLISH_TIMEOUT: {
               ws->stream_end_reason  = XRSR_STREAM_END_REASON_DID_NOT_BEGIN;
               ws->session_end_reason = XRSR_SESSION_END_REASON_ERROR_CONNECT_TIMEOUT;
               xrsr_ws_speech_stream_end(ws, ws->stream_end_reason, ws->detect_resume);
               break;
            }
            case SM_EVENT_TERMINATE: {
               ws->stream_end_reason  = XRSR_STREAM_END_REASON_DID_NOT_BEGIN;
               ws->session_end_reason = XRSR_SESSION_END_REASON_TERMINATE;
               xrsr_ws_speech_stream_end(ws, ws->stream_end_reason, ws->detect_resume);
               break;
            }
            default: {
               break;
            }
         }
         if(ws->timer_obj != NULL && ws->timer_id >= 0) {
            if(!rdkx_timer_remove(ws->timer_obj, ws->timer_id)) {
               XLOGD_ERROR("timer remove");
            }
            ws->timer_id = RDXK_TIMER_ID_INVALID;
         }
         break;
      }
      default: {
         break;
      }
   }
}

void St_Ws_Established(tStateEvent *pEvent, eStateAction eAction, BOOL *bGuardResponse) {
   xrsr_state_ws_t *ws = (xrsr_state_ws_t *)pEvent->mData;
   switch(eAction) {
      case ACT_GUARD: {
         if(bGuardResponse) {
            *bGuardResponse = true;
         }
         break;
      }
      case ACT_ENTER: {
         // Update the timer for connection message monitoring
         rdkx_timestamp_t timeout;
         rdkx_timestamp_get(&timeout);
         rdkx_timestamp_add_ms(&timeout, ws->timeout_inactivity);

         ws->timer_id = rdkx_timer_insert(ws->timer_obj, timeout, xrsr_ws_process_timeout, ws);
         break;
      }
      case ACT_INTERNAL: {
         switch(pEvent->mID) {
            case SM_EVENT_MSG_RECV: {
               rdkx_timestamp_t timeout;
               rdkx_timestamp_get(&timeout);
               rdkx_timestamp_add_ms(&timeout, ws->timeout_inactivity);
               if(ws->timer_obj && ws->timer_id >= 0) {
                  if(!rdkx_timer_update(ws->timer_obj, ws->timer_id, timeout)) {
                     XLOGD_ERROR("timer update");
                  }
               }
               break;
            }
            default: {
               break;
            }
         }
         break;
      }
      case ACT_EXIT: {
         switch(pEvent->mID) {
            case SM_EVENT_APP_CLOSE: {
               ws->session_end_reason = XRSR_SESSION_END_REASON_EOS;
               break;
            }
            case SM_EVENT_TERMINATE: {
               ws->session_end_reason = XRSR_SESSION_END_REASON_TERMINATE;
               break;
            }
            case SM_EVENT_TIMEOUT: {
               ws->session_end_reason = XRSR_SESSION_END_REASON_ERROR_SESSION_TIMEOUT;
               break;
            }
            case SM_EVENT_WS_CLOSE: {
               ws->session_end_reason = XRSR_SESSION_END_REASON_EOS;
               break;
            }
            default: {
               break;
            }
         }
         if(ws->timer_obj != NULL && ws->timer_id >= 0) {
            if(!rdkx_timer_remove(ws->timer_obj, ws->timer_id)) {
               XLOGD_ERROR("timer remove");
            }
            ws->timer_id = RDXK_TIMER_ID_INVALID;
         }
         break;
      }
      default: {
         break;
      }
   }
}

void St_Ws_Streaming(tStateEvent *pEvent, eStateAction eAction, BOOL *bGuardResponse) {
   xrsr_state_ws_t *ws = (xrsr_state_ws_t *)pEvent->mData;
   switch(eAction) {
      case ACT_GUARD: {
         if(bGuardResponse) {
            *bGuardResponse = true;
         }
         break;
      }
      case ACT_ENTER: {
         bool success = false;
         // Call connected handler
         if(ws->handlers.connected == NULL) {
            XLOGD_INFO("connected handler not available");
         } else {
            rdkx_timestamp_t timestamp;
            rdkx_timestamp_get_realtime(&timestamp);
            success = (*ws->handlers.connected)(ws->handlers.data, ws->uuid, xrsr_conn_send, (void *)ws, &timestamp);
         }

         char uuid_str[37] = {'\0'};
         uuid_unparse_lower(ws->uuid, uuid_str);
         xrsr_session_stream_begin(ws->uuid, uuid_str, ws->audio_src, ws->dst_index);

         if (success && ws->is_session_by_text) {
            xrsr_ws_event(ws, SM_EVENT_TEXT_SESSION_SUCCESS, true);
         }
         break;
      }
      case ACT_EXIT: {
         switch(pEvent->mID) {
            case SM_EVENT_EOS_PIPE: {
               ws->stream_end_reason  = XRSR_STREAM_END_REASON_AUDIO_EOF;
               ws->session_end_reason = XRSR_SESSION_END_REASON_EOS;
               break;
            }
            case SM_EVENT_TERMINATE: {
               ws->stream_end_reason  = XRSR_STREAM_END_REASON_DISCONNECT_LOCAL;
               ws->session_end_reason = XRSR_SESSION_END_REASON_TERMINATE;
               break;
            }
            case SM_EVENT_ESTABLISH_TIMEOUT: {
               ws->stream_end_reason  = XRSR_STREAM_END_REASON_DID_NOT_BEGIN;
               ws->session_end_reason = XRSR_SESSION_END_REASON_ERROR_CONNECT_TIMEOUT;
               break;
            }
            case SM_EVENT_AUDIO_ERROR: {
               ws->stream_end_reason  = XRSR_STREAM_END_REASON_ERROR_AUDIO_READ;
               ws->session_end_reason = XRSR_SESSION_END_REASON_EOS;
               break;
            }
            case SM_EVENT_WS_ERROR: {
               ws->stream_end_reason  = XRSR_STREAM_END_REASON_DISCONNECT_REMOTE;
               ws->session_end_reason = XRSR_SESSION_END_REASON_ERROR_WS_SEND;
               break;
            }
            case SM_EVENT_WS_CLOSE: {
               ws->stream_end_reason  = XRSR_STREAM_END_REASON_DISCONNECT_REMOTE;
               ws->session_end_reason = XRSR_SESSION_END_REASON_ERROR_WS_SEND;
               break;
            }
            case SM_EVENT_TEXT_SESSION_SUCCESS: {
               XLOGD_INFO("SM_EVENT_TEXT_SESSION_SUCCESS - text-only session init message sent successfully.");
               break;
            }
            default: {
               break;
            }
         }
         if (pEvent->mID != SM_EVENT_TEXT_SESSION_SUCCESS) {
            xrsr_ws_speech_stream_end(ws, ws->stream_end_reason, ws->detect_resume);
         }
         break;
      }
      default: {
         break;
      }
   }
}

void St_Ws_TextOnlySession(tStateEvent *pEvent, eStateAction eAction, BOOL *bGuardResponse) {
   xrsr_state_ws_t *ws = (xrsr_state_ws_t *)pEvent->mData;
   switch(eAction) {
      case ACT_GUARD: {
         if(bGuardResponse) {
            *bGuardResponse = true;
         }
         break;
      }
      case ACT_ENTER: {
         break;
      }
      case ACT_EXIT: {
         switch(pEvent->mID) {
            case SM_EVENT_EOS_PIPE: {
               ws->stream_end_reason  = XRSR_STREAM_END_REASON_AUDIO_EOF;
               ws->session_end_reason = XRSR_SESSION_END_REASON_EOS;
               break;
            }
            case SM_EVENT_TERMINATE: {
               ws->stream_end_reason  = XRSR_STREAM_END_REASON_DISCONNECT_LOCAL;
               ws->session_end_reason = XRSR_SESSION_END_REASON_TERMINATE;
               break;
            }
            case SM_EVENT_ESTABLISH_TIMEOUT: {
               ws->stream_end_reason  = XRSR_STREAM_END_REASON_DID_NOT_BEGIN;
               ws->session_end_reason = XRSR_SESSION_END_REASON_ERROR_CONNECT_TIMEOUT;
               break;
            }
            case SM_EVENT_AUDIO_ERROR: {
               ws->stream_end_reason  = XRSR_STREAM_END_REASON_ERROR_AUDIO_READ;
               ws->session_end_reason = XRSR_SESSION_END_REASON_EOS;
               break;
            }
            case SM_EVENT_WS_ERROR: {
               ws->stream_end_reason  = XRSR_STREAM_END_REASON_DISCONNECT_REMOTE;
               ws->session_end_reason = XRSR_SESSION_END_REASON_ERROR_WS_SEND;
               break;
            }
            case SM_EVENT_WS_CLOSE: {
               ws->stream_end_reason  = XRSR_STREAM_END_REASON_INVALID;
               ws->session_end_reason = XRSR_SESSION_END_REASON_EOT;
               break;
            }
            default: {
               break;
            }
         }
         xrsr_ws_speech_stream_end(ws, ws->stream_end_reason, ws->detect_resume);
         break;
      }
      default: {
         break;
      }
   }
}

void St_Ws_Connection_Retry(tStateEvent *pEvent, eStateAction eAction, BOOL *bGuardResponse) {
   xrsr_state_ws_t *ws = (xrsr_state_ws_t *)pEvent->mData;
   switch(eAction) {
      case ACT_GUARD: {
         if(bGuardResponse) {
            *bGuardResponse = true;
         }
         break;
      }
      case ACT_ENTER: {
         ws->retry_cnt++;
         // Calculate retry delay
         uint32_t slots = 1 << ws->retry_cnt;
         uint32_t retry_delay_ms = ws->backoff_delay * (rand() % slots);

         XLOGD_INFO("retry connection - delay <%u> ms", retry_delay_ms);

         rdkx_timestamp_t timeout;
         rdkx_timestamp_get(&timeout);
         rdkx_timestamp_add_ms(&timeout, retry_delay_ms);

         if(rdkx_timestamp_cmp(timeout, ws->retry_timestamp_end) > 0) {
            timeout = ws->retry_timestamp_end;
         }

         ws->timer_id = rdkx_timer_insert(ws->timer_obj, timeout, xrsr_ws_process_timeout, ws);
         break;
      }
      case ACT_EXIT: {
         switch(pEvent->mID) {
            case SM_EVENT_TERMINATE: {
               ws->stream_end_reason  = XRSR_STREAM_END_REASON_DISCONNECT_LOCAL;
               ws->session_end_reason = XRSR_SESSION_END_REASON_TERMINATE;
               xrsr_ws_speech_stream_end(ws, ws->stream_end_reason, ws->detect_resume);
               break;
            }
            default: {
               break;
            }
         }
         if(ws->timer_obj != NULL && ws->timer_id >= 0) {
            if(!rdkx_timer_remove(ws->timer_obj, ws->timer_id)) {
               XLOGD_ERROR("timer remove");
            }
            ws->timer_id = RDXK_TIMER_ID_INVALID;
         }
         break;
      }
      default: {
         break;
      }
   }
}

bool xrsr_ws_is_established(xrsr_state_ws_t *ws) {
   bool ret = false;
   if(ws) {
      if(SmInThisState(&ws->state_machine, &St_Ws_Established_Info) ||
         SmInThisState(&ws->state_machine, &St_Ws_Streaming_Info) || 
         SmInThisState(&ws->state_machine, &St_Ws_TextOnlySession_Info)) {
         ret = true;
      }
   }
   return(ret);
}

bool xrsr_ws_is_disconnected(xrsr_state_ws_t *ws) {
   bool ret = false;
   if(ws) {
      if(SmInThisState(&ws->state_machine, &St_Ws_Disconnected_Info)) {
         ret = true;
      }
   }
   return(ret);
}
