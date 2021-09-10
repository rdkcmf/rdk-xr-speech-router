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
#include <xrsr_private.h>
#include <xraudio.h>

#define XRSR_XRAUDIO_IDENTIFIER (0x93482578)

#define FRAME_GROUP_SIZE_MAX (4096)

typedef struct {
   uint32_t                      identifier;
   xraudio_object_t              xraudio_obj;
   xrsr_xraudio_state_t          xraudio_state;
   xraudio_power_mode_t          xraudio_power_mode;
   bool                          xraudio_privacy_mode;
   xraudio_devices_input_t       device_input;
   xraudio_devices_output_t      device_output;
   bool                          detect_active;
   xraudio_keyword_phrase_t      keyword_phrase;
   xraudio_keyword_config_t      keyword_config;
   bool                          audio_stats_rxd;
   xrsr_audio_stats_t            audio_stats;
   xraudio_devices_input_t       available_inputs[XRAUDIO_INPUT_MAX_DEVICE_QTY];
   xraudio_devices_output_t      available_outputs[XRAUDIO_OUTPUT_MAX_DEVICE_QTY];
} xrsr_xraudio_obj_t;

static bool xrsr_xraudio_object_is_valid(xrsr_xraudio_obj_t *obj);
#ifdef XRAUDIO_RESOURCE_MGMT
static void xrsr_xraudio_resource_notification(xraudio_resource_event_t event, void *param);
#endif
static void xrsr_xraudio_keyword_callback(xraudio_devices_input_t source, keyword_callback_event_t event, void *param, xraudio_keyword_detector_result_t *detector_result, xraudio_input_format_t format);
static void xrsr_xraudio_stream_event(xraudio_devices_input_t source, audio_in_callback_event_t event, void *event_param, void *user_param);
static void xrsr_xraudio_device_close(xrsr_xraudio_object_t object);
static void xrsr_xraudio_keyword_detect_start(xrsr_xraudio_obj_t *obj);
static void xrsr_xraudio_keyword_detect_stop(xrsr_xraudio_obj_t *obj);
static xrsr_src_t xrsr_xraudio_src_to_xrsr(xraudio_devices_input_t src);
static void xrsr_audio_stats_clear(xrsr_xraudio_obj_t *obj);
static void xrsr_xraudio_local_mic_type_get(xrsr_xraudio_obj_t *obj);

static xraudio_devices_input_t g_local_mic_full_power = XRAUDIO_DEVICE_INPUT_NONE;
static xraudio_devices_input_t g_local_mic_low_power  = XRAUDIO_DEVICE_INPUT_NONE;

xrsr_xraudio_object_t xrsr_xraudio_create(xraudio_keyword_phrase_t keyword_phrase, xraudio_keyword_config_t keyword_config, xraudio_power_mode_t power_mode, bool privacy_mode, const json_t *json_obj_xraudio) {
   xrsr_xraudio_obj_t *obj = (xrsr_xraudio_obj_t *)malloc(sizeof(xrsr_xraudio_obj_t));

   if(obj == NULL) {
      XLOGD_ERROR("Out of memory.");
      return(NULL);
   }
   if((uint32_t)keyword_config >= XRAUDIO_KEYWORD_CONFIG_INVALID) {
      XLOGD_ERROR("invalid keyword config <%s>", xraudio_keyword_config_str(keyword_config));
      return(NULL);
   }
   if((uint32_t)power_mode >= XRAUDIO_POWER_MODE_INVALID) {
      XLOGD_ERROR("invalid power mode <%s>", xraudio_power_mode_str(power_mode));
      return(NULL);
   }

   obj->identifier           = XRSR_XRAUDIO_IDENTIFIER;
   obj->xraudio_state        = XRSR_XRAUDIO_STATE_CREATED;
   obj->xraudio_power_mode   = power_mode;
   obj->xraudio_privacy_mode = privacy_mode;
   obj->device_input         = XRAUDIO_DEVICE_INPUT_NONE;
   obj->device_output        = XRAUDIO_DEVICE_OUTPUT_NONE;
   obj->detect_active        = true;
   obj->keyword_phrase       = keyword_phrase;
   obj->keyword_config       = keyword_config;
   obj->audio_stats_rxd      = false;
   xrsr_audio_stats_clear(obj);
   obj->xraudio_obj          = xraudio_object_create(json_obj_xraudio);
   if(obj->xraudio_obj == NULL) {
      XLOGD_ERROR("unable to create xraudio object");
      free(obj);
      return(NULL);
   }
   xrsr_xraudio_local_mic_type_get(obj);

   return((xraudio_object_t)obj);
}

bool xrsr_xraudio_object_is_valid(xrsr_xraudio_obj_t *obj) {
   if(obj != NULL && obj->identifier == XRSR_XRAUDIO_IDENTIFIER) {
      return(true);
   }
   return(false);
}

void xrsr_xraudio_destroy(xrsr_xraudio_object_t object) {
   xrsr_xraudio_obj_t *obj = (xrsr_xraudio_obj_t *)object;
   
   if(!xrsr_xraudio_object_is_valid(obj)) {
      XLOGD_ERROR("invalid xrsr xraudio object");
      return;
   }
   if(obj->xraudio_obj != NULL) {
      xrsr_xraudio_device_close(object);
      xraudio_object_destroy(obj->xraudio_obj);
      obj->xraudio_obj = NULL;
   }

   obj->identifier = 0;
   free(obj);
}

