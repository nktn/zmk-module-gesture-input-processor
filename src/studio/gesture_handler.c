/**
 * Gesture Input Processor - Custom Studio RPC Handler
 *
 * This file implements the custom RPC subsystem for gesture input processor
 * configuration. Request dispatch follows the template_handler.c /
 * cormoran/zmk-module-runtime-input-processor custom_handler.c pattern
 * (proto/nktn/gesture/gesture.proto is the source of truth for message
 * shapes and field numbers).
 */

#include <nktn/gesture/gesture.pb.h>
#include <pb_decode.h>
#include <pb_encode.h>
#include <stdio.h>
#include <zephyr/logging/log.h>
#include <zmk/event_manager.h>
#include <zmk/events/gesture_processor_state_changed.h>
#include <zmk/input_processors/gesture.h>
#include <zmk/studio/custom.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if IS_ENABLED(CONFIG_ZMK_GESTURE_INPUT_PROCESSOR)

/**
 * Metadata for the custom subsystem.
 */
static struct zmk_rpc_custom_subsystem_meta gesture_feature_meta = {
    ZMK_RPC_CUSTOM_SUBSYSTEM_UI_URLS("https://nktn.github.io/zmk-module-gesture-input-processor/"),
    // Unsecured is suggested by default to avoid unlocking in un-reliable environments.
    .security = ZMK_STUDIO_RPC_HANDLER_UNSECURED,
};

/**
 * Register the custom RPC subsystem. Using "nktn__gesture" as the identifier.
 */
ZMK_RPC_CUSTOM_SUBSYSTEM(nktn__gesture, &gesture_feature_meta, gesture_rpc_handle_request);

ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER(nktn__gesture, nktn_gesture_Response);

static int handle_list_processors(const nktn_gesture_ListProcessorsRequest *req,
                                  nktn_gesture_Response *resp);
static int handle_set_enabled(const nktn_gesture_SetEnabledRequest *req, nktn_gesture_Response *resp);
static int handle_set_active_layers(const nktn_gesture_SetActiveLayersRequest *req,
                                    nktn_gesture_Response *resp);
static int handle_set_threshold(const nktn_gesture_SetThresholdRequest *req,
                                nktn_gesture_Response *resp);
static int handle_set_reset_ms(const nktn_gesture_SetResetMsRequest *req, nktn_gesture_Response *resp);
static int handle_set_cooldown_ms(const nktn_gesture_SetCooldownMsRequest *req,
                                  nktn_gesture_Response *resp);

/**
 * Main request handler for the custom RPC subsystem.
 */
static bool gesture_rpc_handle_request(const zmk_custom_CallRequest *raw_request,
                                       pb_callback_t *encode_response) {
    nktn_gesture_Response *resp =
        ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER_ALLOCATE(nktn__gesture, encode_response);

    nktn_gesture_Request req = nktn_gesture_Request_init_zero;

    // Decode the incoming request from the raw payload
    pb_istream_t req_stream =
        pb_istream_from_buffer(raw_request->payload.bytes, raw_request->payload.size);
    if (!pb_decode(&req_stream, nktn_gesture_Request_fields, &req)) {
        LOG_WRN("Failed to decode gesture request: %s", PB_GET_ERROR(&req_stream));
        nktn_gesture_ErrorResponse err = nktn_gesture_ErrorResponse_init_zero;
        snprintf(err.message, sizeof(err.message), "Failed to decode request");
        resp->which_response_type = nktn_gesture_Response_error_tag;
        resp->response_type.error = err;
        return true;
    }

    int rc = 0;
    // NOTE: every case below must `break` -- a fall-through here silently invokes the wrong
    // handler and stomps the response, the exact bug fixed in
    // zmk-module-runtime-input-processor's custom_handler.c (set_xy_swap_enabled fell through
    // into set_x_invert). Double-check this switch whenever a case is added.
    switch (req.which_request_type) {
    case nktn_gesture_Request_list_processors_tag:
        rc = handle_list_processors(&req.request_type.list_processors, resp);
        break;
    case nktn_gesture_Request_set_enabled_tag:
        rc = handle_set_enabled(&req.request_type.set_enabled, resp);
        break;
    case nktn_gesture_Request_set_active_layers_tag:
        rc = handle_set_active_layers(&req.request_type.set_active_layers, resp);
        break;
    case nktn_gesture_Request_set_threshold_tag:
        rc = handle_set_threshold(&req.request_type.set_threshold, resp);
        break;
    case nktn_gesture_Request_set_reset_ms_tag:
        rc = handle_set_reset_ms(&req.request_type.set_reset_ms, resp);
        break;
    case nktn_gesture_Request_set_cooldown_ms_tag:
        rc = handle_set_cooldown_ms(&req.request_type.set_cooldown_ms, resp);
        break;
    default:
        LOG_WRN("Unsupported gesture request type: %d", req.which_request_type);
        rc = -1;
    }

    if (rc != 0) {
        nktn_gesture_ErrorResponse err = nktn_gesture_ErrorResponse_init_zero;
        snprintf(err.message, sizeof(err.message), "Failed to process request");
        resp->which_response_type = nktn_gesture_Response_error_tag;
        resp->response_type.error = err;
    }
    return true;
}

// Helper callback to send a Notification for each processor during a list operation. Actual
// instance data is delivered as one Notification per instance (see
// gesture_processor_listener.c), matching zmk-module-runtime-input-processor's
// list_input_processors pattern: raise events from a k_work so we're not doing this work on the
// RPC handling stack.
struct list_processors_context {
    int count;
};

