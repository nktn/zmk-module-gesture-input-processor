# Gesture Input Processor - Web Frontend

Web UI for configuring the ZMK gesture input processor module
(`zmk-module-gesture-input-processor`) over the custom Studio RPC subsystem
`nktn__gesture`.

## Features

- **Device Connection**: Connect to ZMK devices via Bluetooth (GATT) or Serial
- **Gesture Settings**: Per-instance enable toggle, active-layer mask,
  threshold, reset time, and cooldown — applied live and persisted on the
  device
- **React + TypeScript**: Modern web development with Vite for fast builds
- **react-zmk-studio**: Uses the `@cormoran/zmk-studio-react-hook` library for
  simplified ZMK integration

## Quick Start

```bash
# Install dependencies
npm install

# Generate TypeScript types from proto
npm run generate

# Run development server
npm run dev

# Build for production
npm run build

# Run tests
npm test
```

## Project Structure

```
src/
├── main.tsx              # React entry point
├── App.tsx               # Connection UI + GestureProcessorManager
├── App.css               # Styles
└── proto/                # Generated protobuf TypeScript types
    └── nktn/gesture/
        └── gesture.ts

test/
├── App.spec.tsx                     # Tests for App component
└── GestureProcessorManager.spec.tsx # Tests for the gesture settings UI
```

## How It Works

### 1. Protocol Definition

The protobuf schema is defined in `../proto/nktn/gesture/gesture.proto`. It is
the single source of truth shared with the firmware; do not edit the generated
`src/proto/` files by hand.

### 2. Code Generation

TypeScript types are generated using `ts-proto`:

```bash
npm run generate
```

This runs `buf generate` which uses the configuration in `buf.gen.yaml`.

### 3. Using react-zmk-studio

The app uses the `@cormoran/zmk-studio-react-hook` library:

```typescript
import { useZMKApp, ZMKCustomSubsystem } from "@cormoran/zmk-studio-react-hook";

// Connect to device
const { state, connect, findSubsystem, isConnected } = useZMKApp();

// Find the gesture subsystem
const subsystem = findSubsystem("nktn__gesture");

// Create service and make RPC calls
const service = new ZMKCustomSubsystem(state.connection, subsystem.index);
const response = await service.callRPC(payload);
```

### 4. RPC Flow

`ListProcessorsRequest` returns an empty response; the firmware then sends one
`Notification { processor_state }` per gesture processor instance. The UI
merges notifications by `id`. Field updates (`SetEnabledRequest`,
`SetActiveLayersRequest`, `SetThresholdRequest`, `SetResetMsRequest`,
`SetCooldownMsRequest`) are sent per changed field and confirmed via the same
notification channel.

## Testing

```bash
# Run all tests
npm test

# Run tests in watch mode
npm run test:watch

# Run tests with coverage
npm run test:coverage
```

### Writing Tests

Use the test helpers from `@cormoran/zmk-studio-react-hook/testing`:

```typescript
import {
  createConnectedMockZMKApp,
  ZMKAppProvider,
} from "@cormoran/zmk-studio-react-hook/testing";

const mockZMKApp = createConnectedMockZMKApp({
  deviceName: "Test Device",
  subsystems: ["nktn__gesture"],
});

render(
  <ZMKAppProvider value={mockZMKApp}>
    <YourComponent />
  </ZMKAppProvider>
);
```
