# ZMK Gesture Input Processor

![ZMK Version](https://img.shields.io/badge/ZMK-v0.3-blue)
[![Test](https://github.com/nktn/zmk-module-gesture-input-processor/actions/workflows/zmk-module.yml/badge.svg?branch=main)](https://github.com/nktn/zmk-module-gesture-input-processor/actions/workflows/zmk-module.yml)

This ZMK module turns a trackball (or any pointing device that emits `REL_X`/`REL_Y` input events) into a 4-way gesture pad. Flick the ball up, down, left, or right and the processor fires a behavior of your choice for that direction — handy for stacking extra shortcuts (tab switching, volume, layer taps, ...) onto a device that's normally just a cursor.

It uses the same **unofficial** custom ZMK Studio RPC protocol as [zmk-module-template-with-custom-studio-rpc](https://github.com/cormoran/zmk-module-template-with-custom-studio-rpc) and [zmk-module-runtime-input-processor](https://github.com/cormoran/zmk-module-runtime-input-processor), so gesture behavior can be tuned live from a browser without reflashing.

## How it works

The processor sits in an input-processor chain (typically in front of your pointer/scroll processors) and:

1. Accumulates `REL_X` / `REL_Y` movement into two per-axis counters.
2. Once `|accumulated X|` or `|accumulated Y|` reaches `threshold`, it resolves a direction — `REL_X` positive is **right**, `REL_Y` positive is **down** — and taps (press then release) the devicetree-bound behavior for that direction.
3. Both counters reset to 0 after firing, and further accumulation is suppressed for `cooldown-ms` so a single big flick doesn't fire repeatedly.
4. If no matching event arrives for `reset-ms`, the counters reset to 0 on their own, so a slow drift can never build up enough to fire a gesture.

While the processor is enabled and active for the current layer, it **consumes every matching `REL_X`/`REL_Y` event** — pointer motion / scrolling from processors later in the chain stops during a gesture. When disabled, or not active on the current layer, events pass through untouched.

## Features

- **4-direction gestures**: up / down / left / right, each bound to any ZMK behavior in devicetree (`&kp`, `&mo`, `&sk`, custom behaviors, ...)
- **Runtime configuration via Studio Web UI**: enable/disable, active layers, threshold, reset timeout, and cooldown — all adjustable live and persisted to flash
- **Active Layers**: restrict gesture recognition to specific layers using a bitmask (same convention as [zmk-module-runtime-input-processor](https://github.com/cormoran/zmk-module-runtime-input-processor))
- **Multiple instances**: define one processor per input device (e.g. one per trackball on a split keyboard)

## Setup

### 1. Add dependencies to your `config/west.yml`

```yaml
manifest:
  remotes:
    - name: nktn
      url-base: https://github.com/nktn
    - name: cormoran
      url-base: https://github.com/cormoran
  projects:
    - name: zmk-module-gesture-input-processor
      remote: nktn
      revision: main # or a pinned commit hash
      import: true
    # Required: patched ZMK with custom Studio RPC support
    - name: zmk
      remote: cormoran
      revision: v0.3-branch+custom-studio-protocol
      import:
        file: app/west.yml
```

### 2. Enable the feature in your `config/<shield>.conf`

```conf
CONFIG_ZMK_POINTING=y

# Enable the gesture input processor
CONFIG_ZMK_GESTURE_INPUT_PROCESSOR=y

# Optional: enable custom Studio RPC for the web UI
CONFIG_ZMK_STUDIO=y
CONFIG_ZMK_GESTURE_INPUT_PROCESSOR_STUDIO_RPC=y
CONFIG_ZMK_STUDIO_RPC_RX_BUF_SIZE=128
CONFIG_ZMK_LOW_PRIORITY_THREAD_STACK_SIZE=2048
```

### 3. Add a gesture processor to your keymap / overlay

```dts
#include <dt-bindings/zmk/keys.h>
#include <dt-bindings/zmk/input.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>

/ {
    trackball_gestures: trackball_gestures {
        compatible = "zmk,input-processor-gesture";
        processor-label = "tbgest"; // fits CONFIG_ZMK_GESTURE_INPUT_PROCESSOR_NAME_MAX_LEN (8 incl. NUL)

        // Fixed order: up, down, left, right
        bindings = <&kp UP>, <&kp DOWN>, <&kp LEFT>, <&kp RIGHT>;

        threshold = <600>;    // accumulated movement required to fire
        reset-ms = <150>;     // idle time before accumulation resets
        cooldown-ms = <200>;  // minimum time between two firings
        active-layers = <0>;  // 0 = active on all layers

        // Uncomment to have gestures recognized from firmware boot without
        // needing to enable them from the web UI first:
        // start-enabled;

        #input-processor-cells = <0>;
    };

    // Add it in front of your pointer/scroll processors so a gesture "eats"
    // the movement that triggered it instead of also moving the cursor.
    trackball_listener: trackball_listener {
        compatible = "zmk,input-listener";
        device = <&trackball>;
        input-processors = <&trackball_gestures &zip_xy_scaler 1 1>;
    };
};
```

For a split keyboard, define the processor on whichever side actually reports the trackball's `zmk,input-listener` (usually central, or wherever `zmk,input-split` delivers events).

## Usage

### Web UI (ZMK Studio)

1. Build and flash firmware with `CONFIG_ZMK_STUDIO=y` and `CONFIG_ZMK_GESTURE_INPUT_PROCESSOR_STUDIO_RPC=y`.
2. Open [ZMK Studio](https://zmk.studio) (or [dya studio](https://dya.studio), which also supports this unofficial custom RPC protocol) and connect to your keyboard via Web Serial/BLE.
3. Open the **Subsystems** tab and find `nktn__gesture` — this links to the web UI at `https://nktn.github.io/zmk-module-gesture-input-processor/`.
4. Each configured gesture processor instance appears with controls for:
   - **Enabled**: turn gesture recognition on/off
   - **Active Layers**: bitmask of layers where the processor is active (`0` = all layers)
   - **Threshold**: accumulated movement required to fire
   - **Reset (ms)**: idle time before accumulation resets
   - **Cooldown (ms)**: minimum time between firings
5. Changes apply immediately and are persisted to flash — no reflash needed.

See [web/README.md](./web/README.md) for web UI development instructions.

### Parameters

| Parameter        | DT property    | RPC field       | Default | Description                                            |
| ----------------- | --------------- | ---------------- | ------- | -------------------------------------------------------- |
| Processor label   | `processor-label` | -              | (required) | Short instance name (≤ 8 bytes incl. NUL); used as the settings key and in Studio |
| Direction bindings| `bindings`      | -                | (required) | Exactly 4 behaviors, in order: up, down, left, right    |
| Enabled            | `start-enabled` | `enabled`        | `false` | Whether the processor recognizes gestures                |
| Active layers      | `active-layers` | `active_layers`  | `0` (all layers) | Bitmask of layers where the processor is active |
| Threshold          | `threshold`     | `threshold`      | `600`   | Accumulated `\|REL_X\|`/`\|REL_Y\|` required to fire       |
| Reset timeout      | `reset-ms`      | `reset_ms`       | `150`   | Idle time (ms) before accumulation resets to 0            |
| Cooldown           | `cooldown-ms`   | `cooldown_ms`    | `200`   | Minimum time (ms) between two firings                     |

Runtime changes made through the RPC fields above are persisted to flash automatically (debounced) and immediately reflected back to any connected Studio client.

## Development Guide

### Setup for running tests

#### Option 1: Dev container

Not provided in this repository; see Option 2 below (isolated layout).

#### Option 2: Isolated directory layout

Set west topdir as repository root and download dependencies under `./dependencies`.

```bash
git clone <this repository>
cd <cloned directory>
west init -l west --mf west-test-isolated.yml
west update --narrow
west zephyr-export
```

#### Option 3: West workspace directory layout

Set west topdir as parent of repository root and download dependencies under `../`. Useful for sharing dependencies with other Zephyr module development.

```bash
mkdir west-workspace
cd west-workspace # this directory becomes west workspace root (topdir)
git clone <this repository>
west init -l . --mf west/west-test-workspace.yml
west update --narrow
west zephyr-export
```

### Pre-commit

Every commit needs to pass pre-commit verification (formatting + tests).

```bash
pip install pre-commit
pre-commit install

# Run pre-commit manually
pre-commit run --all-files
# Run for git staged files
pre-commit run
```

### Running tests

```bash
# Run unit test + build test and verify the results
python3 -m unittest
# Run build test directly
west zmk-build tests/zmk-config
# Run unit test directly
west zmk-test tests -m .
# Run web tests
cd web && npm test
```

`tests/zmk-config` builds three firmware artifacts (feature disabled, feature enabled with Studio RPC, feature enabled without Studio RPC) against a `zmk,input-processor-gesture` devicetree instance to verify the binding and driver compile and link. `tests/studio` boots a native_sim image and checks that the `nktn__gesture` custom Studio RPC subsystem registers successfully.

## Publishing Web UI

### GitHub Pages (Production)

1. Visit Settings > Pages
2. Set source as "GitHub Actions"
3. Visit Actions > "Test and Build Web UI"
4. Click "Run workflow"

The web UI will then be available at `https://nktn.github.io/zmk-module-gesture-input-processor/`.

### Cloudflare Workers (Pull Request Preview)

For previewing web UI changes in pull requests:

1. Create a Cloudflare Workers project and configure secrets:
   - `CLOUDFLARE_API_TOKEN`: API token with Cloudflare Pages edit permission
   - `CLOUDFLARE_ACCOUNT_ID`: Your Cloudflare account ID
   - (Optional) `CLOUDFLARE_PROJECT_NAME`: Project name (defaults to `zmk-module-web-ui`)
   - Enable "Preview URLs" in the Cloudflare project
2. Optionally set up an `approval-required` environment in repository settings requiring owner approval.
3. Open a pull request with web UI changes — the preview deployment triggers automatically and waits for approval.

## More Info

For more info on modules, see the [Zephyr modules page](https://docs.zephyrproject.org/3.5.0/develop/modules.html), [ZMK's page on using modules](https://zmk.dev/docs/features/modules), and [Zephyr's west manifest page](https://docs.zephyrproject.org/3.5.0/develop/west/manifest.html#west-manifests).