void xrsr_xraudio_internal_capture_params_set(xrsr_xraudio_object_t object, xraudio_internal_capture_params_t *params) {
   xrsr_xraudio_obj_t *obj = (xrsr_xraudio_obj_t *)object;

   if(!xrsr_xraudio_object_is_valid(obj)) {
      XLOGD_ERROR("invalid xrsr xraudio object");
      return;
   }
   if(params == NULL) {
      return;
   }

   if(!params->enable) {
      XLOGD_INFO("disabled");
   } else {
      XLOGD_INFO("file qty max <%u> file size max <%u> dir path <%s> ", params->file_qty_max, params->file_size_max, params->dir_path);
   }

   xraudio_internal_capture_params_set(obj->xraudio_obj, params);
}

void xrsr_xraudio_internal_capture_delete_files(xrsr_xraudio_object_t object, const char *dir_path) {
   xrsr_xraudio_obj_t *obj = (xrsr_xraudio_obj_t *)object;

   if(!xrsr_xraudio_object_is_valid(obj)) {
      XLOGD_ERROR("invalid xrsr xraudio object");
      return;
   }
   if(dir_path == NULL) {
      return;
   }
   XLOGD_INFO("dir path <%s>", dir_path);
   xraudio_internal_capture_delete_files(obj->xraudio_obj, dir_path);
}

#ifdef XRAUDIO_RESOURCE_MGMT
void xrsr_xraudio_resource_notification(xraudio_resource_event_t event, void *param) {
   xrsr_queue_msg_generic_t msg;
   
   if(event == XRAUDIO_RESOURCE_EVENT_GRANTED) {
      msg.header.type = XRSR_QUEUE_MSG_TYPE_XRAUDIO_GRANTED;
   } else if(event == XRAUDIO_RESOURCE_EVENT_REVOKED) {
      msg.header.type = XRSR_QUEUE_MSG_TYPE_XRAUDIO_REVOKED;
   } else {
      XLOGD_ERROR("unhandled event <%s>", xraudio_resource_event_str(event));
      return;
   }
   
   xrsr_queue_msg_push(xrsr_msgq_fd_get(), (const char *)&msg, sizeof(msg));
}
#endif

void xrsr_xraudio_keyword_callback(xraudio_devices_input_t source, keyword_callback_event_t event, void *param, xraudio_keyword_detector_result_t *detector_result, xraudio_input_format_t format) {
   xrsr_queue_msg_keyword_detected_t msg;
   xrsr_xraudio_obj_t *obj = (xrsr_xraudio_obj_t *)param;
   if(!xrsr_xraudio_object_is_valid(obj)) {
      XLOGD_INFO("invalid object");
      return;
   }
   if(!obj->detect_active) {
      XLOGD_INFO("ignore keyword detect event");
      return;
   }
   
   if(event == KEYWORD_CALLBACK_EVENT_DETECTED) {
      msg.header.type    = XRSR_QUEUE_MSG_TYPE_KEYWORD_DETECTED;
      msg.source         = source;
      msg.xraudio_format = format;
   } else if(event == KEYWORD_CALLBACK_EVENT_ERROR || event == KEYWORD_CALLBACK_EVENT_ERROR_FD) {
      msg.header.type                = XRSR_QUEUE_MSG_TYPE_KEYWORD_DETECT_ERROR;
      msg.source                     = source;
      msg.xraudio_format.container   = XRAUDIO_CONTAINER_NONE;
      msg.xraudio_format.encoding    = XRAUDIO_ENCODING_INVALID;
      msg.xraudio_format.sample_rate = XRAUDIO_INPUT_DEFAULT_SAMPLE_RATE;
      msg.xraudio_format.sample_size = XRAUDIO_INPUT_DEFAULT_SAMPLE_SIZE;
      msg.xraudio_format.channel_qty = XRAUDIO_INPUT_DEFAULT_CHANNEL_QTY;
   } else {
      XLOGD_ERROR("unhandled event <%s>", xraudio_resource_event_str(event));
      return;
   }
   if(detector_result == NULL) {
      msg.has_result      = false;
      memset(&msg.detector_result, 0, sizeof(msg.detector_result));
   } else {
      msg.has_result      = true;
      msg.detector_result = *detector_result;
   }

   xrsr_queue_msg_push(xrsr_msgq_fd_get(), (const char *)&msg, sizeof(msg));
}

void xrsr_xraudio_device_update(xrsr_xraudio_object_t object, xrsr_src_t srcs[]) {
   xrsr_xraudio_obj_t *obj = (xrsr_xraudio_obj_t *)object;
   uint32_t index          = 0;
   
   if(!xrsr_xraudio_object_is_valid(obj)) {
      XLOGD_ERROR("invalid xrsr xraudio object");
      return;
   }

   xraudio_devices_input_t device_input = obj->device_input;

   obj->device_input = XRAUDIO_DEVICE_INPUT_NONE;
   do {
      if(srcs[index] >= XRSR_SRC_INVALID) {
         break;
      }
      switch(srcs[index]) {
         case XRSR_SRC_MICROPHONE: {
            if(obj->xraudio_power_mode == XRAUDIO_POWER_MODE_LOW) {
               obj->device_input |= g_local_mic_low_power;
            } else {
               obj->device_input |= g_local_mic_full_power;
            }
            break;
         }
         case XRSR_SRC_RCU_PTT: {
            obj->device_input |= XRAUDIO_DEVICE_INPUT_PTT;
            break;
         }
         case XRSR_SRC_RCU_FF: {
            obj->device_input |= XRAUDIO_DEVICE_INPUT_FF;
            break;
         }
         default: {
            break;
         }
      }
      index++;
   } while(1);
   
   obj->device_output = XRAUDIO_DEVICE_OUTPUT_NONE;
   XLOGD_INFO("input <%s> output <%s>", xraudio_devices_input_str(obj->device_input), xraudio_devices_output_str(obj->device_output));

   // Handle closing of xraudio obj and releasing resources when the input/output devices change
   if(device_input != obj->device_input) {
      xrsr_xraudio_device_close(obj);
   }

   if(obj->xraudio_state == XRSR_XRAUDIO_STATE_CREATED && obj->device_input != XRAUDIO_DEVICE_INPUT_NONE) {
      #ifdef XRAUDIO_RESOURCE_MGMT
      xrsr_xraudio_device_request(obj);
      #else
      xrsr_xraudio_device_granted(obj);
      #endif
   }
}

