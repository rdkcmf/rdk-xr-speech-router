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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/select.h>
#include <semaphore.h>
#include <xr_mq.h>
#include <pthread.h>
#include <xrsr_private.h>
#include <xraudio.h>
#include <xrsr_version.h>

#if defined(XRSR_KEYWORD_PHRASE_HELLO_SKY)
#define XRSR_KEYWORD_PHRASE (XRAUDIO_KEYWORD_PHRASE_HELLO_SKY)
#else
#define XRSR_KEYWORD_PHRASE (XRAUDIO_KEYWORD_PHRASE_HEY_XFINITY)
#endif

typedef enum {
   XRSR_THREAD_MAIN = 0,
   XRSR_THREAD_QTY  = 1,
} xrsr_thread_t;

typedef void *(*xrsr_thread_func_t)(void *);

typedef union {
   #ifdef WS_ENABLED
   xrsr_state_ws_t     ws;
   #endif
   #ifdef HTTP_ENABLED
   xrsr_state_http_t   http;
   #endif
   #ifdef SDT_ENABLED
   xrsr_state_sdt_t  sdt;
   #endif
} xrsr_conn_state_t;

typedef struct {
   bool                         initialized;
   xrsr_url_parts_t             url_parts;
   xrsr_route_handler_t         handler;
   xrsr_handlers_t              handlers;
   xrsr_audio_format_t          format;
   uint16_t                     stream_time_min;
   xraudio_input_record_from_t  stream_from;
   int32_t                      stream_offset;
   xraudio_input_record_until_t stream_until;
   uint32_t                     keyword_begin;
   uint32_t                     keyword_duration;
   xrsr_conn_state_t            conn_state;
   xrsr_dst_param_ptrs_t        dst_param_ptrs[XRSR_POWER_MODE_INVALID];
} xrsr_dst_int_t;

typedef struct {
   xrsr_dst_int_t dsts[XRSR_DST_QTY_MAX];
} xrsr_route_int_t;

typedef struct {
   const char *       name;
   int                msgq_id;
   size_t             msgsize;
   xrsr_thread_func_t func;
   void *             params;
   pthread_t          id;
   sem_t              semaphore;
} xrsr_thread_info_t;

typedef struct {
   bool                running;
   rdkx_timer_object_t timer_obj;
} xrsr_thread_state_t;

#ifdef WS_ENABLED
typedef struct {
   bool *    ptr_debug;
   bool      val_debug;
   uint32_t *ptr_connect_check_interval;
   uint32_t  val_connect_check_interval;
   uint32_t *ptr_timeout_connect;
   uint32_t  val_timeout_connect;
   uint32_t *ptr_timeout_inactivity;
   uint32_t  val_timeout_inactivity;
   uint32_t *ptr_timeout_session;
   uint32_t  val_timeout_session;
   bool *    ptr_ipv4_fallback;
   bool      val_ipv4_fallback;
   uint32_t *ptr_backoff_delay;
   uint32_t  val_backoff_delay;
} xrsr_ws_json_config_t;
#endif

typedef struct {
   bool                          opened;
   xrsr_power_mode_t             power_mode;
   bool                          privacy_mode;
   xrsr_thread_info_t            threads[XRSR_THREAD_QTY];
   xrsr_route_int_t              routes[XRSR_SRC_INVALID];
   xrsr_xraudio_object_t         xrsr_xraudio_object;
   char *                        capture_dir_path;
   xrsr_src_t                    src;
   bool                          first_stream_req;              // the first stream request sets up pipes for all requests
   int                           pipe_fds_rd[XRSR_DST_QTY_MAX]; // cache the read side of the pipes since the stream requests
   #ifdef WS_ENABLED
   xrsr_ws_json_config_t         *ws_json_config;
   xrsr_ws_json_config_t          ws_json_config_fpm;
   xrsr_ws_json_config_t          ws_json_config_lpm;
   #endif
} xrsr_global_t;

static void xrsr_session_stream_kwd(const uuid_t uuid, const char *uuid_str, xrsr_src_t src, uint32_t dst_index);
static void xrsr_session_stream_end(const uuid_t uuid, const char *uuid_str, xrsr_src_t src, uint32_t dst_index, xrsr_stream_stats_t *stats);

typedef void (*xrsr_msg_handler_t)(const xrsr_thread_params_t *params, xrsr_thread_state_t *state, void *msg);

static void xrsr_msg_terminate            (const xrsr_thread_params_t *params, xrsr_thread_state_t *state, void *msg);
static void xrsr_msg_route_update         (const xrsr_thread_params_t *params, xrsr_thread_state_t *state, void *msg);
static void xrsr_msg_keyword_update       (const xrsr_thread_params_t *params, xrsr_thread_state_t *state, void *msg);
static void xrsr_msg_host_name_update     (const xrsr_thread_params_t *params, xrsr_thread_state_t *state, void *msg);
static void xrsr_msg_power_mode_update    (const xrsr_thread_params_t *params, xrsr_thread_state_t *state, void *msg);
static void xrsr_msg_privacy_mode_update  (const xrsr_thread_params_t *params, xrsr_thread_state_t *state, void *msg);
static void xrsr_msg_privacy_mode_get     (const xrsr_thread_params_t *params, xrsr_thread_state_t *state, void *msg);
static void xrsr_msg_xraudio_granted      (const xrsr_thread_params_t *params, xrsr_thread_state_t *state, void *msg);
static void xrsr_msg_xraudio_revoked      (const xrsr_thread_params_t *params, xrsr_thread_state_t *state, void *msg);
static void xrsr_msg_xraudio_event        (const xrsr_thread_params_t *params, xrsr_thread_state_t *state, void *msg);
static void xrsr_msg_keyword_detected     (const xrsr_thread_params_t *params, xrsr_thread_state_t *state, void *msg);
static void xrsr_msg_keyword_detect_error (const xrsr_thread_params_t *params, xrsr_thread_state_t *state, void *msg);
static void xrsr_msg_session_begin        (const xrsr_thread_params_t *params, xrsr_thread_state_t *state, void *msg);
static void xrsr_msg_session_terminate    (const xrsr_thread_params_t *params, xrsr_thread_state_t *state, void *msg);
static void xrsr_msg_session_capture_start(const xrsr_thread_params_t *params, xrsr_thread_state_t *state, void *msg);
static void xrsr_msg_session_capture_stop (const xrsr_thread_params_t *params, xrsr_thread_state_t *state, void *msg);
static void xrsr_msg_thread_poll          (const xrsr_thread_params_t *params, xrsr_thread_state_t *state, void *msg);

static const xrsr_msg_handler_t g_xrsr_msg_handlers[XRSR_QUEUE_MSG_TYPE_INVALID] = {
   xrsr_msg_terminate,
   xrsr_msg_route_update,
   xrsr_msg_keyword_update,
   xrsr_msg_host_name_update,
   xrsr_msg_power_mode_update,
   xrsr_msg_privacy_mode_update,
   xrsr_msg_privacy_mode_get,
   xrsr_msg_xraudio_granted,
   xrsr_msg_xraudio_revoked,
   xrsr_msg_xraudio_event,
   xrsr_msg_keyword_detected,
   xrsr_msg_keyword_detect_error,
   xrsr_msg_session_begin,
   xrsr_msg_session_terminate,
   xrsr_msg_session_capture_start,
   xrsr_msg_session_capture_stop,
   xrsr_msg_thread_poll,
};

static xrsr_global_t g_xrsr;

static bool xrsr_threads_init(bool is_prod);
static void xrsr_threads_term(void);
static void *xrsr_thread_main(void *param);
static void xrsr_route_free_all(void);
static void xrsr_route_free(xrsr_src_t src);
static void xrsr_route_update(const char *host_name, const xrsr_route_t *route, xrsr_thread_state_t *state);

static xrsr_audio_format_t xrsr_audio_format_get(xrsr_audio_format_t format_dst, xraudio_input_format_t format_src);

void xrsr_version(xrsr_version_info_t *version_info, uint32_t *qty) {
   if(qty == NULL || *qty < XRSR_VERSION_QTY_MAX || version_info == NULL) {
      return;
   }
   uint32_t qty_avail = *qty;

   version_info->name      = "xr-speech-router";
   version_info->version   = XRSR_VERSION;
   version_info->branch    = XRSR_BRANCH;
   version_info->commit_id = XRSR_COMMIT_ID;
   version_info++;
   qty_avail--;

   const char *name      = NULL;
   const char *version   = NULL;
   const char *branch    = NULL;
   const char *commit_id = NULL;

   xr_mq_version(&name, &version, &branch, &commit_id);

   version_info->name      = name;
   version_info->version   = version;
   version_info->branch    = branch;
   version_info->commit_id = commit_id;
   version_info++;
   qty_avail--;

   rdkx_timer_version_info_t timer_version_info[RDKX_TIMER_VERSION_QTY];
   memset(timer_version_info, 0, sizeof(timer_version_info));

   rdkx_timer_version(timer_version_info, RDKX_TIMER_VERSION_QTY);

   for(uint32_t index = 0; index < RDKX_TIMER_VERSION_QTY; index++) {
      rdkx_timer_version_info_t *entry = &timer_version_info[index];
      version_info->name      = entry->name;
      version_info->version   = entry->version;
      version_info->branch    = entry->branch;
      version_info->commit_id = entry->commit_id;
      version_info++;
      qty_avail--;
   }

   xraudio_version_info_t xraudio_version_info[XRAUDIO_VERSION_QTY_MAX];
   memset(xraudio_version_info, 0, sizeof(xraudio_version_info));

   uint32_t qty_xraudio = qty_avail;
   xraudio_version(xraudio_version_info, &qty_xraudio);

   for(uint32_t index = 0; index < qty_xraudio; index++) {
      xraudio_version_info_t *entry = &xraudio_version_info[index];
      version_info->name      = entry->name;
      version_info->version   = entry->version;
      version_info->branch    = entry->branch;
      version_info->commit_id = entry->commit_id;
      version_info++;
      qty_avail--;
   }
   *qty -= qty_avail;
}

