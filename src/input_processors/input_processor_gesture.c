/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_input_processor_gesture

#include <drivers/input_processor.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#if IS_ENABLED(CONFIG_SETTINGS)
#include <zephyr/settings/settings.h>
#endif

#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/gesture_processor_state_changed.h>
#include <zmk/input_processors/gesture.h>
#include <zmk/keymap.h>
#include <zmk/virtual_key_position.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

// Fixed devicetree "bindings" order, see
// dts/bindings/input_processors/zmk,input-processor-gesture.yaml
enum gesture_direction {
    GESTURE_DIR_UP = 0,
    GESTURE_DIR_DOWN = 1,
    GESTURE_DIR_LEFT = 2,
    GESTURE_DIR_RIGHT = 3,
    GESTURE_DIR_COUNT = 4,
};

// Upper bound for |accum_x| / |accum_y|, far above any sane threshold value. Keeps the
// accumulator comfortably inside int32_t so `-data->accum_x` / `-data->accum_y` can never be
// signed-overflow UB, regardless of how `threshold` and `reset_ms` are configured at runtime.
#define GESTURE_ACCUM_CLAMP 1000000

struct gesture_processor_config {
    uint8_t index; // devicetree instance index, used for virtual key position + settings key
    const char *name;
    const struct zmk_behavior_binding
        *bindings; // length GESTURE_DIR_COUNT, see enum gesture_direction
    uint32_t initial_threshold;
    uint32_t initial_reset_ms;
    uint32_t initial_cooldown_ms;
    uint32_t initial_active_layers;
    bool initial_enabled;
    uint32_t tap_ms; // compile-time only (devicetree); not exposed over RPC
};

struct gesture_processor_data {
    const struct device *dev;
#if IS_ENABLED(CONFIG_SETTINGS)
    struct k_work_delayable save_work;
#endif

    // Runtime-adjustable configuration (RPC + settings). There is no
    // separate "persistent vs temporary" split like
    // zmk-module-runtime-input-processor: every change made here is always
    // persisted (debounced), since this module has no temporary-hold-key
    // behavior that needs to restore a prior value.
    bool enabled;
    uint32_t active_layers;
    uint32_t threshold;
    uint32_t reset_ms;
    uint32_t cooldown_ms;

    // Movement accumulation state
    int32_t accum_x;
    int32_t accum_y;
    int64_t last_motion_timestamp; // 0 == no motion observed yet
    int64_t cooldown_until;        // k_uptime_get() timestamp; accumulation resumes after this

    // One-shot re-arm state (see gesture_processor_handle_event()). Only meaningful when
    // reset_ms > 0; when reset_ms == 0 the processor falls back to legacy continuous-fire
    // behavior and `armed` is left permanently true.
    bool armed;

    // Async tap dispatch. handle_event() runs on the input thread and must never block on USB
    // HID transmission (see module README / commit message for the real-hardware motivations),
    // so firing a gesture only records *what* to fire and offloads the actual
    // zmk_behavior_invoke_binding() press/release pair to the system workqueue.
    //
    // Concurrency: `tap_in_flight` is the single handoff flag between the input thread (producer)
    // and the system workqueue (consumer/producer of the eventual "done" transition):
    //   - The input thread is the only writer that ever transitions it 0 -> 1, and only does so
    //     right before writing pending_dir/pending_input_device_index and submitting press_work.
    //   - gesture_release_work_handler() (system workqueue) is the only writer that transitions it
    //     1 -> 0, after the release has been invoked.
    // Because each transition direction has exactly one writer, plain atomic_get()/atomic_set()
    // (no compare-and-swap) are enough to avoid a torn read/write; it's declared atomic_t rather
    // than a plain bool so the cross-thread handoff is explicit and safe even if this driver is
    // ever ported to an SMP target. pending_dir / pending_input_device_index themselves are plain
    // fields: the input thread only ever writes them while tap_in_flight is (about to become) 1,
    // and the workqueue only ever reads them after that submission, so there is no concurrent
    // access to guard beyond the flag itself. k_work_submit()/k_work_schedule() provide the
    // necessary memory barrier between "write fields, then submit" and "work runs, then read
    // fields".
    atomic_t tap_in_flight;
    enum gesture_direction pending_dir;
    uint8_t pending_input_device_index;
    struct k_work press_work;
    struct k_work_delayable release_work;
};