#ifdef XRAUDIO_RESOURCE_MGMT
void xrsr_xraudio_device_request(xrsr_xraudio_object_t object) {
   xrsr_xraudio_obj_t *obj = (xrsr_xraudio_obj_t *)object;
   if(obj->xraudio_state == XRSR_XRAUDIO_STATE_CREATED) { // Request xraudio resources
      xraudio_result_t result = xraudio_resource_request(obj->xraudio_obj, obj->device_input, obj->device_output, XRAUDIO_RESOURCE_PRIORITY_LOW, xrsr_xraudio_resource_notification, NULL);
      if(result != XRAUDIO_RESULT_OK) {
         XLOGD_ERROR("xraudio resource request <%s>", xraudio_result_str(result));
      } else {
         obj->xraudio_state = XRSR_XRAUDIO_STATE_REQUESTED;
      }
   }
}

void xrsr_xraudio_device_revoked(xrsr_xraudio_object_t object) {
   xrsr_xraudio_obj_t *obj = (xrsr_xraudio_obj_t *)object;

   if(!xrsr_xraudio_object_is_valid(obj)) {
      XLOGD_ERROR("invalid xrsr xraudio object");
      return;
   }

   XLOGD_INFO("");
   xrsr_xraudio_device_close(object);

   // Request the resources again
   xrsr_xraudio_device_request(object);
}
#endif

void xrsr_xraudio_device_granted(xrsr_xraudio_object_t object) {
   xrsr_xraudio_obj_t *obj = (xrsr_xraudio_obj_t *)object;
   
   if(!xrsr_xraudio_object_is_valid(obj)) {
      XLOGD_ERROR("invalid xrsr xraudio object");
      return;
   }
   
   XLOGD_INFO("");
   
   // Open the device
   xraudio_input_format_t format;
   format.container   = XRAUDIO_CONTAINER_NONE;
   format.encoding    = XRAUDIO_ENCODING_PCM;
   format.sample_rate = XRAUDIO_INPUT_DEFAULT_SAMPLE_RATE;
   format.sample_size = XRAUDIO_INPUT_DEFAULT_SAMPLE_SIZE;
   format.channel_qty = XRAUDIO_INPUT_DEFAULT_CHANNEL_QTY;

   xraudio_result_t result = XRAUDIO_RESULT_ERROR_INVALID;

   #ifdef XRSR_ALLOW_INPUT_FAILURE
   do {
      result = xraudio_open(obj->xraudio_obj, obj->xraudio_power_mode, obj->xraudio_privacy_mode, obj->device_input, obj->device_output, &format);

      if(XRAUDIO_RESULT_ERROR_MIC_OPEN == result) {
         if(obj->xraudio_power_mode == XRAUDIO_POWER_MODE_FULL) {
            obj->device_input &= ~g_local_mic_full_power;
         } else {
            obj->device_input &= ~g_local_mic_low_power;
         }
         XLOGD_INFO("mic error, device_input now <%s>", xraudio_devices_input_str(obj->device_input));
         continue;
      }
      break;
   }while(1);
   #else
   result = xraudio_open(obj->xraudio_obj, obj->xraudio_power_mode, obj->xraudio_privacy_mode, obj->device_input, obj->device_output, &format);
   #endif

   if(result != XRAUDIO_RESULT_OK) {
      XLOGD_ERROR("xraudio open <%s>", xraudio_result_str(result));
      return;
   }
   obj->xraudio_state = XRSR_XRAUDIO_STATE_OPENED;
   
   if(!obj->detect_active) {
      XLOGD_INFO("don't start keyword detection");
      return;
   }

   xrsr_xraudio_keyword_detect_start(obj);
}

void xrsr_xraudio_device_close(xrsr_xraudio_object_t object) {
   xrsr_xraudio_obj_t *obj = (xrsr_xraudio_obj_t *)object;

   if(!xrsr_xraudio_object_is_valid(obj)) {
      XLOGD_ERROR("invalid xrsr xraudio object");
      return;
   }

   XLOGD_INFO("");

   if(obj->xraudio_state == XRSR_XRAUDIO_STATE_STREAMING) {
      xraudio_stream_stop(obj->xraudio_obj, -1);
      obj->xraudio_state = XRSR_XRAUDIO_STATE_OPENED;
   }
   if(obj->xraudio_state == XRSR_XRAUDIO_STATE_DETECTING) {
      xraudio_detect_stop(obj->xraudio_obj);
      obj->xraudio_state = XRSR_XRAUDIO_STATE_OPENED;
   }

   if(obj->xraudio_state == XRSR_XRAUDIO_STATE_OPENED) {
      xraudio_close(obj->xraudio_obj);
      obj->xraudio_state = XRSR_XRAUDIO_STATE_GRANTED;
   }
   if(obj->xraudio_state == XRSR_XRAUDIO_STATE_GRANTED) {
      #ifdef XRAUDIO_RESOURCE_MGMT
      xraudio_resource_release(obj->xraudio_obj);
      #endif
      obj->xraudio_state = XRSR_XRAUDIO_STATE_CREATED;
   }
}