bool xrsr_open(const char *host_name, const xrsr_route_t routes[], const xrsr_keyword_config_t *keyword_config, const xrsr_capture_config_t *capture_config, xrsr_power_mode_t power_mode, bool privacy_mode, const json_t *json_obj_vsdk) {
   json_t *json_obj_xraudio = NULL;
   if(g_xrsr.opened) {
      XLOGD_ERROR("already open");
      return(false);
   }
   if(routes == NULL) {
      XLOGD_ERROR("invalid parameter");
      return(false);
   }
   if(keyword_config != NULL && keyword_config->sensitivity >= XRAUDIO_KEYWORD_CONFIG_INVALID) {
      XLOGD_ERROR("invalid keyword config - sensitivity <%u>", keyword_config->sensitivity);
      return(false);
   }
   if((uint32_t)power_mode >= XRSR_POWER_MODE_INVALID) {
      XLOGD_ERROR("invalid power mode <%s>", xrsr_power_mode_str(power_mode));
      return(false);
   }

   memset(g_xrsr.routes, 0, sizeof(g_xrsr.routes));

   uint32_t index = 0;
   do {
      if(routes[index].src >= XRSR_SRC_INVALID) {
         break;
      }
      XLOGD_INFO("%u: src <%s>", index, xrsr_src_str(routes[index].src));

      if(routes[index].dst_qty < 1 || routes[index].dst_qty > XRSR_DST_QTY_MAX) {
         XLOGD_ERROR("invalid dsts array");
         break;
      }

      for(uint32_t dst_index = 0; dst_index < routes[index].dst_qty; dst_index++) {
         const xrsr_dst_t *dst = &routes[index].dsts[dst_index];
         XLOGD_INFO("dst <%s>", dst->url);
      }
      index++;
   } while(1);

   // Create xraudio object
   xraudio_keyword_config_t config = (keyword_config == NULL) ? XRAUDIO_KEYWORD_CONFIG_5 : (xraudio_keyword_config_t)keyword_config->sensitivity;

   g_xrsr.src                 = XRSR_SRC_INVALID;
   g_xrsr.first_stream_req    = true;

   for(index = 0; index < XRSR_DST_QTY_MAX; index++) {
      g_xrsr.pipe_fds_rd[index] = -1;
   }

   if(NULL == json_obj_vsdk) {
      XLOGD_INFO("xraudio json object not found, using defaults");
   } else {
      json_obj_xraudio = json_object_get(json_obj_vsdk, JSON_OBJ_NAME_XRAUDIO);
      if(NULL == json_obj_xraudio) {
         XLOGD_INFO("xraudio json object not found, using defaults");
      } else {
         if(!json_is_object(json_obj_xraudio))  {
            XLOGD_WARN("json_obj_xraudio is not object, using defaults");
            json_obj_xraudio = NULL;
         }
      }
   }
   #ifdef WS_ENABLED
   memset(&g_xrsr.ws_json_config_fpm, 0, sizeof(xrsr_ws_json_config_t));
   memset(&g_xrsr.ws_json_config_lpm, 0, sizeof(xrsr_ws_json_config_t));

   json_t *json_obj;
   json_t *json_obj_ws     = json_object_get(json_obj_vsdk, JSON_OBJ_NAME_WS);
   if(NULL == json_obj_ws || !json_is_object(json_obj_ws)) {
      XLOGD_INFO("ws json object not found, using defaults");
   } else {
      //"debug" shared between full and low power configs
      json_obj = json_object_get(json_obj_ws, JSON_BOOL_NAME_WS_DEBUG);
      if(json_obj != NULL && json_is_boolean(json_obj)) {
         g_xrsr.ws_json_config_fpm.val_debug = json_is_true(json_obj) ? true : false;
         g_xrsr.ws_json_config_fpm.ptr_debug = &g_xrsr.ws_json_config_fpm.val_debug;
         g_xrsr.ws_json_config_lpm.val_debug = json_is_true(json_obj) ? true : false;
         g_xrsr.ws_json_config_lpm.ptr_debug = &g_xrsr.ws_json_config_lpm.val_debug;
         XLOGD_INFO("ws json: debug <%s>", g_xrsr.ws_json_config_fpm.val_debug ? "YES" : "NO");
      }

      json_t *json_obj_fpm = json_object_get(json_obj_ws, JSON_OBJ_NAME_WS_FPM);
      if(NULL == json_obj_fpm || !json_is_object(json_obj_fpm)) {
	XLOGD_INFO("fpm json object not found, using defaults");
      } else {
         json_obj = json_object_get(json_obj_fpm, JSON_INT_NAME_WS_FPM_CONNECT_CHECK_INTERVAL);
         if(json_obj != NULL && json_is_integer(json_obj)) {
            json_int_t value = json_integer_value(json_obj);
            if(value >= 0 && value <= 1000) {
               g_xrsr.ws_json_config_fpm.val_connect_check_interval = value;
               g_xrsr.ws_json_config_fpm.ptr_connect_check_interval = &g_xrsr.ws_json_config_fpm.val_connect_check_interval;
               XLOGD_INFO("ws fpm json: connect check interval <%d> ms", g_xrsr.ws_json_config_fpm.val_connect_check_interval);
            }
         }
         json_obj = json_object_get(json_obj_fpm, JSON_INT_NAME_WS_FPM_TIMEOUT_CONNECT);
         if(json_obj != NULL && json_is_integer(json_obj)) {
            json_int_t value = json_integer_value(json_obj);
            if(value >= 0 && value <= 60000) {
               g_xrsr.ws_json_config_fpm.val_timeout_connect = value;
               g_xrsr.ws_json_config_fpm.ptr_timeout_connect = &g_xrsr.ws_json_config_fpm.val_timeout_connect;
               XLOGD_INFO("ws fpm json: timeout connect <%d> ms", g_xrsr.ws_json_config_fpm.val_timeout_connect);
            }
         }
         json_obj = json_object_get(json_obj_fpm, JSON_INT_NAME_WS_FPM_TIMEOUT_INACTIVITY);
         if(json_obj != NULL && json_is_integer(json_obj)) {
            json_int_t value = json_integer_value(json_obj);
            if(value >= 0 && value <= 60000) {
               g_xrsr.ws_json_config_fpm.val_timeout_inactivity = value;
               g_xrsr.ws_json_config_fpm.ptr_timeout_inactivity = &g_xrsr.ws_json_config_fpm.val_timeout_inactivity;
               XLOGD_INFO("ws fpm json: timeout inactivity <%d> ms", g_xrsr.ws_json_config_fpm.val_timeout_inactivity);
            }
         }
         json_obj = json_object_get(json_obj_fpm, JSON_INT_NAME_WS_FPM_TIMEOUT_SESSION);
         if(json_obj != NULL && json_is_integer(json_obj)) {
            json_int_t value = json_integer_value(json_obj);
            if(value >= 0 && value <= 60000) {
               g_xrsr.ws_json_config_fpm.val_timeout_session = value;
               g_xrsr.ws_json_config_fpm.ptr_timeout_session = &g_xrsr.ws_json_config_fpm.val_timeout_session;
               XLOGD_INFO("ws fpm json: timeout session <%d> ms", g_xrsr.ws_json_config_fpm.val_timeout_session);
            }
         }
         json_obj = json_object_get(json_obj_fpm, JSON_BOOL_NAME_WS_FPM_IPV4_FALLBACK);
         if(json_obj != NULL && json_is_boolean(json_obj)) {
            g_xrsr.ws_json_config_fpm.val_ipv4_fallback = json_is_true(json_obj) ? true : false;
            g_xrsr.ws_json_config_fpm.ptr_ipv4_fallback = &g_xrsr.ws_json_config_fpm.val_ipv4_fallback;
            XLOGD_INFO("ws fpm json: ipv4 fallback <%s>", g_xrsr.ws_json_config_fpm.val_ipv4_fallback ? "YES" : "NO");
         }
         json_obj = json_object_get(json_obj_fpm, JSON_INT_NAME_WS_FPM_BACKOFF_DELAY);
         if(json_obj != NULL && json_is_integer(json_obj)) {
            json_int_t value = json_integer_value(json_obj);
            if(value >= 0 && value <= 10000) {
               g_xrsr.ws_json_config_fpm.val_backoff_delay = value;
               g_xrsr.ws_json_config_fpm.ptr_backoff_delay = &g_xrsr.ws_json_config_fpm.val_backoff_delay;
               XLOGD_INFO("ws fpm json: backoff delay <%d> ms", g_xrsr.ws_json_config_fpm.val_backoff_delay);
            }
         }
      }

      json_t *json_obj_lpm = json_object_get(json_obj_ws, JSON_OBJ_NAME_WS_LPM);
      if(NULL == json_obj_lpm || !json_is_object(json_obj_lpm)) {
	XLOGD_INFO("lpm json object not found, using defaults");
      } else {
         json_obj = json_object_get(json_obj_lpm, JSON_INT_NAME_WS_LPM_CONNECT_CHECK_INTERVAL);
         if(json_obj != NULL && json_is_integer(json_obj)) {
            json_int_t value = json_integer_value(json_obj);
            if(value >= 0 && value <= 1000) {
               g_xrsr.ws_json_config_lpm.val_connect_check_interval = value;
               g_xrsr.ws_json_config_lpm.ptr_connect_check_interval = &g_xrsr.ws_json_config_lpm.val_connect_check_interval;
               XLOGD_INFO("ws lpm json: connect check interval <%d> ms", g_xrsr.ws_json_config_lpm.val_connect_check_interval);
            }
         }
         json_obj = json_object_get(json_obj_lpm, JSON_INT_NAME_WS_LPM_TIMEOUT_CONNECT);
         if(json_obj != NULL && json_is_integer(json_obj)) {
            json_int_t value = json_integer_value(json_obj);
            if(value >= 0 && value <= 60000) {
               g_xrsr.ws_json_config_lpm.val_timeout_connect = value;
               g_xrsr.ws_json_config_lpm.ptr_timeout_connect = &g_xrsr.ws_json_config_lpm.val_timeout_connect;
               XLOGD_INFO("ws lpm json: timeout connect <%d> ms", g_xrsr.ws_json_config_lpm.val_timeout_connect);
            }
         }
         json_obj = json_object_get(json_obj_lpm, JSON_INT_NAME_WS_LPM_TIMEOUT_INACTIVITY);
         if(json_obj != NULL && json_is_integer(json_obj)) {
            json_int_t value = json_integer_value(json_obj);
            if(value >= 0 && value <= 60000) {
               g_xrsr.ws_json_config_lpm.val_timeout_inactivity = value;
               g_xrsr.ws_json_config_lpm.ptr_timeout_inactivity = &g_xrsr.ws_json_config_lpm.val_timeout_inactivity;
               XLOGD_INFO("ws lpm json: timeout inactivity <%d> ms", g_xrsr.ws_json_config_lpm.val_timeout_inactivity);
            }
         }
         json_obj = json_object_get(json_obj_lpm, JSON_INT_NAME_WS_LPM_TIMEOUT_SESSION);
         if(json_obj != NULL && json_is_integer(json_obj)) {
            json_int_t value = json_integer_value(json_obj);
            if(value >= 0 && value <= 60000) {
               g_xrsr.ws_json_config_lpm.val_timeout_session = value;
               g_xrsr.ws_json_config_lpm.ptr_timeout_session = &g_xrsr.ws_json_config_lpm.val_timeout_session;
               XLOGD_INFO("ws lpm json: timeout session <%d> ms", g_xrsr.ws_json_config_lpm.val_timeout_session);
            }
         }
         json_obj = json_object_get(json_obj_lpm, JSON_BOOL_NAME_WS_LPM_IPV4_FALLBACK);
         if(json_obj != NULL && json_is_boolean(json_obj)) {
            g_xrsr.ws_json_config_lpm.val_ipv4_fallback = json_is_true(json_obj) ? true : false;
            g_xrsr.ws_json_config_lpm.ptr_ipv4_fallback = &g_xrsr.ws_json_config_lpm.val_ipv4_fallback;
            XLOGD_INFO("ws lpm json: ipv4 fallback <%s>", g_xrsr.ws_json_config_lpm.val_ipv4_fallback ? "YES" : "NO");
         }
         json_obj = json_object_get(json_obj_lpm, JSON_INT_NAME_WS_LPM_BACKOFF_DELAY);
         if(json_obj != NULL && json_is_integer(json_obj)) {
            json_int_t value = json_integer_value(json_obj);
            if(value >= 0 && value <= 10000) {
               g_xrsr.ws_json_config_lpm.val_backoff_delay = value;
               g_xrsr.ws_json_config_lpm.ptr_backoff_delay = &g_xrsr.ws_json_config_lpm.val_backoff_delay;
               XLOGD_INFO("ws lpm json: backoff delay <%d> ms", g_xrsr.ws_json_config_lpm.val_backoff_delay);
            }
         }
      }
   }
   #endif

   xraudio_power_mode_t xraudio_power_mode;

   switch(power_mode) {
      case XRSR_POWER_MODE_FULL:
         xraudio_power_mode = XRAUDIO_POWER_MODE_FULL;
         g_xrsr.ws_json_config = &g_xrsr.ws_json_config_fpm;
         break;
      case XRSR_POWER_MODE_LOW:
         xraudio_power_mode = XRAUDIO_POWER_MODE_LOW;
         g_xrsr.ws_json_config = &g_xrsr.ws_json_config_lpm;
         break;
      case XRSR_POWER_MODE_SLEEP:
         xraudio_power_mode = XRAUDIO_POWER_MODE_SLEEP;
         g_xrsr.ws_json_config = &g_xrsr.ws_json_config_lpm;
         break;
      default:
         XLOGD_ERROR("Invalid power mode");
         return(false);
   }

   g_xrsr.xrsr_xraudio_object = xrsr_xraudio_create(XRSR_KEYWORD_PHRASE, config, xraudio_power_mode, privacy_mode, json_obj_xraudio);

   if(capture_config != NULL) {
      if(capture_config->delete_files) {
         xrsr_xraudio_internal_capture_delete_files(g_xrsr.xrsr_xraudio_object, capture_config->dir_path);
      }
      if(capture_config->enable) {
         xraudio_internal_capture_params_t capture_params;
         capture_params.enable        = capture_config->enable;
         capture_params.file_qty_max  = capture_config->file_qty_max;
         capture_params.file_size_max = capture_config->file_size_max;

         if(capture_config->dir_path == NULL) {
            XLOGD_ERROR("dir path is NULL");
         } else {
            g_xrsr.capture_dir_path = strdup(capture_config->dir_path);

            if(g_xrsr.capture_dir_path == NULL) {
               XLOGD_ERROR("out of memory");
            } else {
               capture_params.dir_path = g_xrsr.capture_dir_path;
               xrsr_xraudio_internal_capture_params_set(g_xrsr.xrsr_xraudio_object, &capture_params);
            }
         }
      }
   }

   // TODO Get prod vs debug from rdkversion

   if(!xrsr_threads_init(false)) {
      XLOGD_ERROR("thread init failed");
      return(false);
   }

   // Send the route information
   sem_t semaphore;
   sem_init(&semaphore, 0, 0);
   xrsr_queue_msg_route_update_t msg;
   msg.header.type = XRSR_QUEUE_MSG_TYPE_ROUTE_UPDATE;
   msg.semaphore   = &semaphore;
   msg.routes      = routes;
   msg.host_name   = host_name;

   xrsr_queue_msg_push(xrsr_msgq_fd_get(), (const char *)&msg, sizeof(msg));
   sem_wait(&semaphore);
   sem_destroy(&semaphore);

   g_xrsr.power_mode   = power_mode;
   g_xrsr.privacy_mode = privacy_mode;
   g_xrsr.opened       = true;
   return(true);
}