// Ported from zmk-module-runtime-input-processor's
// is_processor_active_for_current_layers() (input_processor_runtime.c:210-236).
static bool is_processor_active_for_current_layers(uint32_t active_layers_mask) {
    // If mask is 0, processor is active for all layers
    if (active_layers_mask == 0) {
        return true;
    }

    // Check only the layers that are set in the bitmask
    // This is more efficient than checking all layers
    uint32_t remaining_mask = active_layers_mask;
    int layer_idx = 0;

    while (remaining_mask != 0 && layer_idx < ZMK_KEYMAP_LAYERS_LEN) {
        // Check if this bit is set
        if (remaining_mask & 1) {
            zmk_keymap_layer_id_t layer_id = zmk_keymap_layer_index_to_id(layer_idx);

            if (layer_id != ZMK_KEYMAP_LAYER_ID_INVAL && zmk_keymap_layer_active(layer_id)) {
                return true;
            }
        }

        remaining_mask >>= 1;
        layer_idx++;
    }

    return false;
}

// Invoke one phase (press or release) of the tap for the behavior bound to the given direction,
// following the official zmk_behavior_invoke_binding() call pattern from ZMK core's
// app/src/pointing/input_processor_behaviors.c, including its use of
// ZMK_VIRTUAL_KEY_POSITION_BEHAVIOR_INPUT_PROCESSOR() to derive a virtual key position that
// doesn't collide with any real physical key position.
//
// Always called from the system workqueue (see gesture_press_work_handler() /
// gesture_release_work_handler() below), never from the input thread that detects the gesture --
// see the concurrency comment on gesture_processor_data::tap_in_flight for why.
static void invoke_gesture_tap_phase(const struct gesture_processor_config *cfg,
                                     uint8_t input_device_index, enum gesture_direction dir,
                                     bool pressed) {
    const struct zmk_behavior_binding *binding = &cfg->bindings[dir];

    // Per-phase timestamp: this is invoked from the system workqueue, potentially tap-ms after
    // the input event that triggered it, so k_uptime_get() at invoke time (rather than the
    // original input event's timestamp) is what behaviors like hold-taps should see.
    struct zmk_behavior_binding_event event_data = {
        .position =
            ZMK_VIRTUAL_KEY_POSITION_BEHAVIOR_INPUT_PROCESSOR(input_device_index, cfg->index),
        .timestamp = k_uptime_get(),
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
#endif
    };

    LOG_DBG("Gesture '%s' %s dir=%d -> %s", cfg->name, pressed ? "press" : "release", dir,
            binding->behavior_dev);

    zmk_behavior_invoke_binding(binding, event_data, pressed);
}

// System workqueue handler: invokes the press half of the tap, then schedules the release
// tap-ms later. Runs off the input thread so a slow/blocking behavior invocation (e.g. USB HID
// submission) can never stall input event processing.
static void gesture_press_work_handler(struct k_work *work) {
    struct gesture_processor_data *data =
        CONTAINER_OF(work, struct gesture_processor_data, press_work);
    const struct device *dev = data->dev;
    const struct gesture_processor_config *cfg = dev->config;

    invoke_gesture_tap_phase(cfg, data->pending_input_device_index, data->pending_dir, true);

    k_work_schedule(&data->release_work, K_MSEC(cfg->tap_ms));
}

// System workqueue handler: invokes the release half of the tap, tap-ms after the press. Clears
// tap_in_flight last, so no new gesture can be recorded/fired while the release is still pending
// (see the concurrency comment on gesture_processor_data::tap_in_flight).
static void gesture_release_work_handler(struct k_work *work) {
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct gesture_processor_data *data =
        CONTAINER_OF(dwork, struct gesture_processor_data, release_work);
    const struct device *dev = data->dev;
    const struct gesture_processor_config *cfg = dev->config;

    invoke_gesture_tap_phase(cfg, data->pending_input_device_index, data->pending_dir, false);

    atomic_set(&data->tap_in_flight, 0);
}

