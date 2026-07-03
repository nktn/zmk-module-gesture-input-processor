import { act, render, screen, waitFor } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import {
  createConnectedMockZMKApp,
  ZMKAppProvider,
} from "@cormoran/zmk-studio-react-hook/testing";
import type { CustomNotification } from "@zmkfirmware/zmk-studio-ts-client/custom";
import { GestureProcessorManager, SUBSYSTEM_IDENTIFIER } from "../src/App";
import {
  GestureProcessorInfo,
  Notification,
  Request,
  Response,
} from "../src/proto/nktn/gesture/gesture";

// Mock the underlying RPC transport so callRPC() never hits real hardware.
jest.mock("@zmkfirmware/zmk-studio-ts-client", () => ({
  create_rpc_connection: jest.fn(),
  call_rpc: jest.fn(),
}));

// eslint-disable-next-line @typescript-eslint/no-require-imports
const { call_rpc } = require("@zmkfirmware/zmk-studio-ts-client");

const SAMPLE_PROCESSOR: GestureProcessorInfo = {
  id: 1,
  name: "Left Trackball",
  enabled: false,
  activeLayers: 0,
  threshold: 800,
  resetMs: 300,
  cooldownMs: 150,
};

function encodeNotification(proc: GestureProcessorInfo): Uint8Array {
  return Notification.encode(
    Notification.create({ processorState: proc })
  ).finish();
}

function mockOkResponse() {
  return {
    custom: {
      call: {
        subsystemIndex: 0,
        payload: Response.encode(Response.create({ ok: {} })).finish(),
      },
    },
  };
}

function mockListProcessorsResponse() {
  return {
    custom: {
      call: {
        subsystemIndex: 0,
        payload: Response.encode(
          Response.create({ listProcessors: {} })
        ).finish(),
      },
    },
  };
}