void xrsr_close(void) {
   if(!g_xrsr.opened) {
      XLOGD_ERROR("not opened");
      return;
   }
   XLOGD_INFO("");

   xrsr_threads_term();

   xrsr_xraudio_destroy(g_xrsr.xrsr_xraudio_object);
   g_xrsr.xrsr_xraudio_object = NULL;

   xrsr_route_free_all();

   if(g_xrsr.capture_dir_path != NULL) {
      free(g_xrsr.capture_dir_path);
      g_xrsr.capture_dir_path = NULL;
   }

   g_xrsr.opened = false;
}

bool xrsr_threads_init(bool is_prod) {
   xrsr_thread_params_t params_main;

   // Launch threads
   xrsr_thread_info_t *info;
   info             = &g_xrsr.threads[XRSR_THREAD_MAIN];
   info->name       = "main";
   info->msgq_id    = -1;
   info->msgsize    = XRSR_MSG_QUEUE_MSG_SIZE_MAX;
   info->func       = xrsr_thread_main;
   info->params     = &params_main;
   params_main.semaphore = &info->semaphore;
   params_main.is_prod   = is_prod;
   sem_init(&info->semaphore, 0, 0);

   for(uint32_t index = 0; index < XRSR_THREAD_QTY; index++) {
      xrsr_thread_info_t *info = &g_xrsr.threads[index];

      // Create message queue
      if(!xrsr_message_queue_open(&info->msgq_id, info->msgsize)) {
         XLOGD_ERROR("unable to open msgq");
         return(false);
      }
      ((xrsr_thread_params_t *)info->params)->msgq_id = info->msgq_id;

      if(0 != pthread_create(&info->id, NULL, info->func, info->params)) {
         XLOGD_ERROR("unable to launch thread");
         return(false);
      }

      // Block until initialization is complete or a timeout occurs
      XLOGD_INFO("Waiting for %s thread initialization...", info->name);
      sem_wait(&info->semaphore);
   }
   sem_destroy(params_main.semaphore);
   return(true);
}

void xrsr_threads_term(void) {
   // Clean up the threads
   for(uint32_t index = 0; index < XRSR_THREAD_QTY; index++) {
      xrsr_thread_info_t *info = &g_xrsr.threads[index];

      if(info->msgq_id < 0) {
         continue;
      }

      sem_t semaphore;
      sem_init(&semaphore, 0, 0);
      xrsr_queue_msg_term_t msg;
      msg.header.type = XRSR_QUEUE_MSG_TYPE_TERMINATE;
      msg.semaphore   = &semaphore;

      struct timespec end_time;

      xrsr_queue_msg_push(info->msgq_id, (const char *)&msg, sizeof(msg));

      // Block until termination is acknowledged or a timeout occurs
      XLOGD_INFO("Waiting for %s thread termination...", info->name);
      int rc = -1;
      if(clock_gettime(CLOCK_REALTIME, &end_time) != 0) {
         XLOGD_ERROR("unable to get time");
      } else {
         end_time.tv_sec += 5;
         do {
            errno = 0;
            rc = sem_timedwait(&semaphore, &end_time);
            if(rc == -1 && errno == EINTR) {
               XLOGD_INFO("interrupted");
            } else {
               break;
            }
         } while(1);
      }

      if(rc != 0) { // no response received
         XLOGD_INFO("Do NOT wait for thread to exit");
      } else {
         sem_destroy(&semaphore);
         // Wait for thread to exit
         XLOGD_INFO("Waiting for thread to exit");
         void *retval = NULL;
         pthread_join(info->id, &retval);
         XLOGD_INFO("thread exited.");
      }

      // Close message queue
      xrsr_message_queue_close(&info->msgq_id);
   }
}

void xrsr_route_free_all(void) {
   for(uint32_t index = 0; index < XRSR_SRC_INVALID; index++) {
      xrsr_route_free(index);
   }
}

void xrsr_route_free(xrsr_src_t src) {
   for(uint32_t index = 0; index < XRSR_DST_QTY_MAX; index++) {
      xrsr_dst_int_t *dst = &g_xrsr.routes[src].dsts[index];

      if(dst->initialized) {
         switch(dst->url_parts.prot) {
            #ifdef HTTP_ENABLED
            case XRSR_PROTOCOL_HTTP:
            case XRSR_PROTOCOL_HTTPS: {
               xrsr_http_term(&dst->conn_state.http);
               dst->initialized = false;
               break;
            }
            #endif
            #ifdef WS_ENABLED
            case XRSR_PROTOCOL_WS:
            case XRSR_PROTOCOL_WSS: {
               xrsr_ws_term(&dst->conn_state.ws);
               dst->initialized = false;
               break;
            }
            #endif
            #ifdef SDT_ENABLED
            case XRSR_PROTOCOL_SDT: {
               xrsr_sdt_term(&dst->conn_state.sdt);
               break;
            }
            #endif
            default: {
               break;
            }
         }
      }
      dst->handler  = NULL;
      xrsr_url_free(&dst->url_parts);
   }
}

void xrsr_route_update(const char *host_name, const xrsr_route_t *route, xrsr_thread_state_t *state) {
   xrsr_src_t src = route->src;

   if((uint32_t)src >= (uint32_t)XRSR_SRC_INVALID) {
      XLOGD_WARN("invalid src <%s>", xrsr_src_str(src));
      return;
   }

   xrsr_route_free(src);

   if(route->dst_qty == 0) { // Just deleting the route
      return;
   }

   uint32_t index = 0;

   for(uint32_t dst_index = 0; dst_index < route->dst_qty; dst_index++) {
      const xrsr_dst_t *dst = &route->dsts[dst_index];
      const char *                 url             = dst->url;
      xrsr_audio_format_t          format          = dst->format;
      uint16_t                     stream_time_min = dst->stream_time_min;
      xraudio_input_record_from_t  stream_from     = XRAUDIO_INPUT_RECORD_FROM_BEGINNING;
      xraudio_input_record_until_t stream_until    = XRAUDIO_INPUT_RECORD_UNTIL_END_OF_STREAM;

      if(index >= XRSR_DST_QTY_MAX) {
         XLOGD_ERROR("maximum destinations exceeded <%u>", index);
         break;
      }

      if((uint32_t)format >= (uint32_t)XRSR_AUDIO_FORMAT_INVALID) {
         XLOGD_WARN("invalid audio format <%s>", xrsr_audio_format_str(format));
         return;
      }

      if((uint32_t)dst->stream_from >= XRSR_STREAM_FROM_INVALID) {
         XLOGD_WARN("invalid stream from <%s>", xrsr_stream_from_str(dst->stream_from));
         return;
      }

      if((uint32_t)dst->stream_until >= XRSR_STREAM_UNTIL_INVALID) {
         XLOGD_WARN("invalid stream until <%s>", xrsr_stream_until_str(dst->stream_until));
         return;
      }

      if(dst->stream_from == XRSR_STREAM_FROM_KEYWORD_BEGIN) {
         stream_from = XRAUDIO_INPUT_RECORD_FROM_KEYWORD_BEGIN;
      } else if(dst->stream_from == XRSR_STREAM_FROM_KEYWORD_END) {
         stream_from = XRAUDIO_INPUT_RECORD_FROM_KEYWORD_END;
      }

      if(dst->stream_until == XRSR_STREAM_UNTIL_END_OF_SPEECH) {
         stream_until = XRAUDIO_INPUT_RECORD_UNTIL_END_OF_SPEECH;
      } else if(dst->stream_until == XRSR_STREAM_UNTIL_END_OF_KEYWORD) {
         stream_until = XRAUDIO_INPUT_RECORD_UNTIL_END_OF_KEYWORD;
      }

      // Parse url
      xrsr_url_parts_t url_parts;
      if(!xrsr_url_parse(url, &url_parts)) {
         XLOGD_ERROR("invalid url <%s>", url);
         return;
      }

      XLOGD_DEBUG("src <%s> dst qty <%u> index <%u> url <%s> session begin <%p>", xrsr_src_str(route->src), route->dst_qty, dst_index, dst->url, dst->handlers.session_begin);

      xrsr_dst_int_t *dst_int = &g_xrsr.routes[src].dsts[index];

      switch(url_parts.prot) {
         #ifdef HTTP_ENABLED
         case XRSR_PROTOCOL_HTTP:
         case XRSR_PROTOCOL_HTTPS: {
            dst_int->handler = xrsr_protocol_handler_http;

            if(!xrsr_http_init(&dst_int->conn_state.http, true)) {
               XLOGD_ERROR("http init");
               return;
            }
            dst_int->initialized = true;
            break;
         }
         #endif
         #ifdef WS_ENABLED
         case XRSR_PROTOCOL_WS:
         case XRSR_PROTOCOL_WSS: {
            dst_int->handler = xrsr_protocol_handler_ws;

            // Set params from json config and allow override per url/powerstate
            for(int i = 0; i < XRSR_POWER_MODE_INVALID; i++) {
               if(dst->params[i] != NULL) {
                  dst_int->dst_param_ptrs[i].debug                  = &dst->params[i]->debug;
                  dst_int->dst_param_ptrs[i].connect_check_interval = &dst->params[i]->connect_check_interval;
                  dst_int->dst_param_ptrs[i].timeout_connect        = &dst->params[i]->timeout_connect;
                  dst_int->dst_param_ptrs[i].timeout_inactivity     = &dst->params[i]->timeout_inactivity;
                  dst_int->dst_param_ptrs[i].timeout_session        = &dst->params[i]->timeout_session;
                  dst_int->dst_param_ptrs[i].ipv4_fallback          = &dst->params[i]->ipv4_fallback;
                  dst_int->dst_param_ptrs[i].backoff_delay          = &dst->params[i]->backoff_delay;
               } else {
                  dst_int->dst_param_ptrs[i].debug                  = g_xrsr.ws_json_config->ptr_debug;
                  dst_int->dst_param_ptrs[i].connect_check_interval = g_xrsr.ws_json_config->ptr_connect_check_interval;
                  dst_int->dst_param_ptrs[i].timeout_connect        = g_xrsr.ws_json_config->ptr_timeout_connect;
                  dst_int->dst_param_ptrs[i].timeout_inactivity     = g_xrsr.ws_json_config->ptr_timeout_inactivity;
                  dst_int->dst_param_ptrs[i].timeout_session        = g_xrsr.ws_json_config->ptr_timeout_session;
                  dst_int->dst_param_ptrs[i].ipv4_fallback          = g_xrsr.ws_json_config->ptr_ipv4_fallback;
                  dst_int->dst_param_ptrs[i].backoff_delay          = g_xrsr.ws_json_config->ptr_backoff_delay;
               }
            }

            xrsr_ws_params_t params;
            params.prot               = url_parts.prot;
            params.host_name          = host_name;
            params.timer_obj          = state->timer_obj;
            params.dst_params         = &dst_int->dst_param_ptrs[g_xrsr.power_mode];

            if(!xrsr_ws_init(&dst_int->conn_state.ws, &params)) {
               XLOGD_ERROR("ws init");
               return;
            }
            dst_int->initialized = true;
            break;
         }
         #endif
         #ifdef SDT_ENABLED
         case XRSR_PROTOCOL_SDT: {
           dst_int->handler = xrsr_protocol_handler_sdt;
           xrsr_sdt_params_t params;
           params.prot               = url_parts.prot;
           params.host_name          = host_name;
           params.timer_obj          = state->timer_obj;


            if(!xrsr_sdt_init(&dst_int->conn_state.sdt, &params)) {
               XLOGD_ERROR("xrsr sdt init failed");
               return;
            }
            break;
         }
         #endif
         default: {
            XLOGD_ERROR("invalid protocol <%s>", xrsr_protocol_str(url_parts.prot));
            xrsr_url_free(&url_parts);
            return;
         }
      }

      // Add new route
      dst_int->url_parts        = url_parts;
      dst_int->handlers         = dst->handlers;
      dst_int->format           = format;
      dst_int->stream_time_min  = stream_time_min;
      dst_int->stream_from      = stream_from;
      dst_int->stream_offset    = dst->stream_offset;
      dst_int->stream_until     = stream_until;
      dst_int->keyword_begin    = 0;
      dst_int->keyword_duration = 0;

      index++;
   }
}