static int gesture_processor_handle_event(const struct device *dev, struct input_event *event,
                                          uint32_t param1, uint32_t param2,
                                          struct zmk_input_processor_state *state) {
    const struct gesture_processor_config *cfg = dev->config;
    struct gesture_processor_data *data = dev->data;

    if (event->type != INPUT_EV_REL || (event->code != INPUT_REL_X && event->code != INPUT_REL_Y)) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    if (!data->enabled || !is_processor_active_for_current_layers(data->active_layers)) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    int64_t now = k_uptime_get();

    // Reset accumulation if too much time passed since the last matching event, so a slow
    // drift can never build up enough to fire a gesture. When reset_ms > 0, the ball having been
    // still for reset_ms is *also* what re-arms the one-shot mechanism below -- reset_ms
    // therefore doubles as "accumulation reset time" and "re-arm idle time". Order matters here:
    // since_last_motion must be computed from the *previous* last_motion_timestamp before it's
    // overwritten with `now`.
    int64_t since_last_motion =
        data->last_motion_timestamp == 0 ? 0 : now - data->last_motion_timestamp;
    data->last_motion_timestamp = now;
    if (data->reset_ms > 0 && since_last_motion >= (int64_t)data->reset_ms) {
        data->accum_x = 0;
        data->accum_y = 0;
        data->armed = true;
    }

    bool in_cooldown = now < data->cooldown_until;

    // One-shot re-arm gate: once a gesture fires, this processor disarms itself and won't fire
    // again until the ball has been idle for reset_ms (handled above) -- this is what prevents
    // a trackball's mechanical inertia/coasting from re-triggering the same direction over and
    // over once the cooldown window passes (the real-hardware bug this mechanism fixes).
    // reset_ms == 0 opts back into the legacy behavior of firing every cooldown-ms while the
    // ball keeps rolling, so `armed` is intentionally never cleared in that mode.
    bool disarmed = data->reset_ms > 0 && !data->armed;

    // Block new gesture detection while a previously-fired tap's press/release pair hasn't
    // finished dispatching on the system workqueue yet. In one-shot mode this essentially never
    // triggers (tap_ms is a handful of ms, reset_ms/cooldown_ms are typically much larger), but
    // it also protects the reset_ms == 0 continuous-fire fallback from queuing a new press before
    // the previous tap's release has been invoked.
    bool tap_pending = atomic_get(&data->tap_in_flight) != 0;

    if (!in_cooldown && !disarmed && !tap_pending) {
        if (event->code == INPUT_REL_X) {
            data->accum_x += event->value;
        } else {
            data->accum_y += event->value;
        }

        // Clamp well inside the int32_t range: `threshold` is an RPC/DT-controlled uint32_t
        // and isn't capped to INT32_MAX, so without this a very large threshold combined with
        // reset_ms == 0 could let the accumulator grow until -accum_x below is signed-overflow
        // UB. GESTURE_ACCUM_CLAMP is far above any sane threshold, so this never affects normal
        // operation.
        if (data->accum_x > GESTURE_ACCUM_CLAMP) {
            data->accum_x = GESTURE_ACCUM_CLAMP;
        } else if (data->accum_x < -GESTURE_ACCUM_CLAMP) {
            data->accum_x = -GESTURE_ACCUM_CLAMP;
        }
        if (data->accum_y > GESTURE_ACCUM_CLAMP) {
            data->accum_y = GESTURE_ACCUM_CLAMP;
        } else if (data->accum_y < -GESTURE_ACCUM_CLAMP) {
            data->accum_y = -GESTURE_ACCUM_CLAMP;
        }

        // Compare as unsigned: `threshold` is a uint32_t that may legally be >= INT32_MAX
        // (e.g. via RPC), so casting it to int32_t could turn it negative and silently disable
        // firing. abs_x/abs_y are always representable and non-negative here thanks to the
        // clamp above.
        uint32_t abs_x = (uint32_t)(data->accum_x < 0 ? -data->accum_x : data->accum_x);
        uint32_t abs_y = (uint32_t)(data->accum_y < 0 ? -data->accum_y : data->accum_y);

        if (data->threshold > 0 && (abs_x >= data->threshold || abs_y >= data->threshold)) {
            // REL_X positive = right, REL_Y positive = down. Ties (equal |x| and |y|) resolve
            // to the horizontal axis.
            enum gesture_direction dir;
            if (abs_x >= abs_y) {
                dir = data->accum_x >= 0 ? GESTURE_DIR_RIGHT : GESTURE_DIR_LEFT;
            } else {
                dir = data->accum_y >= 0 ? GESTURE_DIR_DOWN : GESTURE_DIR_UP;
            }

            data->accum_x = 0;
            data->accum_y = 0;
            data->cooldown_until = now + (int64_t)data->cooldown_ms;
            if (data->reset_ms > 0) {
                // Disarm immediately; re-armed only once the ball has been idle for reset_ms
                // (handled at the top of this function).
                data->armed = false;
            }

            // Hand off the actual press/release dispatch to the system workqueue instead of
            // invoking it synchronously here -- this input-processor callback runs on the input
            // thread, and zmk_behavior_invoke_binding() can end up blocking on USB HID
            // submission. Fields are written before tap_in_flight is armed and
            // k_work_submit()/k_work_schedule() provide the memory barrier the workqueue side
            // relies on (see the concurrency comment on gesture_processor_data::tap_in_flight).
            data->pending_dir = dir;
            data->pending_input_device_index = state->input_device_index;
            atomic_set(&data->tap_in_flight, 1);
            k_work_submit(&data->press_work);
        }
    }

    // While enabled and active for the current layer(s), always consume matching events: the
    // pointer/scroll processors later in the chain must not see movement while the user is
    // performing (or cooling down from) a gesture, whether or not this particular event
    // caused a firing.
    return ZMK_INPUT_PROC_STOP;
}