describe("GestureProcessorManager Component", () => {
  beforeEach(() => {
    jest.clearAllMocks();
    // The initial mount effect always fires a ListProcessorsRequest; give it
    // a benign default response so it doesn't throw in tests that don't care
    // about it.
    (call_rpc as jest.Mock).mockResolvedValue(mockListProcessorsResponse());
  });

  describe("Without ZMKAppContext", () => {
    it("should not render when ZMKAppContext is not provided", () => {
      const { container } = render(<GestureProcessorManager />);
      expect(container.firstChild).toBeNull();
    });
  });

  describe("Without Subsystem", () => {
    it("should show warning when subsystem is not found", () => {
      const mockZMKApp = createConnectedMockZMKApp({
        deviceName: "Test Device",
        subsystems: [],
      });

      render(
        <ZMKAppProvider value={mockZMKApp}>
          <GestureProcessorManager />
        </ZMKAppProvider>
      );

      expect(
        screen.getByText(/Subsystem "nktn__gesture" not found/i)
      ).toBeInTheDocument();
      expect(
        screen.getByText(
          /Make sure your firmware includes the gesture input processor module/i
        )
      ).toBeInTheDocument();
    });
  });

  describe("With Subsystem", () => {
    it("should render an empty processor list before any notification arrives", async () => {
      const mockZMKApp = createConnectedMockZMKApp({
        deviceName: "Test Device",
        subsystems: [SUBSYSTEM_IDENTIFIER],
      });

      render(
        <ZMKAppProvider value={mockZMKApp}>
          <GestureProcessorManager />
        </ZMKAppProvider>
      );

      expect(screen.getByText(/Gesture Processors/i)).toBeInTheDocument();
      await waitFor(() => {
        expect(
          screen.getByText(/No gesture processors found/i)
        ).toBeInTheDocument();
      });
    });

    it("should populate the processor list and form from a notification", async () => {
      let notifyCallback: ((notification: CustomNotification) => void) | null =
        null;

      const mockZMKApp = createConnectedMockZMKApp({
        deviceName: "Test Device",
        subsystems: [SUBSYSTEM_IDENTIFIER],
      });
      mockZMKApp.onNotification = jest.fn((subscription) => {
        if (subscription.type === "custom") {
          notifyCallback = subscription.callback;
        }
        return () => {};
      });

      render(
        <ZMKAppProvider value={mockZMKApp}>
          <GestureProcessorManager />
        </ZMKAppProvider>
      );

      act(() => {
        notifyCallback?.({
          subsystemIndex: 0,
          payload: encodeNotification(SAMPLE_PROCESSOR),
        });
      });

      await waitFor(() => {
        expect(screen.getByText("Left Trackball")).toBeInTheDocument();
      });

      expect(
        screen.getByText(/Configure: Left Trackball/i)
      ).toBeInTheDocument();

      const enabledCheckbox = screen.getByRole("checkbox", {
        name: "Enabled",
      }) as HTMLInputElement;
      expect(enabledCheckbox.checked).toBe(false);

      const thresholdInput = screen.getByLabelText(
        "Threshold:"
      ) as HTMLInputElement;
      expect(thresholdInput.value).toBe("800");

      const resetInput = screen.getByLabelText(
        "Reset (ms):"
      ) as HTMLInputElement;
      expect(resetInput.value).toBe("300");

      const cooldownInput = screen.getByLabelText(
        "Cooldown (ms):"
      ) as HTMLInputElement;
      expect(cooldownInput.value).toBe("150");
    });

    it("should update the active-layers bitmask readout when a layer checkbox is toggled", async () => {
      let notifyCallback: ((notification: CustomNotification) => void) | null =
        null;

      const mockZMKApp = createConnectedMockZMKApp({
        deviceName: "Test Device",
        subsystems: [SUBSYSTEM_IDENTIFIER],
      });
      mockZMKApp.onNotification = jest.fn((subscription) => {
        if (subscription.type === "custom") {
          notifyCallback = subscription.callback;
        }
        return () => {};
      });

      render(
        <ZMKAppProvider value={mockZMKApp}>
          <GestureProcessorManager />
        </ZMKAppProvider>
      );

      act(() => {
        notifyCallback?.({
          subsystemIndex: 0,
          payload: encodeNotification(SAMPLE_PROCESSOR),
        });
      });

      await waitFor(() => {
        expect(screen.getByText("Left Trackball")).toBeInTheDocument();
      });

      expect(screen.getByText(/Bitmask: 0x00/i)).toBeInTheDocument();

      const user = userEvent.setup();
      await user.click(screen.getByRole("checkbox", { name: "Layer 2" }));

      expect(screen.getByText(/Bitmask: 0x04/i)).toBeInTheDocument();
    });

    it("should only send requests for fields that changed when applying settings", async () => {
      let notifyCallback: ((notification: CustomNotification) => void) | null =
        null;

      const mockZMKApp = createConnectedMockZMKApp({
        deviceName: "Test Device",
        subsystems: [SUBSYSTEM_IDENTIFIER],
      });
      mockZMKApp.onNotification = jest.fn((subscription) => {
        if (subscription.type === "custom") {
          notifyCallback = subscription.callback;
        }
        return () => {};
      });

      render(
        <ZMKAppProvider value={mockZMKApp}>
          <GestureProcessorManager />
        </ZMKAppProvider>
      );

      act(() => {
        notifyCallback?.({
          subsystemIndex: 0,
          payload: encodeNotification(SAMPLE_PROCESSOR),
        });
      });

      await waitFor(() => {
        expect(screen.getByText("Left Trackball")).toBeInTheDocument();
      });

      // Discard the ListProcessorsRequest call made on mount so the
      // assertions below only count calls triggered by "Apply Settings".
      (call_rpc as jest.Mock).mockClear();
      (call_rpc as jest.Mock).mockResolvedValue(mockOkResponse());

      const user = userEvent.setup();
      await user.click(screen.getByRole("checkbox", { name: "Enabled" }));
      await user.click(screen.getByText(/Apply Settings/i));

      await waitFor(() => {
        expect(call_rpc).toHaveBeenCalledTimes(1);
      });

      const [, rpcRequest] = (call_rpc as jest.Mock).mock.calls[0];
      const gestureRequest = Request.decode(rpcRequest.custom.call.payload);
      expect(gestureRequest.setEnabled).toEqual({ id: 1, enabled: true });
    });
  });
});