bool xrsr_route(const xrsr_route_t routes[]) {
   if(!g_xrsr.opened) {
      XLOGD_ERROR("not opened");
      return(false);
   }
   if(routes == NULL) {
      XLOGD_ERROR("invalid parameter");
      return(false);
   }

   uint32_t index = 0;
   do {
      if(routes[index].src >= XRSR_SRC_INVALID) {
         break;
      }
      XLOGD_INFO("%u: src <%s>", index, xrsr_src_str(routes[index].src));

      if(routes[index].dst_qty < 1 || routes[index].dst_qty > XRSR_DST_QTY_MAX) {
         XLOGD_ERROR("invalid dsts array");
         break;
      }

      for(uint32_t dst_index = 0; dst_index < routes[index].dst_qty; dst_index++) {
         const xrsr_dst_t *dst = &routes[index].dsts[dst_index];
         XLOGD_INFO("dst <%s> audio format <%s>", dst->url, xrsr_audio_format_str(dst->format));
      }

      index++;
   } while(1);

   sem_t semaphore;
   sem_init(&semaphore, 0, 0);

   // Send the route information
   xrsr_queue_msg_route_update_t msg;
   msg.header.type = XRSR_QUEUE_MSG_TYPE_ROUTE_UPDATE;
   msg.semaphore   = &semaphore;
   msg.routes      = routes;

   xrsr_queue_msg_push(xrsr_msgq_fd_get(), (const char *)&msg, sizeof(msg));
   sem_wait(&semaphore);
   sem_destroy(&semaphore);

   return(true);
}

bool xrsr_host_name_set(const char *host_name) {
   if(!g_xrsr.opened) {
      XLOGD_ERROR("not opened");
      return(false);
   }

   sem_t semaphore;
   sem_init(&semaphore, 0, 0);

   // Send the keyword information
   xrsr_queue_msg_host_name_update_t msg;
   msg.header.type    = XRSR_QUEUE_MSG_TYPE_HOST_NAME_UPDATE;
   msg.semaphore      = &semaphore;
   msg.host_name      = host_name;

   xrsr_queue_msg_push(xrsr_msgq_fd_get(), (const char *)&msg, sizeof(msg));
   sem_wait(&semaphore);
   sem_destroy(&semaphore);

   return(true);
}

bool xrsr_keyword_config_set(const xrsr_keyword_config_t *keyword_config) {
   if(!g_xrsr.opened) {
      XLOGD_ERROR("not opened");
      return(false);
   }
   if(keyword_config == NULL) {
      XLOGD_ERROR("invalid parameter");
      return(false);
   }
   if(keyword_config->sensitivity >= XRAUDIO_KEYWORD_CONFIG_INVALID) {
      XLOGD_ERROR("invalid keyword config - sensitivity <%u>", keyword_config->sensitivity);
      return(false);
   }

   sem_t semaphore;
   sem_init(&semaphore, 0, 0);

   // Send the keyword information
   xrsr_queue_msg_keyword_update_t msg;
   msg.header.type    = XRSR_QUEUE_MSG_TYPE_KEYWORD_UPDATE;
   msg.semaphore      = &semaphore;
   msg.keyword_config = keyword_config;

   xrsr_queue_msg_push(xrsr_msgq_fd_get(), (const char *)&msg, sizeof(msg));
   sem_wait(&semaphore);
   sem_destroy(&semaphore);

   return(true);
}

bool xrsr_power_mode_set(xrsr_power_mode_t power_mode) {
   if(!g_xrsr.opened) {
      XLOGD_ERROR("not opened");
      return(false);
   }
   if((uint32_t)power_mode >= XRSR_POWER_MODE_INVALID) {
      XLOGD_ERROR("invalid power mode <%s>", xrsr_power_mode_str(power_mode));
      return(false);
   }
   if(g_xrsr.power_mode == power_mode) {
      return(true);
   }

   bool result = false;
   sem_t semaphore;
   sem_init(&semaphore, 0, 0);

   // Send the power mode
   xrsr_queue_msg_power_mode_update_t msg;
   msg.header.type    = XRSR_QUEUE_MSG_TYPE_POWER_MODE_UPDATE;
   msg.semaphore      = &semaphore;
   msg.power_mode     = power_mode;
   msg.result         = &result;

   xrsr_queue_msg_push(xrsr_msgq_fd_get(), (const char *)&msg, sizeof(msg));
   sem_wait(&semaphore);
   sem_destroy(&semaphore);

   if(result) {
      g_xrsr.power_mode = power_mode;

      #ifdef WS_ENABLED
      g_xrsr.ws_json_config = (XRSR_POWER_MODE_LOW==power_mode) ? &g_xrsr.ws_json_config_lpm : &g_xrsr.ws_json_config_fpm;
      #endif
   }

   return(result);
}

bool xrsr_privacy_mode_set(bool enable) {
   if(!g_xrsr.opened) {
      XLOGD_ERROR("not opened");
      return(false);
   }
   if(g_xrsr.privacy_mode == enable) {
      XLOGD_WARN("already %s", enable ? "enabled" : "disabled");
      return(true);
   }

   bool result = false;
   sem_t semaphore;
   sem_init(&semaphore, 0, 0);

   // Send the privacy mode
   xrsr_queue_msg_privacy_mode_update_t msg;
   msg.header.type    = XRSR_QUEUE_MSG_TYPE_PRIVACY_MODE_UPDATE;
   msg.semaphore      = &semaphore;
   msg.enable         = enable;
   msg.result         = &result;

   xrsr_queue_msg_push(xrsr_msgq_fd_get(), (const char *)&msg, sizeof(msg));
   sem_wait(&semaphore);
   sem_destroy(&semaphore);

   if(result) {
      g_xrsr.privacy_mode = enable;
   }

   return(result);
}

bool xrsr_privacy_mode_get(bool *enabled) {
   if(!g_xrsr.opened) {
      XLOGD_ERROR("not opened");
      return(false);
   }

   bool result = false;
   sem_t semaphore;
   sem_init(&semaphore, 0, 0);

   // Get the privacy mode
   xrsr_queue_msg_privacy_mode_get_t msg;
   msg.header.type = XRSR_QUEUE_MSG_TYPE_PRIVACY_MODE_GET;
   msg.semaphore   = &semaphore;
   msg.enabled     = enabled;
   msg.result      = &result;

   xrsr_queue_msg_push(xrsr_msgq_fd_get(), (const char *)&msg, sizeof(msg));
   sem_wait(&semaphore);
   sem_destroy(&semaphore);

   return(result);
}

void *xrsr_thread_main(void *param) {
   xrsr_thread_params_t params = *((xrsr_thread_params_t *)param);
   char msg[XRSR_MSG_QUEUE_MSG_SIZE_MAX];

   xrsr_thread_state_t state;
   state.running               = true;
   state.timer_obj             = rdkx_timer_create(16, true, !params.is_prod);

   if(state.timer_obj == NULL) {
      XLOGD_ERROR("timer create");
      return(NULL);
   }

   // Unblock the caller that launched this thread
   sem_post(params.semaphore);
   params.semaphore = NULL;

   XLOGD_INFO("Enter main loop");

   do {
      int src;
      int nfds = params.msgq_id + 1;

      fd_set rfds;
      FD_ZERO(&rfds);
      FD_SET(params.msgq_id, &rfds);

      fd_set wfds;
      FD_ZERO(&wfds);

      // Add fd's for all open connections
      for(uint32_t index_src = 0; index_src < XRSR_SRC_INVALID; index_src++) {
         for(uint32_t index_dst = 0; index_dst < XRSR_DST_QTY_MAX; index_dst++) {
            xrsr_dst_int_t *dst = &g_xrsr.routes[index_src].dsts[index_dst];

            if(dst->handler == NULL) {
               continue;
            }
            switch(dst->url_parts.prot) {
               #ifdef HTTP_ENABLED
               case XRSR_PROTOCOL_HTTP:
               case XRSR_PROTOCOL_HTTPS: {
                  xrsr_state_http_t *http = &dst->conn_state.http;

                  if(xrsr_http_is_connected(http)) {
                     xrsr_http_fd_set(http, 1, &nfds, &rfds, &wfds, NULL);
                  }
                  break;
               }
               #endif
               #ifdef WS_ENABLED
               case XRSR_PROTOCOL_WS:
               case XRSR_PROTOCOL_WSS: {
                  xrsr_state_ws_t *ws = &dst->conn_state.ws;
                  if(xrsr_ws_is_established(ws)) {
                     xrsr_ws_fd_set(ws, &nfds, &rfds, &wfds, NULL);
                  }
                  break;
               }
               #endif
               #ifdef SDT_ENABLED
               case XRSR_PROTOCOL_SDT: {
                  xrsr_state_sdt_t *sdt = &dst->conn_state.sdt;
                  if(xrsr_sdt_is_established(sdt)) {
                     xrsr_sdt_fd_set(sdt, &nfds, &rfds, &wfds, NULL);
                  }
                  break;
               }
               #endif

               default: {
                  break;
               }
            }
         }
      }

      struct timeval tv;
      rdkx_timer_handler_t handler = NULL;
      void *data = NULL;
      rdkx_timer_id_t timer_id = rdkx_timer_next_get(state.timer_obj, &tv, &handler, &data);

      errno = 0;
      if(timer_id >= 0) {
         XLOGD_DEBUG("timer id <%d> timeout %d secs %d microsecs", timer_id, tv.tv_sec, tv.tv_usec);
         if(tv.tv_sec == 0 && tv.tv_usec == 0) { // Process the expired timer instead of calling select().
            src = 0;
         } else {
            src = select(nfds, &rfds, &wfds, NULL, &tv);
         }
      } else {
         XLOGD_DEBUG("no timeout set");
         src = select(nfds, &rfds, &wfds, NULL, NULL);
      }

      if(src < 0) { // error occurred
         int errsv = errno;
         XLOGD_ERROR("select failed, rc <%s>", strerror(errsv));
         break;
      } else if(src == 0) { // timeout occurred
         XLOGD_DEBUG("timeout occurred");
         if(handler == NULL) {
            XLOGD_ERROR("invalid timer - handler <%p> data <%p>", handler, data);
            if(!rdkx_timer_remove(state.timer_obj, timer_id)) {
               XLOGD_ERROR("timer remove");
            }
         } else {
            (*handler)(data);
         }
         
         continue;
      }
      if(FD_ISSET(params.msgq_id, &rfds)) {
         ssize_t bytes_read = xr_mq_pop(params.msgq_id, msg, sizeof(msg));
         if(bytes_read <= 0) {
            XLOGD_ERROR("mq_receive failed, rc <%d>", bytes_read);
         } else {
            xrsr_queue_msg_header_t *header = (xrsr_queue_msg_header_t *)msg;

            if((uint32_t)header->type >= XRSR_QUEUE_MSG_TYPE_INVALID) {
               XLOGD_ERROR("invalid msg type <%s>", xrsr_queue_msg_type_str(header->type));
            } else {
               XLOGD_DEBUG("msg type <%s>", xrsr_queue_msg_type_str(header->type));
               (*g_xrsr_msg_handlers[header->type])(&params, &state, msg);
            }
         }
      }

      // Check fd's for all open connections
      for(uint32_t index_src = 0; index_src < XRSR_SRC_INVALID; index_src++) {
         for(uint32_t index_dst = 0; index_dst < XRSR_DST_QTY_MAX; index_dst++) {
            xrsr_dst_int_t *dst = &g_xrsr.routes[index_src].dsts[index_dst];

            switch(dst->url_parts.prot) {
               #ifdef HTTP_ENABLED
               case XRSR_PROTOCOL_HTTP:
               case XRSR_PROTOCOL_HTTPS: {
                  xrsr_state_http_t *http = &dst->conn_state.http;
                  if(xrsr_http_is_connected(http)) {
                     xrsr_http_handle_fds(http, 1, &rfds, &wfds, NULL);
                  }
                  break;
               }
               #endif
               #ifdef WS_ENABLED
               case XRSR_PROTOCOL_WS:
               case XRSR_PROTOCOL_WSS: {
                  xrsr_state_ws_t *ws = &dst->conn_state.ws;
                  if(!xrsr_ws_is_disconnected(ws)) {
                     xrsr_ws_handle_fds(ws, &rfds, &wfds, NULL);
                  }
                  break;
               }
               #endif
               #ifdef SDT_ENABLED
               case XRSR_PROTOCOL_SDT: {
                  xrsr_state_sdt_t *sdt = &dst->conn_state.sdt;
                  if(!xrsr_sdt_is_disconnected(sdt)) {
                     xrsr_sdt_handle_fds(sdt, &rfds, &wfds, NULL);
                  }
                  break;
               }
               #endif

               default: {
                  break;
               }
            }
         }
      }
   } while(state.running);

   // Terminate all open connections
   for(uint32_t index_src = 0; index_src < XRSR_SRC_INVALID; index_src++) {
      for(uint32_t index_dst = 0; index_dst < XRSR_DST_QTY_MAX; index_dst++) {
         xrsr_dst_int_t *dst = &g_xrsr.routes[index_src].dsts[index_dst];

         switch(dst->url_parts.prot) {
            #ifdef HTTP_ENABLED
            case XRSR_PROTOCOL_HTTP:
            case XRSR_PROTOCOL_HTTPS: {
               //xrsr_state_http_t *http = &dst->conn_state.http;
               break;
            }
            #endif
            #ifdef WS_ENABLED
            case XRSR_PROTOCOL_WS:
            case XRSR_PROTOCOL_WSS: {
               xrsr_state_ws_t *ws = &dst->conn_state.ws;
               xrsr_ws_term(ws);
               break;
            }
            #endif
            #ifdef SDT_ENABLED
            case XRSR_PROTOCOL_SDT: {
               xrsr_state_sdt_t *sdt = &dst->conn_state.sdt;
               xrsr_sdt_term(sdt);
               break;
            }
            #endif

            default: {
               break;
            }
         }
      }
   }

   rdkx_timer_destroy(state.timer_obj);

   return(NULL);
}