static struct zmk_input_processor_driver_api gesture_processor_driver_api = {
    .handle_event = gesture_processor_handle_event,
};

#if IS_ENABLED(CONFIG_SETTINGS)
struct gesture_processor_settings {
    bool enabled;
    uint32_t active_layers;
    uint32_t threshold;
    uint32_t reset_ms;
    uint32_t cooldown_ms;
};

static void gesture_processor_save_work_handler(struct k_work *work) {
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct gesture_processor_data *data =
        CONTAINER_OF(dwork, struct gesture_processor_data, save_work);
    const struct device *dev = data->dev;
    const struct gesture_processor_config *cfg = dev->config;

    struct gesture_processor_settings settings = {
        .enabled = data->enabled,
        .active_layers = data->active_layers,
        .threshold = data->threshold,
        .reset_ms = data->reset_ms,
        .cooldown_ms = data->cooldown_ms,
    };

    char path[64];
    snprintf(path, sizeof(path), "gesture_ip/%s", cfg->name);

    int ret = settings_save_one(path, &settings, sizeof(settings));
    if (ret < 0) {
        LOG_ERR("Failed to save settings for %s: %d", cfg->name, ret);
    } else {
        LOG_INF("Saved settings for %s", cfg->name);
    }
}

static int schedule_save_gesture_processor_settings(const struct device *dev) {
    struct gesture_processor_data *data = dev->data;
    // Use a module-specific (and much shorter) debounce than ZMK's general
    // CONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE (60s by default): gesture parameters are commonly tuned
    // live from the web UI and then verified by power-cycling the board, and a 60s window meant
    // changes were routinely lost if the board was reset/unplugged before it elapsed.
    return k_work_reschedule(&data->save_work,
                             K_MSEC(CONFIG_ZMK_GESTURE_INPUT_PROCESSOR_SETTINGS_SAVE_DEBOUNCE_MS));
}