void xrsr_xraudio_keyword_detect_params(xrsr_xraudio_object_t *object, xraudio_keyword_phrase_t keyword_phrase, xraudio_keyword_config_t keyword_config) {
   xrsr_xraudio_obj_t *obj = (xrsr_xraudio_obj_t *)object;

   if(!xrsr_xraudio_object_is_valid(obj)) {
      XLOGD_ERROR("invalid xrsr xraudio object");
      return;
   }

   XLOGD_INFO("phrase <%s> config <%s>", xraudio_keyword_phrase_str(keyword_phrase), xraudio_keyword_config_str(keyword_config));

   bool changed = (obj->keyword_phrase != keyword_phrase) || (obj->keyword_config != keyword_config);

   obj->keyword_phrase = keyword_phrase;
   obj->keyword_config = keyword_config;

   if(changed && (obj->xraudio_state == XRSR_XRAUDIO_STATE_DETECTING)) {
      xraudio_result_t result = xraudio_detect_params(obj->xraudio_obj, obj->keyword_phrase, obj->keyword_config);
      if(XRAUDIO_RESULT_OK != result) {
         XLOGD_ERROR("xraudio_detect_params <%s>", xraudio_result_str(result));
      }
   }
}

void xrsr_xraudio_keyword_detect_restart(xrsr_xraudio_object_t object) {
   xrsr_xraudio_obj_t *obj = (xrsr_xraudio_obj_t *)object;

   if(!xrsr_xraudio_object_is_valid(obj)) {
      XLOGD_ERROR("invalid xrsr xraudio object");
      return;
   }
   xrsr_xraudio_keyword_detect_start(obj);
}

void xrsr_xraudio_keyword_detect_start(xrsr_xraudio_obj_t *obj) {
   XLOGD_INFO("phrase <%s> config <%s>", xraudio_keyword_phrase_str(obj->keyword_phrase), xraudio_keyword_config_str(obj->keyword_config));

   if(obj->xraudio_power_mode == XRAUDIO_POWER_MODE_SLEEP ) {
      XLOGD_INFO("sleep mode, do not start keyword detector");
      return;
   }

   xraudio_result_t result = xraudio_detect_params(obj->xraudio_obj, obj->keyword_phrase, obj->keyword_config);
   if(XRAUDIO_RESULT_OK != result) {
      XLOGD_ERROR("xraudio_detect_params <%s>", xraudio_result_str(result));
   }

   result = xraudio_detect_keyword(obj->xraudio_obj, (keyword_callback_t) xrsr_xraudio_keyword_callback, obj);
   if(result != XRAUDIO_RESULT_OK) {
      XLOGD_ERROR("xraudio keyword detect <%s>", xraudio_result_str(result));
   } else {
      obj->xraudio_state = XRSR_XRAUDIO_STATE_DETECTING;
   }
}

void xrsr_xraudio_keyword_detect_stop(xrsr_xraudio_obj_t *obj) {
   xraudio_result_t result = xraudio_detect_stop(obj->xraudio_obj);

   if(result != XRAUDIO_RESULT_OK) {
       XLOGD_ERROR("xraudio_detect_stop <%s>", xraudio_result_str(result));
       return;
   }
   obj->xraudio_state = XRSR_XRAUDIO_STATE_OPENED;
}

void xrsr_xraudio_keyword_detected(xrsr_xraudio_object_t object, xrsr_queue_msg_keyword_detected_t *msg, xrsr_src_t current_session_src) {
   xrsr_xraudio_obj_t *obj = (xrsr_xraudio_obj_t *)object;
   xrsr_src_t          src = XRSR_SRC_INVALID;
   
   if(!xrsr_xraudio_object_is_valid(obj)) {
      XLOGD_ERROR("invalid xrsr xraudio object");
      return;
   }
   if(obj->xraudio_state != XRSR_XRAUDIO_STATE_DETECTING) {
      XLOGD_ERROR("invalid state <%s>", xrsr_xraudio_state_str(obj->xraudio_state));
      return;
   }
   
   XLOGD_DEBUG("");
   
   bool user_initiated = false;
   if(XRAUDIO_DEVICE_INPUT_LOCAL_GET(msg->source)) { // Local microphone
      src = XRSR_SRC_MICROPHONE;
   } else if(XRAUDIO_DEVICE_INPUT_EXTERNAL_GET(msg->source) == XRAUDIO_DEVICE_INPUT_PTT) {
      src = XRSR_SRC_RCU_PTT;
      user_initiated = true;
   } else if(XRAUDIO_DEVICE_INPUT_EXTERNAL_GET(msg->source) == XRAUDIO_DEVICE_INPUT_FF) {
      src = XRSR_SRC_RCU_FF;
   } else {
      XLOGD_ERROR("invalid source <0x%08X>", msg->source);
      return;
   }

   // Stop the detector
   xrsr_xraudio_keyword_detect_stop(obj);

   if((uint32_t)current_session_src < XRSR_SRC_INVALID && current_session_src != src) {
      XLOGD_WARN("Rejecting keyword detected from source <%s>, session in progress on source <%s>.  Restarting keyword detector...", xrsr_src_str(src), xrsr_src_str(current_session_src));
      xrsr_xraudio_keyword_detect_start(obj);   //xrsr_xraudio_keyword_detect_stop() needs to be called before this.
      return;
   }

   xraudio_keyword_detector_result_t *detector_result = NULL;
   if(msg->has_result) {
      detector_result = &msg->detector_result;
   }

   XLOGD_INFO("Keyword detected for source <%s>", xrsr_src_str(src));

   // Call the appropriate handler based on the source
   xrsr_session_begin(src, user_initiated, msg->xraudio_format, detector_result, NULL);
}