int xrsr_msgq_fd_get(void) {
   return(g_xrsr.threads[XRSR_THREAD_MAIN].msgq_id);
}

void xrsr_msg_terminate(const xrsr_thread_params_t *params, xrsr_thread_state_t *state, void *msg) {
   xrsr_queue_msg_term_t *term = (xrsr_queue_msg_term_t *)msg;
   if(term->semaphore != NULL) {
      sem_post(term->semaphore);
   }
   state->running = false;
}

void xrsr_msg_route_update(const xrsr_thread_params_t *params, xrsr_thread_state_t *state, void *msg) {
   xrsr_queue_msg_route_update_t *route_update = (xrsr_queue_msg_route_update_t *)msg;
   xrsr_src_t srcs[XRSR_SRC_INVALID+1];
   uint32_t index = 0;
   do {
      xrsr_src_t src = route_update->routes[index].src;
      if(src >= XRSR_SRC_INVALID) {
         break;
      }
      XLOGD_INFO("%u: src <%s>", index, xrsr_src_str(src));

      xrsr_route_update(route_update->host_name, &route_update->routes[index], state);
      srcs[index] = src;
      index++;
   } while(1);

   srcs[index] = XRSR_SRC_INVALID;

   if(index == 0) {
      XLOGD_INFO("removing all routes");
   }

   xrsr_xraudio_device_update(g_xrsr.xrsr_xraudio_object, srcs);

   if(route_update->semaphore != NULL) {
      sem_post(route_update->semaphore);
   }
}

bool xrsr_session_request(xrsr_src_t src, const char* transcription_in) {
   if(transcription_in == NULL && src != XRSR_SRC_MICROPHONE) {
      XLOGD_INFO("unsupported source <%s>", xrsr_src_str(src));
      return(false);
   }
   xraudio_input_format_t xraudio_format;
   xraudio_format.container   = XRAUDIO_CONTAINER_NONE;
   xraudio_format.encoding    = XRAUDIO_ENCODING_PCM;
   xraudio_format.sample_rate = XRAUDIO_INPUT_DEFAULT_SAMPLE_RATE;
   xraudio_format.sample_size = XRAUDIO_INPUT_DEFAULT_SAMPLE_SIZE;
   xraudio_format.channel_qty = XRAUDIO_INPUT_DEFAULT_CHANNEL_QTY;

   return(xrsr_xraudio_session_request(g_xrsr.xrsr_xraudio_object, src, xraudio_format, transcription_in));
}

bool xrsr_session_keyword_info_set(xrsr_src_t src, uint32_t keyword_begin, uint32_t keyword_duration) {
   if(src != XRSR_SRC_RCU_FF) {
      XLOGD_INFO("unsupported source <%s>", xrsr_src_str(src));
      return(false);
   }
   for(uint32_t index = 0; index < XRSR_DST_QTY_MAX; index++) {
      g_xrsr.routes[src].dsts[index].keyword_begin    = keyword_begin;
      g_xrsr.routes[src].dsts[index].keyword_duration = keyword_duration;
   }
   return(true);
}

bool xrsr_session_capture_start(xrsr_audio_container_t container, const char *file_path, bool raw_mic_enable) {
   if(!g_xrsr.opened) {
      XLOGD_ERROR("not opened");
      return(false);
   }

   sem_t semaphore;
   sem_init(&semaphore, 0, 0);

   // Send the keyword information
   xrsr_queue_msg_session_capture_start_t msg;
   msg.header.type    = XRSR_QUEUE_MSG_TYPE_SESSION_CAPTURE_START;
   msg.semaphore      = &semaphore;
   msg.container      = container;
   msg.file_path      = file_path;
   msg.raw_mic_enable = raw_mic_enable;

   xrsr_queue_msg_push(xrsr_msgq_fd_get(), (const char *)&msg, sizeof(msg));
   sem_wait(&semaphore);
   sem_destroy(&semaphore);

   return(true);
}

bool xrsr_session_capture_stop(void) {
   if(!g_xrsr.opened) {
      XLOGD_ERROR("not opened");
      return(false);
   }

   sem_t semaphore;
   sem_init(&semaphore, 0, 0);

   // Send the keyword information
   xrsr_queue_msg_session_capture_stop_t msg;
   msg.header.type    = XRSR_QUEUE_MSG_TYPE_SESSION_CAPTURE_STOP;
   msg.semaphore      = &semaphore;

   xrsr_queue_msg_push(xrsr_msgq_fd_get(), (const char *)&msg, sizeof(msg));
   sem_wait(&semaphore);
   sem_destroy(&semaphore);

   return(true);
}

void xrsr_session_terminate() {
   sem_t semaphore;
   sem_init(&semaphore, 0, 0);

   xrsr_queue_msg_session_terminate_t terminate;
   terminate.header.type = XRSR_QUEUE_MSG_TYPE_SESSION_TERMINATE;
   terminate.semaphore   = &semaphore;
   xrsr_queue_msg_push(xrsr_msgq_fd_get(), (const char *)&terminate, sizeof(terminate));

   sem_wait(terminate.semaphore);
   sem_destroy(&semaphore);
}

void xrsr_msg_keyword_update(const xrsr_thread_params_t *params, xrsr_thread_state_t *state, void *msg) {
   xrsr_queue_msg_keyword_update_t *keyword_update = (xrsr_queue_msg_keyword_update_t *)msg;
   if(keyword_update->keyword_config == NULL) {
      XLOGD_ERROR("NULL keyword config");
   } else {
      xrsr_xraudio_keyword_detect_params(g_xrsr.xrsr_xraudio_object, XRSR_KEYWORD_PHRASE, (xraudio_keyword_config_t)keyword_update->keyword_config->sensitivity);
   }
   if(keyword_update->semaphore != NULL) {
      sem_post(keyword_update->semaphore);
   }
}

void xrsr_msg_host_name_update(const xrsr_thread_params_t *params, xrsr_thread_state_t *state, void *msg) {
   xrsr_queue_msg_host_name_update_t *host_name_update = (xrsr_queue_msg_host_name_update_t *)msg;

   for(uint32_t index_src = 0; index_src < XRSR_SRC_INVALID; index_src++) {
      for(uint32_t index_dst = 0; index_dst < XRSR_DST_QTY_MAX; index_dst++) {
         xrsr_dst_int_t *dst = &g_xrsr.routes[index_src].dsts[index_dst];

         switch(dst->url_parts.prot) {
            #ifdef HTTP_ENABLED
            case XRSR_PROTOCOL_HTTP:
            case XRSR_PROTOCOL_HTTPS: {
               //xrsr_state_http_t *http = &dst->conn_state.http;
               break;
            }
            #endif
            #ifdef WS_ENABLED
            case XRSR_PROTOCOL_WS:
            case XRSR_PROTOCOL_WSS: {
               xrsr_state_ws_t *ws = &dst->conn_state.ws;
               xrsr_ws_host_name_set(ws, host_name_update->host_name);
               break;
            }
            #endif
            default: {
               break;
            }
         }
      }
   }

   if(host_name_update->semaphore != NULL) {
      sem_post(host_name_update->semaphore);
   }
}

void xrsr_msg_power_mode_update(const xrsr_thread_params_t *params, xrsr_thread_state_t *state, void *msg) {
   xrsr_queue_msg_power_mode_update_t *power_mode_update = (xrsr_queue_msg_power_mode_update_t *)msg;

   XLOGD_INFO("power mode <%s>", xrsr_power_mode_str(power_mode_update->power_mode));

   // Update the dst params for the new power mode
   for(uint32_t index_src = 0; index_src < XRSR_SRC_INVALID; index_src++) {
      for(uint32_t index_dst = 0; index_dst < XRSR_DST_QTY_MAX; index_dst++) {
         xrsr_dst_int_t *dst = &g_xrsr.routes[index_src].dsts[index_dst];

         switch(dst->url_parts.prot) {
            #ifdef WS_ENABLED
            case XRSR_PROTOCOL_WS:
            case XRSR_PROTOCOL_WSS: {
               xrsr_state_ws_t *ws = &dst->conn_state.ws;
               xrsr_ws_update_dst_params(ws, &dst->dst_param_ptrs[power_mode_update->power_mode]);
               break;
            }
            #endif
            default: {
               break;
            }
         }
      }
   }

   bool result = xrsr_xraudio_power_mode_update(g_xrsr.xrsr_xraudio_object, power_mode_update->power_mode);

   if(power_mode_update->semaphore != NULL) {
      if(power_mode_update->result != NULL) {
         *(power_mode_update->result) = result;
      }
      sem_post(power_mode_update->semaphore);
   }
}

void xrsr_msg_privacy_mode_update(const xrsr_thread_params_t *params, xrsr_thread_state_t *state, void *msg) {
   xrsr_queue_msg_privacy_mode_update_t *privacy_mode_update = (xrsr_queue_msg_privacy_mode_update_t *)msg;

   XLOGD_INFO("privacy mode <%s>", privacy_mode_update->enable ? "ENABLE" : "DISABLE");

   bool result = xrsr_xraudio_privacy_mode_update(g_xrsr.xrsr_xraudio_object, privacy_mode_update->enable);

   if(privacy_mode_update->semaphore != NULL) {
      if(privacy_mode_update->result != NULL) {
         *(privacy_mode_update->result) = result;
      }
      sem_post(privacy_mode_update->semaphore);
   }
}

void xrsr_msg_xraudio_granted(const xrsr_thread_params_t *params, xrsr_thread_state_t *state, void *msg) {
   xrsr_xraudio_device_granted(g_xrsr.xrsr_xraudio_object);
}

void xrsr_msg_xraudio_revoked(const xrsr_thread_params_t *params, xrsr_thread_state_t *state, void *msg) {
#ifdef XRAUDIO_RESOURCE_MGMT
   xrsr_xraudio_device_revoked(g_xrsr.xrsr_xraudio_object);
#endif
}

void xrsr_msg_xraudio_event(const xrsr_thread_params_t *params, xrsr_thread_state_t *state, void *msg) {
   xrsr_queue_msg_xraudio_in_event_t *event = (xrsr_queue_msg_xraudio_in_event_t *)msg;

   if((uint32_t)g_xrsr.src >= XRSR_SRC_INVALID) {
      XLOGD_ERROR("invalid source <%s>", xrsr_src_str(g_xrsr.src));
      return;
   }

   if(event->event.event != XRSR_EVENT_INVALID) {
      uint32_t index_src = g_xrsr.src;
      for(uint32_t index_dst = 0; index_dst < XRSR_DST_QTY_MAX; index_dst++) {
         xrsr_dst_int_t *dst = &g_xrsr.routes[index_src].dsts[index_dst];

         switch(dst->url_parts.prot) {
            #ifdef HTTP_ENABLED
            case XRSR_PROTOCOL_HTTP:
            case XRSR_PROTOCOL_HTTPS: {
               xrsr_state_http_t *http = &dst->conn_state.http;
               xrsr_http_handle_speech_event(http, &event->event);
               break;
            }
            #endif
            #ifdef WS_ENABLED
            case XRSR_PROTOCOL_WS:
            case XRSR_PROTOCOL_WSS: {
               xrsr_state_ws_t *ws = &dst->conn_state.ws;
               xrsr_ws_handle_speech_event(ws, &event->event);
               break;
            }
            #endif
            #ifdef SDT_ENABLED
            case XRSR_PROTOCOL_SDT: {
               xrsr_state_sdt_t *sdt = &dst->conn_state.sdt;
               xrsr_sdt_handle_speech_event(sdt, &event->event);
               break;
            }
            #endif
            default: {
               break;
            }
         }
      }
   }
}