static int load_gesture_processor_settings_cb(const char *name, size_t len,
                                              settings_read_cb read_cb, void *cb_arg, void *param) {
    const struct device *dev = (const struct device *)param;
    struct gesture_processor_data *data = dev->data;
    const struct gesture_processor_config *cfg = dev->config;

    if (len != sizeof(struct gesture_processor_settings)) {
        return -EINVAL;
    }

    struct gesture_processor_settings settings;
    int rc = read_cb(cb_arg, &settings, sizeof(settings));
    if (rc < 0) {
        return rc;
    }

    data->enabled = settings.enabled;
    data->active_layers = settings.active_layers;
    data->threshold = settings.threshold;
    data->reset_ms = settings.reset_ms;
    data->cooldown_ms = settings.cooldown_ms;

    LOG_INF("Loaded settings for %s: enabled=%d, active_layers=0x%08x, threshold=%d, "
            "reset_ms=%d, cooldown_ms=%d",
            cfg->name, settings.enabled, settings.active_layers, settings.threshold,
            settings.reset_ms, settings.cooldown_ms);
    return 0;
}

static int gesture_processor_settings_load_cb(const char *name, size_t len,
                                              settings_read_cb read_cb, void *cb_arg);

SETTINGS_STATIC_HANDLER_DEFINE(gesture_ip, "gesture_ip", NULL, gesture_processor_settings_load_cb,
                               NULL, NULL);
#endif

static int gesture_processor_init(const struct device *dev) {
    const struct gesture_processor_config *cfg = dev->config;
    struct gesture_processor_data *data = dev->data;

    data->dev = dev;
    data->enabled = cfg->initial_enabled;
    data->active_layers = cfg->initial_active_layers;
    data->threshold = cfg->initial_threshold;
    data->reset_ms = cfg->initial_reset_ms;
    data->cooldown_ms = cfg->initial_cooldown_ms;

    data->accum_x = 0;
    data->accum_y = 0;
    data->last_motion_timestamp = 0;
    data->cooldown_until = 0;

    data->armed = true;
    atomic_set(&data->tap_in_flight, 0);
    data->pending_dir = GESTURE_DIR_UP;
    data->pending_input_device_index = 0;
    k_work_init(&data->press_work, gesture_press_work_handler);
    k_work_init_delayable(&data->release_work, gesture_release_work_handler);

#if IS_ENABLED(CONFIG_SETTINGS)
    k_work_init_delayable(&data->save_work, gesture_processor_save_work_handler);
#endif

    LOG_INF("Gesture input processor '%s' initialized", cfg->name);

    return 0;
}

// Forward declaration (defined later in this file) so the change notification below can
// report the correct processor id.
int zmk_gesture_processor_get_id(const struct device *dev);

// Helper to raise state changed event.
static void raise_state_changed_event(const struct device *dev) {
    const char *name;
    struct zmk_gesture_processor_config config;

    int ret = zmk_gesture_processor_get_config(dev, &name, &config);
    if (ret < 0) {
        return;
    }

    // IMPORTANT: id must always be resolved and set explicitly here. Omitting it defaults the
    // event (and the encoded notification) to id=0, which makes the Studio web UI mistake
    // every processor's update for processor 0 and overwrite the wrong instance's state -- the
    // exact bug zmk-module-runtime-input-processor's fix-notify-id commit addressed
    // (input_processor_runtime.c:683-707).
    int id = zmk_gesture_processor_get_id(dev);
    if (id < 0) {
        return;
    }

    raise_zmk_gesture_processor_state_changed((struct zmk_gesture_processor_state_changed){
        .id = (uint8_t)id, .name = name, .config = config});
}

