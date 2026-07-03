/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/kernel.h>
#include <zmk/event_manager.h>
#include <zmk/input_processors/gesture.h>

struct zmk_gesture_processor_state_changed {
    uint8_t id;
    const char *name;
    struct zmk_gesture_processor_config config;
};

ZMK_EVENT_DECLARE(zmk_gesture_processor_state_changed);