void xrsr_xraudio_keyword_detect_error(xrsr_xraudio_object_t object, xraudio_devices_input_t source) {
   xrsr_xraudio_obj_t *obj = (xrsr_xraudio_obj_t *)object;
   xrsr_src_t          src = XRSR_SRC_INVALID;

   if(!xrsr_xraudio_object_is_valid(obj)) {
      XLOGD_ERROR("invalid xrsr xraudio object");
      return;
   }
   if(obj->xraudio_state != XRSR_XRAUDIO_STATE_DETECTING) {
      XLOGD_ERROR("invalid state <%s>", xrsr_xraudio_state_str(obj->xraudio_state));
      return;
   }

   XLOGD_INFO("source <%s>", xraudio_devices_input_str(source));

   bool restart = false;

   if(XRAUDIO_DEVICE_INPUT_LOCAL_GET(source) != XRAUDIO_DEVICE_INPUT_NONE) { // Local microphone
      src = XRSR_SRC_MICROPHONE;
      restart = true;
   } else if(XRAUDIO_DEVICE_INPUT_EXTERNAL_GET(source) == XRAUDIO_DEVICE_INPUT_PTT) {
      src = XRSR_SRC_RCU_PTT;
   } else if(XRAUDIO_DEVICE_INPUT_EXTERNAL_GET(source) == XRAUDIO_DEVICE_INPUT_FF) {
      src = XRSR_SRC_RCU_FF;
   } else {
      XLOGD_ERROR("invalid source <0x%08X>", source);
      return;
   }

   // Stop the detector
   xrsr_xraudio_keyword_detect_stop(obj);

   // Call the appropriate handler based on the source
   xrsr_keyword_detect_error(src);

   if(restart) {
      xrsr_xraudio_device_close(obj);
      #ifdef XRAUDIO_RESOURCE_MGMT
      xrsr_xraudio_device_request(obj);
      // TODO Need to wait for grant before proceeding
      return;
      #else
      xrsr_xraudio_device_granted(obj);
      #endif
   }

   // TODO Should the app determine what to do here?
   if(obj->detect_active) { // Start detector again
      xrsr_xraudio_keyword_detect_start(obj);
   }
}

bool xrsr_xraudio_stream_begin(xrsr_xraudio_object_t object, const char *stream_id, xraudio_devices_input_t source, bool user_initiated, xraudio_input_format_t *format_decoded, xraudio_dst_pipe_t dsts[], uint16_t stream_time_min, uint32_t keyword_begin, uint32_t keyword_duration, uint32_t frame_duration) {
   xrsr_xraudio_obj_t *obj = (xrsr_xraudio_obj_t *)object;

   if(!xrsr_xraudio_object_is_valid(obj)) {
      XLOGD_ERROR("invalid xrsr xraudio object");
      return(false);
   }
   if(obj->xraudio_state != XRSR_XRAUDIO_STATE_OPENED) {
      XLOGD_ERROR("invalid state <%s>", xrsr_xraudio_state_str(obj->xraudio_state));
      return(false);
   }

   XLOGD_INFO("stream id <%s> source <%s> user <%s> pipe <%d, %d>", stream_id, xraudio_devices_input_str(source), user_initiated ? "YES" : "NO", dsts[0].pipe, dsts[1].pipe);

   uint64_t frame_byte_qty;
   if(format_decoded) {
      frame_byte_qty = (format_decoded->sample_rate * format_decoded->sample_size * format_decoded->channel_qty) * (frame_duration) / (1000000);
   } else if(XRAUDIO_DEVICE_INPUT_EXTERNAL_GET(source)) {
      frame_byte_qty = 95;
   } else {
      frame_byte_qty = (XRAUDIO_INPUT_DEFAULT_SAMPLE_RATE * XRAUDIO_INPUT_DEFAULT_SAMPLE_SIZE * XRAUDIO_INPUT_DEFAULT_CHANNEL_QTY) * (frame_duration) / (1000000);
   }
   uint32_t frame_group_quantity = FRAME_GROUP_SIZE_MAX / frame_byte_qty;
   if(frame_group_quantity > XRAUDIO_INPUT_MAX_FRAME_GROUP_QTY) {
      frame_group_quantity = XRAUDIO_INPUT_MAX_FRAME_GROUP_QTY;
   } else if(frame_group_quantity < XRAUDIO_INPUT_MIN_FRAME_GROUP_QTY) {
      frame_group_quantity = XRAUDIO_INPUT_MIN_FRAME_GROUP_QTY;
   }

   XLOGD_INFO("frame size <%llu> group qty <%u> duration <%u> usecs", frame_byte_qty, frame_group_quantity, frame_duration);

   xraudio_result_t result = xraudio_stream_frame_group_quantity_set(obj->xraudio_obj, frame_group_quantity);
   if(result != XRAUDIO_RESULT_OK) {
      XLOGD_WARN("unable to set frame group quantity <%s>", xraudio_result_str(result));
   }

   result = xraudio_stream_identifier_set(obj->xraudio_obj, stream_id);
   if(result != XRAUDIO_RESULT_OK) {
      XLOGD_WARN("unable to set stream id <%s>", xraudio_result_str(result));
   }

   result = xraudio_stream_time_minimum(obj->xraudio_obj, stream_time_min);
   if(result != XRAUDIO_RESULT_OK) {
      XLOGD_WARN("unable to set stream time min <%s>", xraudio_result_str(result));
   }

   if(keyword_duration != 0) { // Keyword is present
      result = xraudio_stream_keyword_info(obj->xraudio_obj, keyword_begin, keyword_duration);
      if(result != XRAUDIO_RESULT_OK) {
         XLOGD_WARN("unable to set stream keyword info <%s>", xraudio_result_str(result));
      }
   }

   result = xraudio_stream_to_pipe(obj->xraudio_obj, source, dsts, format_decoded, xrsr_xraudio_stream_event, (void *)obj);

   if(result != XRAUDIO_RESULT_OK) {
      XLOGD_ERROR("xraudio_stream_to_pipe <%s>", xraudio_result_str(result));
      return(false);
   }
   obj->xraudio_state = XRSR_XRAUDIO_STATE_STREAMING;
   return(true);
}

