/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <zephyr/device.h>

/**
 * @brief Runtime-adjustable configuration of a gesture input processor
 * instance.
 *
 * Direction -> behavior bindings are fixed in devicetree and intentionally
 * not exposed here (see proto/nktn/gesture/gesture.proto).
 */
struct zmk_gesture_processor_config {
    bool enabled;
    uint32_t active_layers;
    uint32_t threshold;
    uint32_t reset_ms;
    uint32_t cooldown_ms;
};

/**
 * @brief Enable or disable gesture recognition for one processor instance.
 *
 * The change is persisted to settings (debounced) and a
 * zmk_gesture_processor_state_changed event is raised.
 */
int zmk_gesture_processor_set_enabled(const struct device *dev, bool enabled);

/**
 * @brief Set the bitmask of layers where the processor is active.
 *
 * @param active_layers 0 = active on all layers, otherwise bit N = layer N.
 */
int zmk_gesture_processor_set_active_layers(const struct device *dev, uint32_t active_layers);

/**
 * @brief Set the accumulated-movement threshold required to fire a gesture.
 */
int zmk_gesture_processor_set_threshold(const struct device *dev, uint32_t threshold);

/**
 * @brief Set the idle time (ms) after which accumulated movement resets to 0.
 */
int zmk_gesture_processor_set_reset_ms(const struct device *dev, uint32_t reset_ms);

/**
 * @brief Set the minimum time (ms) between two gesture firings.
 */
int zmk_gesture_processor_set_cooldown_ms(const struct device *dev, uint32_t cooldown_ms);

/**
 * @brief Get the current configuration and processor-label name of an
 * instance.
 *
 * @param dev Pointer to the device structure.
 * @param name Pointer to store the processor-label name (can be NULL).
 * @param config Pointer to store the configuration (can be NULL).
 * @return 0 on success, negative error code on failure.
 */
int zmk_gesture_processor_get_config(const struct device *dev, const char **name,
                                     struct zmk_gesture_processor_config *config);

/**
 * @brief Find a gesture processor instance by its processor-label name.
 */
const struct device *zmk_gesture_processor_find_by_name(const char *name);

/**
 * @brief Find a gesture processor instance by its 0-based registration id.
 *
 * The id is stable for a given firmware build (assigned by devicetree
 * instantiation order) and is what the custom Studio RPC protocol and
 * settings notifications use to identify an instance.
 */
const struct device *zmk_gesture_processor_find_by_id(uint8_t id);

/**
 * @brief Get the 0-based registration id of a gesture processor instance.
 *
 * @return The id, or -1 if the device is not a registered gesture processor.
 */
int zmk_gesture_processor_get_id(const struct device *dev);

/**
 * @brief Iterate over all gesture processor instances.
 *
 * @param callback Callback invoked once per instance. Iteration stops early
 * if it returns non-zero.
 * @param user_data Passed through to callback.
 * @return 0 on success, or the first non-zero value returned by callback.
 */
int zmk_gesture_processor_foreach(int (*callback)(const struct device *dev, void *user_data),
                                  void *user_data);