int zmk_gesture_processor_set_enabled(const struct device *dev, bool enabled) {
    if (!dev) {
        return -EINVAL;
    }

    struct gesture_processor_data *data = dev->data;
    data->enabled = enabled;

    if (!enabled) {
        // Drop any in-flight accumulation and re-arm, so re-enabling starts clean and can fire
        // immediately rather than waiting for an idle period first. Note: any tap press/release
        // already queued on the system workqueue is intentionally left to run to completion --
        // cancelling it here could invoke a press without ever invoking the matching release.
        data->accum_x = 0;
        data->accum_y = 0;
        data->armed = true;
    }

    LOG_INF("Gesture enabled: %d", enabled);

    int ret = 0;
#if IS_ENABLED(CONFIG_SETTINGS)
    ret = schedule_save_gesture_processor_settings(dev);
#endif
    raise_state_changed_event(dev);

    return ret;
}

int zmk_gesture_processor_set_active_layers(const struct device *dev, uint32_t active_layers) {
    if (!dev) {
        return -EINVAL;
    }

    struct gesture_processor_data *data = dev->data;
    data->active_layers = active_layers;

    LOG_INF("Gesture active layers: 0x%08x", active_layers);

    int ret = 0;
#if IS_ENABLED(CONFIG_SETTINGS)
    ret = schedule_save_gesture_processor_settings(dev);
#endif
    raise_state_changed_event(dev);

    return ret;
}

int zmk_gesture_processor_set_threshold(const struct device *dev, uint32_t threshold) {
    if (!dev) {
        return -EINVAL;
    }
    if (threshold == 0) {
        return -EINVAL;
    }

    struct gesture_processor_data *data = dev->data;
    data->threshold = threshold;
    // Reset accumulation so a new, possibly lower, threshold can't be satisfied instantly by
    // movement that had already accumulated under the old threshold.
    data->accum_x = 0;
    data->accum_y = 0;

    LOG_INF("Gesture threshold: %d", threshold);

    int ret = 0;
#if IS_ENABLED(CONFIG_SETTINGS)
    ret = schedule_save_gesture_processor_settings(dev);
#endif
    raise_state_changed_event(dev);

    return ret;
}

int zmk_gesture_processor_set_reset_ms(const struct device *dev, uint32_t reset_ms) {
    if (!dev) {
        return -EINVAL;
    }

    struct gesture_processor_data *data = dev->data;
    data->reset_ms = reset_ms;

    LOG_INF("Gesture reset_ms: %d", reset_ms);

    int ret = 0;
#if IS_ENABLED(CONFIG_SETTINGS)
    ret = schedule_save_gesture_processor_settings(dev);
#endif
    raise_state_changed_event(dev);

    return ret;
}

int zmk_gesture_processor_set_cooldown_ms(const struct device *dev, uint32_t cooldown_ms) {
    if (!dev) {
        return -EINVAL;
    }

    struct gesture_processor_data *data = dev->data;
    data->cooldown_ms = cooldown_ms;

    LOG_INF("Gesture cooldown_ms: %d", cooldown_ms);

    int ret = 0;
#if IS_ENABLED(CONFIG_SETTINGS)
    ret = schedule_save_gesture_processor_settings(dev);
#endif
    raise_state_changed_event(dev);

    return ret;
}

int zmk_gesture_processor_get_config(const struct device *dev, const char **name,
                                     struct zmk_gesture_processor_config *config) {
    if (!dev) {
        return -EINVAL;
    }

    const struct gesture_processor_config *cfg = dev->config;
    struct gesture_processor_data *data = dev->data;

    if (name) {
        *name = cfg->name;
    }
    if (config) {
        config->enabled = data->enabled;
        config->active_layers = data->active_layers;
        config->threshold = data->threshold;
        config->reset_ms = data->reset_ms;
        config->cooldown_ms = data->cooldown_ms;
    }

    return 0;
}