bool xrsr_xraudio_stream_end(xrsr_xraudio_object_t object, uint32_t dst_index, bool more_streams, bool detect_resume, xrsr_audio_stats_t *audio_stats) {
   xrsr_xraudio_obj_t *obj = (xrsr_xraudio_obj_t *)object;

   if(!xrsr_xraudio_object_is_valid(obj)) {
      XLOGD_ERROR("invalid xrsr xraudio object");
      return(false);
   }
   if(obj->xraudio_state != XRSR_XRAUDIO_STATE_OPENED && obj->xraudio_state != XRSR_XRAUDIO_STATE_STREAMING) {
      XLOGD_ERROR("invalid state <%s>", xrsr_xraudio_state_str(obj->xraudio_state));
      return(false);
   }

   XLOGD_INFO("state <%s> more streams <%s>", xrsr_xraudio_state_str(obj->xraudio_state), more_streams ? "YES" : "NO");

   if(obj->xraudio_state == XRSR_XRAUDIO_STATE_STREAMING) {
      xraudio_result_t result = xraudio_stream_stop(obj->xraudio_obj, dst_index);

      if(result != XRAUDIO_RESULT_OK) {
          XLOGD_ERROR("xraudio_stream_stop <%s> dst index <%u>", xraudio_result_str(result), dst_index);
          return(false);
      }
   }

   if(audio_stats != NULL) {
      *audio_stats = obj->audio_stats;
   }

   if(!more_streams) { // no more streams open
      obj->xraudio_state = XRSR_XRAUDIO_STATE_OPENED;
      obj->detect_active = detect_resume;

      if(obj->detect_active) { // Start detector again
         xrsr_xraudio_keyword_detect_start(obj);
      }
      xrsr_audio_stats_clear(obj);
   }

   return(true);
}

void xrsr_xraudio_stream_event(xraudio_devices_input_t source, audio_in_callback_event_t event, void *event_param, void *user_param) {
   xrsr_xraudio_obj_t *obj = (xrsr_xraudio_obj_t *)user_param;
   XLOGD_INFO("source <%s> event <%s>", xraudio_devices_input_str(source), audio_in_callback_event_str(event));
   xrsr_queue_msg_xraudio_in_event_t msg;
   memset(&msg, 0, sizeof(msg));
   msg.header.type = XRSR_QUEUE_MSG_TYPE_XRAUDIO_EVENT;
   msg.event.src   = xrsr_xraudio_src_to_xrsr(source);
   switch(event) {
      case AUDIO_IN_CALLBACK_EVENT_EOS:
      case AUDIO_IN_CALLBACK_EVENT_EOS_TIMEOUT_INITIAL:
      case AUDIO_IN_CALLBACK_EVENT_EOS_TIMEOUT_END: {
         msg.event.event = XRSR_EVENT_EOS;
         if(event_param) {
            xraudio_audio_stats_t *xraudio_audio_stats = (xraudio_audio_stats_t *)event_param;
            obj->audio_stats.packets_processed    = xraudio_audio_stats->packets_processed;
            obj->audio_stats.packets_lost         = xraudio_audio_stats->packets_lost;
            obj->audio_stats.samples_processed    = xraudio_audio_stats->samples_processed;
            obj->audio_stats.samples_lost         = xraudio_audio_stats->samples_lost;
            obj->audio_stats.decoder_failures     = xraudio_audio_stats->decoder_failures;
            obj->audio_stats.samples_buffered_max = xraudio_audio_stats->samples_buffered_max;
            obj->audio_stats.valid                = true;

            XLOGD_DEBUG("xraudio stats - packets processed <%u> lost <%u> samples processed <%u> lost <%u> decoder failures <%u>", obj->audio_stats.packets_processed, obj->audio_stats.packets_lost, obj->audio_stats.samples_processed, obj->audio_stats.samples_lost, obj->audio_stats.decoder_failures);
         } else {
            XLOGD_DEBUG("xraudio did NOT provided stats with EOS event");
         }
         break;
      }
      case AUDIO_IN_CALLBACK_EVENT_STREAM_TIME_MINIMUM: {
         msg.event.event = XRSR_EVENT_STREAM_TIME_MINIMUM;
         break;
      }
      case AUDIO_IN_CALLBACK_EVENT_STREAM_KWD_INFO: {
         msg.event.event = XRSR_EVENT_STREAM_KWD_INFO;
         if(event_param == NULL) {
            XLOGD_ERROR("xraudio did NOT provide param with KWD info event");
            return;
         }

         xraudio_stream_keyword_info_t *kwd_info = (xraudio_stream_keyword_info_t *)event_param;
         msg.event.data.byte_qty = kwd_info->byte_qty;
         break;
      }
      default: {
         msg.event.event = XRSR_EVENT_INVALID;
         break;
      }
   }
   xrsr_queue_msg_push(xrsr_msgq_fd_get(), (const char *)&msg, sizeof(msg));
}