void xrsr_msg_keyword_detected(const xrsr_thread_params_t *params, xrsr_thread_state_t *state, void *msg) {
   xrsr_queue_msg_keyword_detected_t *keyword_detected = (xrsr_queue_msg_keyword_detected_t *)msg;
   xrsr_xraudio_keyword_detected(g_xrsr.xrsr_xraudio_object, keyword_detected, g_xrsr.src);
}

void xrsr_msg_keyword_detect_error(const xrsr_thread_params_t *params, xrsr_thread_state_t *state, void *msg) {
   xrsr_queue_msg_keyword_detected_t *keyword_detected = (xrsr_queue_msg_keyword_detected_t *)msg;
   xrsr_xraudio_keyword_detect_error(g_xrsr.xrsr_xraudio_object, keyword_detected->source);
}

void xrsr_msg_session_begin(const xrsr_thread_params_t *params, xrsr_thread_state_t *state, void *msg) {
   xrsr_queue_msg_session_begin_t *begin = (xrsr_queue_msg_session_begin_t *)msg;

   if(begin->src >= XRSR_SRC_INVALID) {
      XLOGD_ERROR("invalid source <%s>", xrsr_src_str(begin->src));
      return;
   }
   if(g_xrsr.src == begin->src && !begin->retry) { // Keyword was triggered again from same source while previous session is in progress
      #ifdef XRSR_SESSION_RETRIGGER_ABORT
      // terminate current session and start a new one
      XLOGD_INFO("aborting current session in progress on source <%s>", xrsr_src_str(g_xrsr.src));
      xrsr_queue_msg_session_terminate_t terminate;
      terminate.header.type = XRSR_QUEUE_MSG_TYPE_SESSION_TERMINATE;
      terminate.semaphore   = NULL;
      xrsr_msg_session_terminate(params, state, &terminate);

      // TODO Need to set a flag to restart a new session immediately after the current session terminates

      #else // ignore the keyword detection and restart the detector
      XLOGD_INFO("ignoring due to current session in progress on source <%s>", xrsr_src_str(g_xrsr.src));

      // Need to restart the keyword detector again
      xrsr_xraudio_keyword_detect_restart(g_xrsr.xrsr_xraudio_object);
      #endif
      return;
   }
   if((uint32_t)g_xrsr.src < XRSR_SRC_INVALID && !begin->retry) {
      XLOGD_ERROR("session in progress on source <%s>", xrsr_src_str(g_xrsr.src));
      return;
   }
   g_xrsr.src = begin->src;

   xrsr_keyword_detector_result_t *detector_result_ptr = NULL;
   xrsr_keyword_detector_result_t  detector_result;
   if(begin->has_result) {
      if(begin->detector_result.chan_selected >= XRAUDIO_INPUT_MAX_CHANNEL_QTY) {
         XLOGD_ERROR("invalid selected channel <%u>", begin->detector_result.chan_selected);
      } else {
         detector_result.score            = begin->detector_result.channels[begin->detector_result.chan_selected].score;
         detector_result.snr              = begin->detector_result.channels[begin->detector_result.chan_selected].snr;
         detector_result.doa              = begin->detector_result.channels[begin->detector_result.chan_selected].doa;
         detector_result.offset_buf_begin = begin->detector_result.endpoints.pre;
         detector_result.offset_kwd_begin = begin->detector_result.endpoints.begin;
         detector_result.offset_kwd_end   = begin->detector_result.endpoints.end;
         detector_result.detector_name    = begin->detector_result.detector_name;
         detector_result.dsp_name         = begin->detector_result.dsp_name;

         detector_result_ptr   = &detector_result;

         XLOGD_INFO("selected kwd channel <%u>", begin->detector_result.chan_selected);
         for(uint32_t chan = 0; chan < XRAUDIO_INPUT_MAX_CHANNEL_QTY; chan++) {
            xraudio_kwd_chan_result_t *chan_result = &begin->detector_result.channels[chan];
            if(chan_result->score >= 0.0) {
               XLOGD_INFO("chan <%u> score <%0.6f> snr <%0.4f> doa <%u>", chan, chan_result->score, chan_result->snr, chan_result->doa);
            }
         }
      }
   }

   const char *transcription_in = (begin->transcription_in[0] == '\0') ? NULL : begin->transcription_in;

   for(uint32_t dst_index = 0; dst_index < XRSR_DST_QTY_MAX; dst_index++) {
      xrsr_dst_int_t *dst = &g_xrsr.routes[g_xrsr.src].dsts[dst_index];

      if((uint32_t)g_xrsr.src >= XRSR_SRC_INVALID) { // Source can be released by index 0
         break;
      }
      if(dst->handler == NULL) {
         continue;
      }
      xrsr_protocol_t prot = dst->url_parts.prot;

      switch(prot) {
         #ifdef HTTP_ENABLED
         case XRSR_PROTOCOL_HTTP:
         case XRSR_PROTOCOL_HTTPS: {
            int pipe_fd_read = -1;
            xrsr_state_http_t *http = &dst->conn_state.http;
            if(!xrsr_http_is_disconnected(http)) {
               XLOGD_ERROR("invalid state");
               break;
            }

            xrsr_session_configuration_http_t *session_config = &http->session_configuration.http;
            memset(&http->session_configuration, 0, sizeof(http->session_configuration));

            uuid_generate(session_config->uuid);
            char uuid_str[37] = {'\0'};
            uuid_unparse_lower(session_config->uuid, uuid_str);

            session_config->format = xrsr_audio_format_get(dst->format, begin->xraudio_format);

            XLOGD_INFO("src <%s(%u)> prot <%s> uuid <%s> format <%s>", xrsr_src_str(g_xrsr.src), dst_index, xrsr_protocol_str(prot), uuid_str, xrsr_audio_format_str(session_config->format));

            // Set the handlers based on source
            http->handlers  = dst->handlers;
            http->dst_index = dst_index;

            session_config->query_strs = NULL;

            // Call session begin handler
            if(http->handlers.session_begin != NULL) {
               (*http->handlers.session_begin)(http->handlers.data, session_config->uuid, g_xrsr.src, dst_index, detector_result_ptr, &http->session_configuration, &begin->timestamp, transcription_in);
            }

            if( (XRSR_SRC_MICROPHONE == g_xrsr.src) && (g_xrsr.power_mode == XRSR_POWER_MODE_LOW) ) {
               g_xrsr.routes[g_xrsr.src].dsts[0].keyword_begin    = session_config->keyword_begin;
               g_xrsr.routes[g_xrsr.src].dsts[0].keyword_duration = session_config->keyword_duration;
            }

            bool deferred = ((dst->stream_time_min > 0) && (transcription_in == NULL)) ? true : false;

            if(!xrsr_speech_stream_begin(session_config->uuid, g_xrsr.src, dst_index, begin->xraudio_format, begin->user_initiated, &pipe_fd_read)) {
               XLOGD_ERROR("xrsr_speech_stream_begin failed");
            } else if(!xrsr_http_connect(http, &dst->url_parts, g_xrsr.src, begin->xraudio_format, state->timer_obj, deferred, session_config->query_strs, transcription_in)) {
               XLOGD_ERROR("http connect failed");
            } else {
               http->audio_pipe_fd_read = pipe_fd_read;
            }
            break;
         }
         #endif
         #ifdef WS_ENABLED
         case XRSR_PROTOCOL_WS:
         case XRSR_PROTOCOL_WSS: {
            xrsr_state_ws_t *ws = &dst->conn_state.ws;
            if(xrsr_ws_is_disconnected(ws)) {
               xrsr_session_configuration_ws_t *session_config = &ws->session_configuration.ws;

               if(!begin->retry) { // Only generate new uuid if it's not a retry attempt
                  uuid_generate(session_config->uuid);
                  ws->stream_time_min_rxd = false;
               }
               char uuid_str[37] = {'\0'};
               uuid_unparse_lower(session_config->uuid, uuid_str);

               session_config->format = xrsr_audio_format_get(dst->format, begin->xraudio_format);

               XLOGD_INFO("src <%s(%u)> prot <%s> uuid <%s> format <%s>", xrsr_src_str(g_xrsr.src), dst_index, xrsr_protocol_str(prot), uuid_str, xrsr_audio_format_str(session_config->format));

               // Set the handlers based on source
               ws->handlers  = dst->handlers;
               ws->dst_index = dst_index;

               // Call session begin handler
               session_config->user_initiated = begin->user_initiated;
               session_config->query_strs     = NULL;

               const char *sat_token = NULL;
               if(!begin->retry && ws->handlers.session_begin != NULL) {
                  (*ws->handlers.session_begin)(ws->handlers.data, session_config->uuid, g_xrsr.src, dst_index, detector_result_ptr, &ws->session_configuration, &begin->timestamp, transcription_in);
                  if(ws->session_configuration.ws.sat_token[0] != '\0') {
                     sat_token = ws->session_configuration.ws.sat_token;
                  }
                  if( (XRSR_SRC_MICROPHONE == g_xrsr.src) && (g_xrsr.power_mode == XRSR_POWER_MODE_LOW) ) {
                      g_xrsr.routes[g_xrsr.src].dsts[0].keyword_begin    = session_config->keyword_begin;
                      g_xrsr.routes[g_xrsr.src].dsts[0].keyword_duration = session_config->keyword_duration;
                   }
               }

               if(!begin->retry) { // start streaming audio to the pipe
                  int pipe_fd_read = -1;
                  if(!xrsr_speech_stream_begin(session_config->uuid, g_xrsr.src, ws->dst_index, begin->xraudio_format, begin->user_initiated, &pipe_fd_read)) {
                     XLOGD_ERROR("xrsr_speech_stream_begin failed");
                     // perform clean up of the session
                     xrsr_ws_speech_session_end(ws, XRSR_SESSION_END_REASON_ERROR_AUDIO_BEGIN);
                     break;
                  } else {
                     ws->audio_pipe_fd_read = pipe_fd_read;
                  }
               }

               bool deferred = ((dst->stream_time_min == 0) || transcription_in != NULL) ? false : !ws->stream_time_min_rxd;

               if(!xrsr_ws_connect(ws, &dst->url_parts, g_xrsr.src, begin->xraudio_format, begin->user_initiated, begin->retry, deferred, sat_token, session_config->query_strs)) {
                  XLOGD_ERROR("ws connect");
               }
            } else if(xrsr_ws_is_established(ws)) {
               XLOGD_INFO("ws session continue");
               if(!xrsr_ws_audio_stream(ws, g_xrsr.src)) {
                  XLOGD_ERROR("ws audio stream");
               }
            } else {
               XLOGD_ERROR("invalid state");
            }
            break;
         }
         #endif

         #ifdef SDT_ENABLED
         case XRSR_PROTOCOL_SDT: {
            xrsr_state_sdt_t *sdt = &dst->conn_state.sdt;
            if(xrsr_sdt_is_disconnected(sdt)){
               xrsr_session_configuration_sdt_t *session_config = &sdt->session_configuration.sdt;
               uuid_generate(session_config->uuid);
               sdt->stream_time_min_rxd = false;
               
               char uuid_str[37] = {'\0'};
               uuid_unparse_lower(session_config->uuid, uuid_str);

               session_config->format = xrsr_audio_format_get(dst->format, begin->xraudio_format);

               XLOGD_INFO("src <%s(%u)> prot <%s> uuid <%s> format <%s>", xrsr_src_str(g_xrsr.src), dst_index, xrsr_protocol_str(prot), uuid_str, xrsr_audio_format_str(session_config->format));

               // Set the handlers based on source
               sdt->handlers  = dst->handlers;
               sdt->dst_index = dst_index;

               // Call session begin handler
               session_config->user_initiated = begin->user_initiated;

               int pipe_fd_read = -1;
               if(!xrsr_speech_stream_begin(session_config->uuid, g_xrsr.src, sdt->dst_index, begin->xraudio_format, begin->user_initiated, &pipe_fd_read)) {
                  XLOGD_ERROR("xrsr_speech_stream_begin failed");
                  // perform clean up of the session
                  xrsr_sdt_speech_session_end(sdt, XRSR_SESSION_END_REASON_ERROR_AUDIO_BEGIN);
                  break;
               } else {
                  sdt->audio_pipe_fd_read = pipe_fd_read;
               }
               
               bool deferred = (dst->stream_time_min == 0) ? false : !sdt->stream_time_min_rxd;

               if(!xrsr_sdt_connect(sdt, &dst->url_parts, g_xrsr.src, begin->xraudio_format, begin->user_initiated, begin->retry, deferred, NULL, NULL)) {
                  XLOGD_ERROR("sdt connect");
               }
            } else if(xrsr_sdt_is_established(sdt)) {
               XLOGD_INFO("sdt session continue");
               if(!xrsr_sdt_audio_stream(sdt, g_xrsr.src)) {
                  XLOGD_ERROR("sdt audio stream");
               }
            } else {
               XLOGD_ERROR("invalid state");
            }
           break;
         }
         #endif
         default: {
            XLOGD_ERROR("invalid protocol <%s>", xrsr_protocol_str(prot));
            return;
         }
      }
   }
}