#define GESTURE_PROCESSOR_INST(n)                                                                  \
    BUILD_ASSERT(DT_INST_PROP_LEN(n, bindings) == GESTURE_DIR_COUNT,                               \
                 "zmk,input-processor-gesture requires exactly 4 bindings: up, down, left, "       \
                 "right (in that order)");                                                         \
    BUILD_ASSERT(sizeof(DT_INST_PROP(n, processor_label)) <=                                       \
                     CONFIG_ZMK_GESTURE_INPUT_PROCESSOR_NAME_MAX_LEN,                              \
                 "processor_label " DT_INST_PROP(                                                  \
                     n, processor_label) " property +1 exceeds maximum "                           \
                                         "length " STRINGIFY(                                      \
                                             CONFIG_ZMK_GESTURE_INPUT_PROCESSOR_NAME_MAX_LEN));    \
    static const struct zmk_behavior_binding gesture_bindings_##n[GESTURE_DIR_COUNT] = {           \
        LISTIFY(DT_INST_PROP_LEN(n, bindings), ZMK_KEYMAP_EXTRACT_BINDING, (, ), DT_DRV_INST(n))}; \
    static const struct gesture_processor_config gesture_config_##n = {                            \
        .index = n,                                                                                \
        .name = DT_INST_PROP(n, processor_label),                                                  \
        .bindings = gesture_bindings_##n,                                                          \
        .initial_threshold = DT_INST_PROP_OR(n, threshold, 600),                                   \
        .initial_reset_ms = DT_INST_PROP_OR(n, reset_ms, 150),                                     \
        .initial_cooldown_ms = DT_INST_PROP_OR(n, cooldown_ms, 200),                               \
        .initial_active_layers = DT_INST_PROP_OR(n, active_layers, 0),                             \
        .initial_enabled = DT_INST_PROP(n, start_enabled),                                         \
        .tap_ms = DT_INST_PROP_OR(n, tap_ms, 10),                                                  \
    };                                                                                             \
    static struct gesture_processor_data gesture_data_##n;                                         \
    DEVICE_DT_INST_DEFINE(n, &gesture_processor_init, NULL, &gesture_data_##n,                     \
                          &gesture_config_##n, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,   \
                          &gesture_processor_driver_api);

DT_INST_FOREACH_STATUS_OKAY(GESTURE_PROCESSOR_INST)

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)
#define GESTURE_DEVICE_ADDR(idx) DEVICE_DT_GET(DT_DRV_INST(idx)),

static const struct device *gesture_processors[] = {
    DT_INST_FOREACH_STATUS_OKAY(GESTURE_DEVICE_ADDR)};

static const size_t gesture_processors_count =
    sizeof(gesture_processors) / sizeof(gesture_processors[0]);

#else

static const struct device *gesture_processors[] = {};
static const size_t gesture_processors_count = 0;

#endif

int zmk_gesture_processor_foreach(int (*callback)(const struct device *dev, void *user_data),
                                  void *user_data) {
    for (size_t i = 0; i < gesture_processors_count; i++) {
        int ret = callback(gesture_processors[i], user_data);
        if (ret != 0) {
            return ret;
        }
    }
    return 0;
}

const struct device *zmk_gesture_processor_find_by_name(const char *name) {
    for (size_t i = 0; i < gesture_processors_count; i++) {
        const struct device *dev = gesture_processors[i];
        const struct gesture_processor_config *cfg = dev->config;
        if (strcmp(cfg->name, name) == 0) {
            return dev;
        }
    }

    return NULL;
}

const struct device *zmk_gesture_processor_find_by_id(uint8_t id) {
    if (id < gesture_processors_count) {
        return gesture_processors[id];
    }
    return NULL;
}

int zmk_gesture_processor_get_id(const struct device *dev) {
    for (size_t i = 0; i < gesture_processors_count; i++) {
        if (gesture_processors[i] == dev) {
            return (int)i;
        }
    }
    return -1;
}

#if IS_ENABLED(CONFIG_SETTINGS)

static int gesture_processor_settings_load_cb(const char *name, size_t len,
                                              settings_read_cb read_cb, void *cb_arg) {
    for (size_t i = 0; i < gesture_processors_count; i++) {
        const struct device *dev = gesture_processors[i];
        const struct gesture_processor_config *cfg = dev->config;
        if (strcmp(name, cfg->name) == 0) {
            return load_gesture_processor_settings_cb(name, len, read_cb, cb_arg, (void *)dev);
        }
    }
    return -ENOENT;
}

#endif