bool xrsr_xraudio_session_request(xrsr_xraudio_object_t object, xrsr_src_t src, xraudio_input_format_t xraudio_format, const char* transcription_in) {
   xrsr_xraudio_obj_t *obj = (xrsr_xraudio_obj_t *)object;

   if(!xrsr_xraudio_object_is_valid(obj)) {
      XLOGD_ERROR("invalid xrsr xraudio object");
      return(false);
   }
   if(obj->xraudio_state != XRSR_XRAUDIO_STATE_OPENED && obj->xraudio_state != XRSR_XRAUDIO_STATE_DETECTING) {
      XLOGD_ERROR("invalid state <%s>", xrsr_xraudio_state_str(obj->xraudio_state));
      return(false);
   }

   if(obj->xraudio_state == XRSR_XRAUDIO_STATE_DETECTING) {
      xraudio_detect_stop(obj->xraudio_obj);
      obj->xraudio_state = XRSR_XRAUDIO_STATE_OPENED;
   }

   xrsr_session_begin(src, true, xraudio_format, NULL, transcription_in);

   return(true);
}

xrsr_src_t xrsr_xraudio_src_to_xrsr(xraudio_devices_input_t src) {
   switch(src) {
      case XRAUDIO_DEVICE_INPUT_PTT:  return(XRSR_SRC_RCU_PTT);
      case XRAUDIO_DEVICE_INPUT_FF:   return(XRSR_SRC_RCU_FF);
      default:                        return(XRSR_SRC_MICROPHONE);
   }
   return(XRSR_SRC_INVALID);
}

void xrsr_audio_stats_clear(xrsr_xraudio_obj_t *obj) {
   if(obj != NULL) {
      obj->audio_stats_rxd = false;
      memset(&obj->audio_stats, 0, sizeof(obj->audio_stats));
   }
}

void xrsr_xraudio_local_mic_type_get(xrsr_xraudio_obj_t *obj) {
   xraudio_result_t result = XRAUDIO_RESULT_OK;

   if(obj == NULL) {
      XLOGD_ERROR("null xrsr obj");
      return;
   }

   memset(obj->available_inputs,  0, sizeof(obj->available_inputs));
   memset(obj->available_outputs, 0, sizeof(obj->available_outputs));

   result = xraudio_available_devices_get(obj->xraudio_obj, obj->available_inputs, XRAUDIO_INPUT_MAX_DEVICE_QTY, obj->available_outputs, XRAUDIO_OUTPUT_MAX_DEVICE_QTY);
   if(result != XRAUDIO_RESULT_OK) {
      XLOGD_ERROR("unable to get available xraudio in/output devices");
      return;
   }

   for (int i = 0; i < XRAUDIO_INPUT_MAX_DEVICE_QTY; i++) {
      if (XRAUDIO_DEVICE_INPUT_LOCAL_GET(obj->available_inputs[i]) != XRAUDIO_DEVICE_INPUT_NONE) {
         g_local_mic_low_power = XRAUDIO_DEVICE_INPUT_SINGLE;
         //This assumes that low power uses a single mic, which is the case and likely to remain so, but may need to be revisited later.
         if(obj->available_inputs[i] != XRAUDIO_DEVICE_INPUT_SINGLE) {
            //If we have only one mic, use it. If more than one use the multi for full power
            g_local_mic_full_power = obj->available_inputs[i] & ~XRAUDIO_DEVICE_INPUT_SINGLE;
         }
         break;
      }
   }
}

void xrsr_xraudio_session_capture_start(xrsr_xraudio_object_t object, xrsr_audio_container_t container, const char *file_path, bool raw_mic_enable) {
   xrsr_xraudio_obj_t *obj = (xrsr_xraudio_obj_t *)object;

   if(!xrsr_xraudio_object_is_valid(obj)) {
      XLOGD_ERROR("invalid xrsr xraudio object");
      return;
   }

   XLOGD_INFO("container <%s> file path <%s> raw mic enable <%d>", xrsr_audio_container_str(container), file_path, raw_mic_enable);

   xraudio_capture_t capture = XRAUDIO_CAPTURE_INPUT_ALL | XRAUDIO_CAPTURE_KWD | XRAUDIO_CAPTURE_EOS | XRAUDIO_CAPTURE_DGA | XRAUDIO_CAPTURE_OUTPUT;

   xraudio_result_t result = xraudio_capture_to_file_start(obj->xraudio_obj, capture, (container == XRSR_AUDIO_CONTAINER_WAV) ? XRAUDIO_CONTAINER_WAV : XRAUDIO_CONTAINER_NONE, file_path, raw_mic_enable, NULL, NULL);

   if(XRAUDIO_RESULT_OK != result) {
      XLOGD_ERROR("xraudio_capture_to_file_start returned <%s>", xraudio_result_str(result));
   }
}