void xrsr_session_end(const uuid_t uuid, const char *uuid_str, xrsr_src_t src, uint32_t dst_index, xrsr_session_stats_t *stats) {
   rdkx_timestamp_t timestamp;
   rdkx_timestamp_get_realtime(&timestamp);

   XLOGD_INFO("uuid <%s> src <%s> dst index <%u>", uuid_str, xrsr_src_str(src), dst_index);

   if(((uint32_t) src) >= (uint32_t)XRSR_SRC_INVALID) {
      XLOGD_ERROR("invalid source <%s>", xrsr_src_str(src));
      return;
   }
   if((uint32_t)g_xrsr.src != src) {
      XLOGD_ERROR("source <%s> is not active source <%s>", xrsr_src_str(src), xrsr_src_str(g_xrsr.src));
      return;
   }
   if(dst_index >= XRSR_DST_QTY_MAX || g_xrsr.routes[src].dsts[dst_index].handler == NULL) {
      XLOGD_ERROR("source <%s> invalid dst index <%u>", xrsr_src_str(src), dst_index);
      return;
   }

   xrsr_dst_int_t *dst = &g_xrsr.routes[src].dsts[dst_index];

   // Call session end handler
   if(dst->handlers.session_end != NULL) {
      (*dst->handlers.session_end)(dst->handlers.data, uuid, stats, &timestamp);
   } else {
      XLOGD_DEBUG("no session end handler");
   }

   // check state of each dst to determine if session if overall session is completed
   bool session_in_progress = false;
   for(uint32_t dst_index = 0; dst_index < XRSR_DST_QTY_MAX; dst_index++) {
      xrsr_dst_int_t *dst = &g_xrsr.routes[g_xrsr.src].dsts[dst_index];
      if(dst->handler == NULL) {
         continue;
      }
      xrsr_protocol_t prot = dst->url_parts.prot;

      switch(prot) {
         #ifdef HTTP_ENABLED
         case XRSR_PROTOCOL_HTTP:
         case XRSR_PROTOCOL_HTTPS: {
            xrsr_state_http_t *http = &dst->conn_state.http;
            if(!xrsr_http_is_disconnected(http)) {
               session_in_progress = true;
            }
            break;
         }
         #endif
         #ifdef WS_ENABLED
         case XRSR_PROTOCOL_WS:
         case XRSR_PROTOCOL_WSS: {
            xrsr_state_ws_t *ws = &dst->conn_state.ws;
            if(!xrsr_ws_is_disconnected(ws)) {
               session_in_progress = true;
            }
            break;
         }
         #endif
         #ifdef SDT_ENABLED
         case XRSR_PROTOCOL_SDT: {
            xrsr_state_sdt_t *sdt = &dst->conn_state.sdt;
            if(!xrsr_sdt_is_disconnected(sdt)) {
               session_in_progress = true;
            }
            break;
         }
         #endif
         default: {
         }
      }
   }

   if(!session_in_progress) {
      g_xrsr.src = XRSR_SRC_INVALID;
   }
}

void xrsr_msg_session_terminate(const xrsr_thread_params_t *params, xrsr_thread_state_t *state, void *msg) {
   xrsr_queue_msg_session_terminate_t *terminate = (xrsr_queue_msg_session_terminate_t *)msg;

   if((uint32_t)g_xrsr.src >= XRSR_SRC_INVALID) {
      XLOGD_ERROR("source is not active <%s>", xrsr_src_str(g_xrsr.src));
      if(terminate->semaphore != NULL) {
         sem_post(terminate->semaphore);
      }
      return;
   }

   uint32_t index_src = g_xrsr.src;
   for(uint32_t index_dst = 0; index_dst < XRSR_DST_QTY_MAX; index_dst++) {
      xrsr_dst_int_t *dst = &g_xrsr.routes[index_src].dsts[index_dst];

      switch(dst->url_parts.prot) {
         #ifdef HTTP_ENABLED
         case XRSR_PROTOCOL_HTTP:
         case XRSR_PROTOCOL_HTTPS: {
            xrsr_state_http_t *http = &dst->conn_state.http;
            XLOGD_INFO("http");
            if(!xrsr_http_is_disconnected(http)) {
               xrsr_http_terminate(http);
            }
            break;
         }
         #endif
         #ifdef WS_ENABLED
         case XRSR_PROTOCOL_WS:
         case XRSR_PROTOCOL_WSS: {
            xrsr_state_ws_t *ws = &dst->conn_state.ws;
            if(!xrsr_ws_is_disconnected(ws)) {
               xrsr_ws_terminate(ws);
            }
            break;
         }
         #endif
         #ifdef SDT_ENABLED
         case XRSR_PROTOCOL_SDT: {
            xrsr_state_sdt_t *sdt = &dst->conn_state.sdt;
            if(!xrsr_sdt_is_disconnected(sdt)) {
               xrsr_sdt_terminate(sdt);
            }
            break;
         }
         #endif
         default: {
            break;
         }
      }
   }

   if(terminate->semaphore != NULL) {
      sem_post(terminate->semaphore);
   }
}

void xrsr_session_stream_begin(const uuid_t uuid, const char *uuid_str, xrsr_src_t src, uint32_t dst_index) {
   rdkx_timestamp_t timestamp;
   rdkx_timestamp_get_realtime(&timestamp);

   XLOGD_INFO("uuid <%s> src <%s> dst index <%u>", uuid_str, xrsr_src_str(src), dst_index);

   if(((uint32_t) src) >= (uint32_t)XRSR_SRC_INVALID) {
      XLOGD_ERROR("invalid source <%s>", xrsr_src_str(src));
      return;
   }
   if(dst_index >= XRSR_DST_QTY_MAX || g_xrsr.routes[src].dsts[dst_index].handler == NULL) {
      XLOGD_ERROR("source <%s> invalid dst index <%u>", xrsr_src_str(src), dst_index);
      return;
   }

   xrsr_dst_int_t *dst = &g_xrsr.routes[src].dsts[dst_index];

   // Call session stream begin handler
   if(dst->handlers.stream_begin != NULL) {
      (*dst->handlers.stream_begin)(dst->handlers.data, uuid, src, &timestamp);
   } else {
      XLOGD_DEBUG("no stream begin handler");
   }
}

void xrsr_session_stream_kwd(const uuid_t uuid, const char *uuid_str, xrsr_src_t src, uint32_t dst_index) {
   rdkx_timestamp_t timestamp;
   rdkx_timestamp_get_realtime(&timestamp);

   XLOGD_INFO("uuid <%s> src <%s> dst index <%u>", uuid_str, xrsr_src_str(src), dst_index);

   if(((uint32_t) src) >= (uint32_t)XRSR_SRC_INVALID) {
      XLOGD_ERROR("invalid source <%s>", xrsr_src_str(src));
      return;
   }
   if(dst_index >= XRSR_DST_QTY_MAX || g_xrsr.routes[src].dsts[dst_index].handler == NULL) {
      XLOGD_ERROR("source <%s> invalid dst index <%u>", xrsr_src_str(src), dst_index);
      return;
   }

   xrsr_dst_int_t *dst = &g_xrsr.routes[src].dsts[dst_index];

   // Call session stream kwd handler
   if(dst->handlers.stream_kwd != NULL) {
      (*dst->handlers.stream_kwd)(dst->handlers.data, uuid, &timestamp);
   } else {
      XLOGD_DEBUG("no stream keyword handler");
   }
}

void xrsr_session_stream_end(const uuid_t uuid, const char *uuid_str, xrsr_src_t src, uint32_t dst_index, xrsr_stream_stats_t *stats) {
   rdkx_timestamp_t timestamp;
   rdkx_timestamp_get_realtime(&timestamp);

   XLOGD_INFO("uuid <%s> src <%s>", uuid_str, xrsr_src_str(src));

   if(((uint32_t) src) >= (uint32_t)XRSR_SRC_INVALID) {
      XLOGD_ERROR("invalid source <%s>", xrsr_src_str(src));
      return;
   }
   if(dst_index >= XRSR_DST_QTY_MAX || g_xrsr.routes[src].dsts[dst_index].handler == NULL) {
      XLOGD_ERROR("source <%s> invalid dst index <%u>", xrsr_src_str(src), dst_index);
      return;
   }

   xrsr_dst_int_t *dst = &g_xrsr.routes[src].dsts[dst_index];

   // Call session stream end handler
   if(dst->handlers.stream_end != NULL) {
      (*dst->handlers.stream_end)(dst->handlers.data, uuid, stats, &timestamp);
   } else {
      XLOGD_DEBUG("no stream end handler");
   }
}

void xrsr_msg_session_capture_start(const xrsr_thread_params_t *params, xrsr_thread_state_t *state, void *msg) {
   xrsr_queue_msg_session_capture_start_t *capture_start = (xrsr_queue_msg_session_capture_start_t *)msg;


   xrsr_xraudio_session_capture_start(g_xrsr.xrsr_xraudio_object, capture_start->container, capture_start->file_path, capture_start->raw_mic_enable);

   if(capture_start->semaphore != NULL) {
      sem_post(capture_start->semaphore);
   }
}

void xrsr_msg_session_capture_stop(const xrsr_thread_params_t *params, xrsr_thread_state_t *state, void *msg) {
   xrsr_queue_msg_session_capture_stop_t *capture_stop = (xrsr_queue_msg_session_capture_stop_t *)msg;


   xrsr_xraudio_session_capture_stop(g_xrsr.xrsr_xraudio_object);

   if(capture_stop->semaphore != NULL) {
      sem_post(capture_stop->semaphore);
   }
}

void xrsr_msg_privacy_mode_get(const xrsr_thread_params_t *params, xrsr_thread_state_t *state, void *msg) {
   xrsr_queue_msg_privacy_mode_get_t *privacy_mode_get = (xrsr_queue_msg_privacy_mode_get_t *)msg;

   bool result = xrsr_xraudio_privacy_mode_get(g_xrsr.xrsr_xraudio_object, privacy_mode_get->enabled);

   if(privacy_mode_get->semaphore != NULL) {
      if(privacy_mode_get->result != NULL) {
         *(privacy_mode_get->result) = result;
      }
      sem_post(privacy_mode_get->semaphore);
   }
}
void xrsr_send_stream_data(xrsr_src_t src, uint8_t *buffer, uint32_t size)
{
  if((uint32_t)src >= (uint32_t)XRSR_SRC_INVALID) {
     XLOGD_ERROR("invalid source <%s>", xrsr_src_str(src));
     return;
   }

   for(uint32_t dst_index = 0; dst_index < XRSR_DST_QTY_MAX; dst_index++) {
      xrsr_dst_int_t *dst = &g_xrsr.routes[src].dsts[dst_index];

      if(dst->handler == NULL) {
         if(dst_index == 0) {
            XLOGD_ERROR("no handler for source <%s>", xrsr_src_str(src));
         }
         return;
      }
      // Call source send audio handler
      if(dst->handlers.stream_audio != NULL) {
         (*dst->handlers.stream_audio)(buffer,size);
      }
   }
}

void xrsr_session_begin(xrsr_src_t src, bool user_initiated, xraudio_input_format_t xraudio_format, xraudio_keyword_detector_result_t *detector_result, const char *transcription_in) {
   if((uint32_t)src >= (uint32_t)XRSR_SRC_INVALID) {
      XLOGD_ERROR("invalid source <%s>", xrsr_src_str(src));
      return;
   }

   // TODO Only the handler for dst index 0 is called.  Really this needs to be changed so that each protocol doesn't need to get called here.
   for(uint32_t dst_index = 0; dst_index < 1; dst_index++) {
      xrsr_dst_int_t *dst = &g_xrsr.routes[src].dsts[dst_index];

      if(dst->handler == NULL) {
         XLOGD_ERROR("no handler for source <%s> dst index <%u>", xrsr_src_str(src), dst_index);
         return;
      }
      (*dst->handler)(src, false, user_initiated, xraudio_format, detector_result, transcription_in);
   }
}