static int list_processors_callback(const struct device *dev, void *user_data) {
    struct list_processors_context *ctx = (struct list_processors_context *)user_data;

    const char *name;
    struct zmk_gesture_processor_config config;
    int ret = zmk_gesture_processor_get_config(dev, &name, &config);
    if (ret < 0) {
        return 0;
    }

    int id = zmk_gesture_processor_get_id(dev);
    if (id < 0) {
        return 0;
    }

    raise_zmk_gesture_processor_state_changed(
        (struct zmk_gesture_processor_state_changed){.id = (uint8_t)id, .name = name, .config = config});

    ctx->count++;
    return 0;
}

static void list_processors_work_handler(struct k_work *work) {
    struct list_processors_context ctx = {.count = 0};
    zmk_gesture_processor_foreach(list_processors_callback, &ctx);
    LOG_INF("Raised events for %d gesture processors", ctx.count);
}

K_WORK_DEFINE(list_processors_work, list_processors_work_handler);

/**
 * Handle listing all gesture processors - raises events for each, response body stays empty.
 */
static int handle_list_processors(const nktn_gesture_ListProcessorsRequest *req,
                                  nktn_gesture_Response *resp) {
    k_work_submit(&list_processors_work);

    resp->which_response_type = nktn_gesture_Response_list_processors_tag;
    resp->response_type.list_processors =
        (nktn_gesture_ListProcessorsResponse)nktn_gesture_ListProcessorsResponse_init_zero;
    return 0;
}

static int handle_set_enabled(const nktn_gesture_SetEnabledRequest *req, nktn_gesture_Response *resp) {
    LOG_DBG("Setting gesture enabled for id=%d to %d", req->id, req->enabled);

    const struct device *dev = zmk_gesture_processor_find_by_id(req->id);
    if (!dev) {
        LOG_WRN("Gesture processor not found: id=%d", req->id);
        return -ENODEV;
    }

    int ret = zmk_gesture_processor_set_enabled(dev, req->enabled);
    if (ret < 0) {
        LOG_ERR("Failed to set gesture enabled: %d", ret);
        return ret;
    }

    resp->which_response_type = nktn_gesture_Response_ok_tag;
    resp->response_type.ok = (nktn_gesture_OkResponse)nktn_gesture_OkResponse_init_zero;
    return 0;
}

static int handle_set_active_layers(const nktn_gesture_SetActiveLayersRequest *req,
                                    nktn_gesture_Response *resp) {
    LOG_DBG("Setting gesture active layers for id=%d to 0x%08x", req->id, req->active_layers);

    const struct device *dev = zmk_gesture_processor_find_by_id(req->id);
    if (!dev) {
        LOG_WRN("Gesture processor not found: id=%d", req->id);
        return -ENODEV;
    }

    int ret = zmk_gesture_processor_set_active_layers(dev, req->active_layers);
    if (ret < 0) {
        LOG_ERR("Failed to set gesture active layers: %d", ret);
        return ret;
    }

    resp->which_response_type = nktn_gesture_Response_ok_tag;
    resp->response_type.ok = (nktn_gesture_OkResponse)nktn_gesture_OkResponse_init_zero;
    return 0;
}

static int handle_set_threshold(const nktn_gesture_SetThresholdRequest *req,
                                nktn_gesture_Response *resp) {
    LOG_DBG("Setting gesture threshold for id=%d to %d", req->id, req->threshold);

    const struct device *dev = zmk_gesture_processor_find_by_id(req->id);
    if (!dev) {
        LOG_WRN("Gesture processor not found: id=%d", req->id);
        return -ENODEV;
    }

    int ret = zmk_gesture_processor_set_threshold(dev, req->threshold);
    if (ret < 0) {
        LOG_ERR("Failed to set gesture threshold: %d", ret);
        return ret;
    }

    resp->which_response_type = nktn_gesture_Response_ok_tag;
    resp->response_type.ok = (nktn_gesture_OkResponse)nktn_gesture_OkResponse_init_zero;
    return 0;
}

static int handle_set_reset_ms(const nktn_gesture_SetResetMsRequest *req,
                               nktn_gesture_Response *resp) {
    LOG_DBG("Setting gesture reset_ms for id=%d to %d", req->id, req->reset_ms);

    const struct device *dev = zmk_gesture_processor_find_by_id(req->id);
    if (!dev) {
        LOG_WRN("Gesture processor not found: id=%d", req->id);
        return -ENODEV;
    }

    int ret = zmk_gesture_processor_set_reset_ms(dev, req->reset_ms);
    if (ret < 0) {
        LOG_ERR("Failed to set gesture reset_ms: %d", ret);
        return ret;
    }

    resp->which_response_type = nktn_gesture_Response_ok_tag;
    resp->response_type.ok = (nktn_gesture_OkResponse)nktn_gesture_OkResponse_init_zero;
    return 0;
}

static int handle_set_cooldown_ms(const nktn_gesture_SetCooldownMsRequest *req,
                                  nktn_gesture_Response *resp) {
    LOG_DBG("Setting gesture cooldown_ms for id=%d to %d", req->id, req->cooldown_ms);

    const struct device *dev = zmk_gesture_processor_find_by_id(req->id);
    if (!dev) {
        LOG_WRN("Gesture processor not found: id=%d", req->id);
        return -ENODEV;
    }

    int ret = zmk_gesture_processor_set_cooldown_ms(dev, req->cooldown_ms);
    if (ret < 0) {
        LOG_ERR("Failed to set gesture cooldown_ms: %d", ret);
        return ret;
    }

    resp->which_response_type = nktn_gesture_Response_ok_tag;
    resp->response_type.ok = (nktn_gesture_OkResponse)nktn_gesture_OkResponse_init_zero;
    return 0;
}

#endif // CONFIG_ZMK_GESTURE_INPUT_PROCESSOR