void xrsr_xraudio_session_capture_stop(xrsr_xraudio_object_t object) {
   xrsr_xraudio_obj_t *obj = (xrsr_xraudio_obj_t *)object;

   if(!xrsr_xraudio_object_is_valid(obj)) {
      XLOGD_ERROR("invalid xrsr xraudio object");
      return;
   }

   XLOGD_INFO("");

   xraudio_result_t result = xraudio_capture_stop(obj->xraudio_obj);
   if(XRAUDIO_RESULT_OK != result) {
      XLOGD_ERROR("xraudio_capture_stop returned <%s>", xraudio_result_str(result));
   }
}

void xrsr_xraudio_thread_poll(xrsr_xraudio_object_t object, xrsr_thread_poll_func_t func) {
   xrsr_xraudio_obj_t *obj = (xrsr_xraudio_obj_t *)object;

   if(!xrsr_xraudio_object_is_valid(obj)) {
      XLOGD_ERROR("invalid xrsr xraudio object");
      return;
   }

   xraudio_thread_poll(obj->xraudio_obj, func);
}

bool xrsr_xraudio_power_mode_update(xrsr_xraudio_object_t object, xrsr_power_mode_t power_mode) {
   xrsr_xraudio_obj_t *obj = (xrsr_xraudio_obj_t *)object;

   if(!xrsr_xraudio_object_is_valid(obj)) {
      XLOGD_ERROR("invalid xrsr xraudio object");
      return(false);
   }
   xraudio_power_mode_t xraudio_power_mode = XRAUDIO_POWER_MODE_INVALID;

   switch(power_mode) {
      case XRSR_POWER_MODE_FULL:
         xraudio_power_mode = XRAUDIO_POWER_MODE_FULL;
         obj->device_input  &= ~g_local_mic_low_power;
         obj->device_input  |=  g_local_mic_full_power;
         break;

      case XRSR_POWER_MODE_LOW:
         xraudio_power_mode = XRAUDIO_POWER_MODE_LOW;
         obj->device_input  &= ~g_local_mic_full_power;
         obj->device_input  |=  g_local_mic_low_power;
         break;

      case XRSR_POWER_MODE_SLEEP:
         xraudio_power_mode = XRAUDIO_POWER_MODE_SLEEP;
         obj->device_input  &= ~g_local_mic_full_power;
         obj->device_input  |=  g_local_mic_low_power;
         break;

      default: {
         XLOGD_ERROR("invalid power mode <%s>", xrsr_power_mode_str(power_mode));
         return(false);
      }
   }

   if(obj->xraudio_power_mode==xraudio_power_mode) {
      XLOGD_WARN("power mode already <%s>", xrsr_power_mode_str(power_mode));
      return(true);
   }

   //Closing, updating power mode, and reopening allows us to configure xraudio inputs and load new firmware easily
   xrsr_xraudio_device_close(obj);
   obj->xraudio_power_mode = xraudio_power_mode;
   #ifdef XRAUDIO_RESOURCE_MGMT
   xrsr_xraudio_device_request(obj);
   #else
   xrsr_xraudio_device_granted(obj);
   #endif

   xraudio_result_t result = xraudio_power_mode_update(obj->xraudio_obj, xraudio_power_mode);
   if(result != XRAUDIO_RESULT_OK) {
      XLOGD_ERROR("unable to set xraudio power mode <%s>", xraudio_result_str(result));
      return(false);
   }

   return(true);
}

bool xrsr_xraudio_privacy_mode_update(xrsr_xraudio_object_t object, bool enable) {
   xrsr_xraudio_obj_t *obj = (xrsr_xraudio_obj_t *)object;

   if(!xrsr_xraudio_object_is_valid(obj)) {
      XLOGD_ERROR("invalid xrsr xraudio object");
      return(false);
   }

   if(obj->xraudio_privacy_mode == enable) {
      XLOGD_ERROR("already %s", enable ? "enabled" : "disabled");
      return(false);
   }

   xraudio_result_t result = xraudio_privacy_mode_update(obj->xraudio_obj, g_local_mic_full_power, enable);
   if(result != XRAUDIO_RESULT_OK) {
      XLOGD_ERROR("unable to set xraudio privacy mode <%s>", xraudio_result_str(result));
      return(false);
   }

   obj->xraudio_privacy_mode = enable;
   return(true);
}

bool xrsr_xraudio_privacy_mode_get(xrsr_xraudio_object_t object, bool *enabled) {
   xrsr_xraudio_obj_t *obj = (xrsr_xraudio_obj_t *)object;

   if(!xrsr_xraudio_object_is_valid(obj)) {
      XLOGD_ERROR("invalid xrsr xraudio object");
      return false;
   }
   if(enabled == NULL) {
      XLOGD_ERROR("invalid pointer");
      return false;
   }

   xraudio_result_t result = xraudio_privacy_mode_get(obj->xraudio_obj, obj->device_input, enabled);
   if(XRAUDIO_RESULT_OK != result) {
      XLOGD_ERROR("xraudio_privacy_mode_get returned <%s>", xraudio_result_str(result));
      return false;
   }

   return true;
}

