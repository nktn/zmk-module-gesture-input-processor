/**
 * Gesture Input Processor - Event Listener for Studio Notifications
 *
 * Listens to gesture processor state changed events and sends notifications
 * to Studio, following zmk-module-runtime-input-processor's
 * src/studio/input_processor_listener.c pattern.
 */

#include <nktn/gesture/gesture.pb.h>
#include <pb_encode.h>
#include <string.h>
#include <zephyr/logging/log.h>
#include <zmk/event_manager.h>
#include <zmk/events/gesture_processor_state_changed.h>
#include <zmk/studio/custom.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if IS_ENABLED(CONFIG_ZMK_GESTURE_INPUT_PROCESSOR_STUDIO_RPC)

// Encoder for the notification
static bool encode_notification(pb_ostream_t *stream, const pb_field_t *field, void *const *arg) {
    nktn_gesture_Notification *notification = (nktn_gesture_Notification *)*arg;
    if (!pb_encode_tag_for_field(stream, field)) {
        return false;
    }

    size_t size;
    if (!pb_get_encoded_size(&size, nktn_gesture_Notification_fields, notification)) {
        LOG_WRN("Failed to get encoded size for notification");
        return false;
    }

    if (!pb_encode_varint(stream, size)) {
        return false;
    }
    return pb_encode(stream, nktn_gesture_Notification_fields, notification);
}

// Find subsystem index by iterating through registered subsystems
static uint8_t find_subsystem_index(const char *identifier) {
    extern struct zmk_rpc_custom_subsystem _zmk_rpc_custom_subsystem_list_start[];
    extern struct zmk_rpc_custom_subsystem _zmk_rpc_custom_subsystem_list_end[];

    uint8_t index = 0;
    for (struct zmk_rpc_custom_subsystem *subsys = _zmk_rpc_custom_subsystem_list_start;
         subsys < _zmk_rpc_custom_subsystem_list_end; subsys++) {
        if (strcmp(subsys->identifier, identifier) == 0) {
            return index;
        }
        index++;
    }
    return 0; // Default to first subsystem if not found
}

static int gesture_processor_state_changed_listener(const zmk_event_t *eh) {
    const struct zmk_gesture_processor_state_changed *ev = as_zmk_gesture_processor_state_changed(eh);

    if (!ev) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    LOG_DBG("Gesture processor state changed: %s (id=%d)", ev->name, ev->id);

    nktn_gesture_Notification notification = nktn_gesture_Notification_init_zero;
    notification.which_notification_type = nktn_gesture_Notification_processor_state_tag;
    nktn_gesture_GestureProcessorInfo *info = &notification.notification_type.processor_state;

    // IMPORTANT: id must be set explicitly here (not left at the zero-initialized default).
    // Omitting it would make every processor's notification report id=0, and the web UI would
    // then overwrite whichever processor happens to be id 0 -- see the fix-notify-id lesson
    // referenced in raise_state_changed_event() in input_processor_gesture.c.
    info->id = ev->id;
    strncpy(info->name, ev->name, sizeof(info->name) - 1);
    info->name[sizeof(info->name) - 1] = '\0';
    info->enabled = ev->config.enabled;
    info->active_layers = ev->config.active_layers;
    info->threshold = ev->config.threshold;
    info->reset_ms = ev->config.reset_ms;
    info->cooldown_ms = ev->config.cooldown_ms;

    // Send notification via custom studio subsystem
    pb_callback_t encode_cb = {.funcs.encode = encode_notification, .arg = &notification};

    raise_zmk_studio_custom_notification((struct zmk_studio_custom_notification){
        .subsystem_index = find_subsystem_index("nktn__gesture"), .encode_payload = encode_cb});

    LOG_INF("Sent notification for gesture processor %s", ev->name);

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(gesture_processor_state_listener, gesture_processor_state_changed_listener);
ZMK_SUBSCRIPTION(gesture_processor_state_listener, zmk_gesture_processor_state_changed);

// NOTE: relay from peripheral is not required because gesture input processors are expected to
// be defined on the central side only (the trackball's input-listener chain, including this
// processor, lives wherever `zmk,input-split` reports events -- typically central).

#endif // CONFIG_ZMK_GESTURE_INPUT_PROCESSOR_STUDIO_RPC