void xrsr_keyword_detect_error(xrsr_src_t src) {
   if((uint32_t)src >= (uint32_t)XRSR_SRC_INVALID) {
      XLOGD_ERROR("invalid source <%s>", xrsr_src_str(src));
      return;
   }

   for(uint32_t dst_index = 0; dst_index < XRSR_DST_QTY_MAX; dst_index++) {
      xrsr_dst_int_t *dst = &g_xrsr.routes[src].dsts[dst_index];

      if(dst->handler == NULL) {
         if(dst_index == 0) {
            XLOGD_ERROR("no handler for source <%s>", xrsr_src_str(src));
         }
         return;
      }

      // Call source error handler
      if(dst->handlers.source_error != NULL) {
         (*dst->handlers.source_error)(dst->handlers.data, src);
      }
   }
}

xrsr_result_t xrsr_conn_send(void *param, const uint8_t *buffer, uint32_t length) {
   xrsr_protocol_t *prot = (xrsr_protocol_t *)param;
   switch(*prot) {
      #ifdef WS_ENABLED
      case XRSR_PROTOCOL_WS:
      case XRSR_PROTOCOL_WSS: {
         xrsr_state_ws_t *ws = (xrsr_state_ws_t *)param;
         xrsr_ws_send_text(ws, buffer, length);
         break;
      }
      #endif
      #ifdef HTTP_ENABLED
      case XRSR_PROTOCOL_HTTP:
      case XRSR_PROTOCOL_HTTPS: {
         xrsr_state_http_t *http = (xrsr_state_http_t *)param;
         xrsr_http_send(http, buffer, length);
         break;
      }
      #endif
      #ifdef SDT_ENABLED
      case XRSR_PROTOCOL_SDT: {
        xrsr_state_sdt_t *sdt = (xrsr_state_sdt_t *)param;
        xrsr_sdt_send_text(sdt, buffer, length);
         break;
      }
      #endif
      default: {
         XLOGD_ERROR("protocol not supportted");
         break;
      }
   }
   // TODO process return code
   return(XRSR_RESULT_SUCCESS);
}

bool xrsr_speech_stream_begin(const uuid_t uuid, xrsr_src_t src, uint32_t dst_index, xraudio_input_format_t native_format, bool user_initiated, int *pipe_fd_read) {
   if(!g_xrsr.first_stream_req) { // return the pipe for this destination
      *pipe_fd_read = g_xrsr.pipe_fds_rd[dst_index];
      return(true);
   }
   g_xrsr.first_stream_req = false;

   xraudio_dst_pipe_t dsts[XRSR_DST_QTY_MAX];

   // create pipe for each destination
   for(uint32_t index = 0; index < XRSR_DST_QTY_MAX; index++) {
      xrsr_dst_int_t *dst = &g_xrsr.routes[src].dsts[index];

      if(dst->handler == NULL) {
         g_xrsr.pipe_fds_rd[index] = -1;
         dsts[index].pipe          = -1;
         dsts[index].from          = XRAUDIO_INPUT_RECORD_FROM_INVALID;
         dsts[index].offset        = 0;
         dsts[index].until         = XRAUDIO_INPUT_RECORD_UNTIL_INVALID;
         break;
      }

      int pipe_fds[2];

      errno = 0;
      if(pipe(pipe_fds) == -1) {
         int errsv = errno;
         XLOGD_ERROR("unable to create pipe <%s>", strerror(errsv));
         g_xrsr.first_stream_req = true;
         return(false);
      }

      // Hold up to X milliseconds of audio in the pipe
      uint32_t duration = 10000;
      uint32_t size     = (XRAUDIO_INPUT_DEFAULT_SAMPLE_RATE * XRAUDIO_INPUT_DEFAULT_SAMPLE_SIZE * XRAUDIO_INPUT_DEFAULT_CHANNEL_QTY * duration) / 1000;

      int rc = fcntl(pipe_fds[1], F_SETPIPE_SZ, size);
      if(rc != size) {
         XLOGD_WARN("set pipe size failed exp <%u> rxd <%d>", size, rc);
      } else {
         XLOGD_INFO("set pipe size %u ms (%u bytes)", duration, size);
      }

      g_xrsr.pipe_fds_rd[index] = pipe_fds[0];
      dsts[index].pipe          = pipe_fds[1];
      dsts[index].from          = dst->stream_from;
      dsts[index].offset        = dst->stream_offset;
      dsts[index].until         = dst->stream_until;
   }

   xraudio_devices_input_t source;

   switch(src) {
      case XRSR_SRC_RCU_PTT:    { source = XRAUDIO_DEVICE_INPUT_PTT;    break; }
      case XRSR_SRC_RCU_FF:     { source = XRAUDIO_DEVICE_INPUT_FF;     break; }
      case XRSR_SRC_MICROPHONE: { source = XRAUDIO_DEVICE_INPUT_SINGLE; break; }
      default: {
         XLOGD_ERROR("invalid src <%s>", xrsr_src_str(src));
         g_xrsr.first_stream_req = true;
         return(false);
      }
   }

   xrsr_dst_int_t *dst = &g_xrsr.routes[src].dsts[dst_index];

   xraudio_input_format_t xraudio_format = native_format;

   switch(dst->format) {
      case XRSR_AUDIO_FORMAT_PCM:    { xraudio_format.encoding = XRAUDIO_ENCODING_PCM;   xraudio_format.sample_size = 2; break; }
      case XRSR_AUDIO_FORMAT_ADPCM:  { xraudio_format.encoding = XRAUDIO_ENCODING_ADPCM; break; }
      case XRSR_AUDIO_FORMAT_OPUS:   { xraudio_format.encoding = XRAUDIO_ENCODING_OPUS;  break; }
      case XRSR_AUDIO_FORMAT_NATIVE: { break; }
      default: {
         xraudio_format.encoding = XRAUDIO_ENCODING_INVALID;
         break;
      }
   }

   char uuid_str[37] = {'\0'};
   uuid_unparse_lower(uuid, uuid_str);

   uint32_t frame_duration;
   if (XRAUDIO_DEVICE_INPUT_EXTERNAL_GET(source)) {
      if (native_format.encoding == XRAUDIO_ENCODING_ADPCM_SKY) {
         frame_duration = 1000 * 1000 * XRAUDIO_INPUT_ADPCM_SKY_FRAME_SAMPLE_QTY / xraudio_format.sample_rate;
      } else {
         frame_duration = 1000 * 1000 * XRAUDIO_INPUT_ADPCM_XVP_FRAME_SAMPLE_QTY / xraudio_format.sample_rate;
      }
   } else {
      frame_duration = XRAUDIO_INPUT_FRAME_PERIOD * 1000;
   }
   
   // Make a single call to start streaming to all destinations
   if(!xrsr_xraudio_stream_begin(g_xrsr.xrsr_xraudio_object, uuid_str, source, user_initiated, &xraudio_format, dsts, dst->stream_time_min, user_initiated ? 0 : dst->keyword_begin, user_initiated ? 0 : dst->keyword_duration, frame_duration)) {
      for(uint32_t index = 0; index < XRSR_DST_QTY_MAX; index++) {
         if(dsts[index].pipe >= 0) {
            close(dsts[index].pipe);
         }
         if(g_xrsr.pipe_fds_rd[index] >= 0) {
            close(g_xrsr.pipe_fds_rd[index]);
            g_xrsr.pipe_fds_rd[index] = -1;
         }
      }
      g_xrsr.first_stream_req = true;
      XLOGD_ERROR("xrsr_xraudio_stream_begin failed");
      return(false);
   }
   *pipe_fd_read = g_xrsr.pipe_fds_rd[dst_index];

   return(true);
}

bool xrsr_speech_stream_kwd(const uuid_t uuid, xrsr_src_t src, uint32_t dst_index) {
   char uuid_str[37] = {'\0'};
   uuid_unparse_lower(uuid, uuid_str);

   xrsr_session_stream_kwd(uuid, uuid_str, src, dst_index);

   return(true);
}

bool xrsr_speech_stream_end(const uuid_t uuid, xrsr_src_t src, uint32_t dst_index, xrsr_stream_end_reason_t reason, bool detect_resume, xrsr_audio_stats_t *audio_stats) {
   bool result = true;

   XLOGD_INFO("src <%s> dst index <%u> reason <%s> detect resume <%s>", xrsr_src_str(src), dst_index, xrsr_stream_end_reason_str(reason), detect_resume ? "YES" : "NO");

   if(((uint32_t) src) >= (uint32_t)XRSR_SRC_INVALID) {
      XLOGD_ERROR("invalid source <%s>", xrsr_src_str(src));
      return(false);
   }
   if(dst_index >= XRSR_DST_QTY_MAX || g_xrsr.routes[src].dsts[dst_index].handler == NULL) {
      XLOGD_ERROR("source <%s> invalid dst index <%u>", xrsr_src_str(src), dst_index);
      return(false);
   }

   // Need to know when all streams have ended to call xraudio stream end?
   g_xrsr.pipe_fds_rd[dst_index] = -1;

   bool more_streams = false;
   for(uint32_t index = 0; index < XRSR_DST_QTY_MAX; index++) {
      if(g_xrsr.pipe_fds_rd[index] >= 0) {
         more_streams = true;
      }
   }

   if(!xrsr_xraudio_stream_end(g_xrsr.xrsr_xraudio_object, dst_index, more_streams, detect_resume, audio_stats)) {
      XLOGD_ERROR("xrsr_xraudio_stream_end failed");
      result = false;
   }

   if(!more_streams) {
      g_xrsr.first_stream_req = true;
   }

   xrsr_dst_int_t *dst = &g_xrsr.routes[src].dsts[dst_index];

   if(reason != XRSR_STREAM_END_REASON_DID_NOT_BEGIN) { // the stream was started
      char uuid_str[37] = {'\0'};
      uuid_unparse_lower(uuid, uuid_str);

      xrsr_stream_stats_t stats;
      memset(&stats, 0, sizeof(stats));

      stats.result = (reason == XRSR_STREAM_END_REASON_AUDIO_EOF) ? true : false;
      stats.prot   = dst->url_parts.prot;
      if(audio_stats) {
         stats.audio_stats = *audio_stats;
      }
      xrsr_session_stream_end(uuid, uuid_str, src, dst_index, &stats);
   }

   return(result);
}

void xrsr_thread_poll(xrsr_thread_poll_func_t func) {
   xrsr_queue_msg_thread_poll_t msg;
   msg.header.type  = XRSR_QUEUE_MSG_TYPE_THREAD_POLL;
   msg.func         = func;
   xrsr_queue_msg_push(xrsr_msgq_fd_get(), (const char *)&msg, sizeof(msg));
}

void xrsr_msg_thread_poll(const xrsr_thread_params_t *params, xrsr_thread_state_t *state, void *msg) {
   xrsr_queue_msg_thread_poll_t *thread_poll = (xrsr_queue_msg_thread_poll_t *)msg;

   if(thread_poll == NULL || thread_poll->func == NULL) {
      return;
   }

   if(g_xrsr.xrsr_xraudio_object == NULL) { // xraudio is not open.  call poll response directly.
      (*thread_poll->func)();
   } else { // send thread poll to xraudio
      xrsr_xraudio_thread_poll(g_xrsr.xrsr_xraudio_object, thread_poll->func);
   }
}

xrsr_audio_format_t xrsr_audio_format_get(xrsr_audio_format_t format_dst, xraudio_input_format_t format_src) {
   switch(format_dst) {
      case XRSR_AUDIO_FORMAT_PCM:    { return(XRSR_AUDIO_FORMAT_PCM); }
      case XRSR_AUDIO_FORMAT_ADPCM:  { return(XRSR_AUDIO_FORMAT_ADPCM); }
      case XRSR_AUDIO_FORMAT_OPUS:   { return(XRSR_AUDIO_FORMAT_OPUS); }
      case XRSR_AUDIO_FORMAT_NATIVE: {
         if(format_src.encoding == XRAUDIO_ENCODING_PCM) {
            return(XRSR_AUDIO_FORMAT_PCM);
         } else if(format_src.encoding == XRAUDIO_ENCODING_OPUS  || format_src.encoding == XRAUDIO_ENCODING_OPUS_XVP) {
            return(XRSR_AUDIO_FORMAT_OPUS);
         } else if(format_src.encoding == XRAUDIO_ENCODING_ADPCM || format_src.encoding == XRAUDIO_ENCODING_ADPCM_XVP) {
            return(XRSR_AUDIO_FORMAT_ADPCM);
         }
      }
      case XRSR_AUDIO_FORMAT_INVALID: {
      }
   }
   return(XRSR_AUDIO_FORMAT_INVALID);
}

