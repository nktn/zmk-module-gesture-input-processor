/**
 * ZMK Gesture Input Processor - Web UI
 * Configure trackball gesture processor instances at runtime via custom
 * ZMK Studio RPC.
 */

import { useContext, useState, useEffect, useMemo, useCallback } from "react";
import "./App.css";
import { connect as serial_connect } from "@zmkfirmware/zmk-studio-ts-client/transport/serial";
import {
  ZMKConnection,
  ZMKCustomSubsystem,
  ZMKAppContext,
} from "@cormoran/zmk-studio-react-hook";
import {
  Request,
  Response,
  GestureProcessorInfo,
  Notification,
} from "./proto/nktn/gesture/gesture";

// Custom subsystem identifier - must match firmware registration
export const SUBSYSTEM_IDENTIFIER = "nktn__gesture";

// Number of selectable layers exposed in the UI (bits 0-7 of active_layers).
const LAYER_COUNT = 8;

const THRESHOLD_MIN = 50;
const THRESHOLD_MAX = 3000;
const RESET_MS_MIN = 0;
const RESET_MS_MAX = 1000;
const COOLDOWN_MS_MIN = 0;
const COOLDOWN_MS_MAX = 2000;

function App() {
  return (
    <div className="app">
      <header className="app-header">
        <h1>🎯 Gesture Input Processor Settings</h1>
        <p>Configure trackball gesture behavior at runtime</p>
      </header>

      <ZMKConnection
        renderDisconnected={({ connect, isLoading, error }) => (
          <section className="card">
            <h2>Device Connection</h2>
            {isLoading && <p>⏳ Connecting...</p>}
            {error && (
              <div className="error-message">
                <p>🚨 {error}</p>
              </div>
            )}
            {!isLoading && (
              <button
                className="btn btn-primary"
                onClick={() => connect(serial_connect)}
              >
                🔌 Connect Serial
              </button>
            )}
          </section>
        )}
        renderConnected={({ disconnect, deviceName }) => (
          <>
            <section className="card">
              <h2>Device Connection</h2>
              <div className="device-info">
                <h3>✅ Connected to: {deviceName}</h3>
              </div>
              <button className="btn btn-secondary" onClick={disconnect}>
                Disconnect
              </button>
            </section>

            <GestureProcessorManager />
          </>
        )}
      />

      <footer className="app-footer">
        <p>
          <strong>Gesture Input Processor Module</strong> - Configure trackball
          gesture recognition
        </p>
      </footer>
    </div>
  );
}

export function GestureProcessorManager() {
  const zmkApp = useContext(ZMKAppContext);
  const [processors, setProcessors] = useState<GestureProcessorInfo[]>([]);
  const [selectedProcessorId, setSelectedProcessorId] = useState<number | null>(
    null
  );
  const [isLoading, setIsLoading] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [isUpdating, setIsUpdating] = useState(false);

  // Form state
  const [enabled, setEnabled] = useState<boolean>(false);
  const [activeLayers, setActiveLayers] = useState<number>(0);
  const [threshold, setThreshold] = useState<number>(THRESHOLD_MIN);
  const [resetMs, setResetMs] = useState<number>(RESET_MS_MIN);
  const [cooldownMs, setCooldownMs] = useState<number>(COOLDOWN_MS_MIN);

  const subsystem = useMemo(
    () => zmkApp?.findSubsystem(SUBSYSTEM_IDENTIFIER),
    // eslint-disable-next-line react-hooks/exhaustive-deps
    [zmkApp?.state.customSubsystems]
  );

  const callRPC = useCallback(
    async (request: Request): Promise<Response | null> => {
      if (!zmkApp?.state.connection || !subsystem) return null;
      try {
        const service = new ZMKCustomSubsystem(
          zmkApp.state.connection,
          subsystem.index
        );

        const payload = Request.encode(request).finish();
        const responsePayload = await service.callRPC(payload);

        if (responsePayload) {
          return Response.decode(responsePayload);
        }
      } catch (err) {
        console.error("RPC call failed:", err);
        throw err;
      }
      return null;
    },
    [zmkApp, subsystem]
  );

  const loadProcessors = useCallback(async () => {
    setIsLoading(true);
    setError(null);

    try {
      // Request list of gesture processors - notifications will be sent for
      // each instance.
      const request = Request.create({
        listProcessors: {},
      });

      const resp = await callRPC(request);
      if (resp?.error) {
        setError(resp.error.message);
      }
      // Response is empty - processors will arrive via notifications
    } catch (err) {
      setError(
        `Failed to load processors: ${err instanceof Error ? err.message : "Unknown error"}`
      );
    } finally {
      setIsLoading(false);
    }
  }, [callRPC]);

  const applyFieldUpdate = useCallback(
    async (
      condition: boolean,
      buildRequest: () => Request
    ): Promise<boolean> => {
      if (!condition) return true;
      const resp = await callRPC(buildRequest());
      if (resp?.error) {
        setError(resp.error.message);
        return false;
      }
      return true;
    },
    [callRPC]
  );

  const updateProcessor = useCallback(async () => {
    if (selectedProcessorId === null) return;

    const currentProcessor = processors.find(
      (p) => p.id === selectedProcessorId
    );
    if (!currentProcessor) return;

    setIsLoading(true);
    setError(null);
    setIsUpdating(true);

    try {
      // Only send requests for fields that have actually changed.
      if (
        !(await applyFieldUpdate(currentProcessor.enabled !== enabled, () =>
          Request.create({
            setEnabled: { id: selectedProcessorId, enabled },
          })
        ))
      ) {
        return;
      }

      if (
        !(await applyFieldUpdate(
          currentProcessor.activeLayers !== activeLayers,
          () =>
            Request.create({
              setActiveLayers: { id: selectedProcessorId, activeLayers },
            })
        ))
      ) {
        return;
      }

      if (
        !(await applyFieldUpdate(currentProcessor.threshold !== threshold, () =>
          Request.create({
            setThreshold: { id: selectedProcessorId, threshold },
          })
        ))
      ) {
        return;
      }

      if (
        !(await applyFieldUpdate(currentProcessor.resetMs !== resetMs, () =>
          Request.create({
            setResetMs: { id: selectedProcessorId, resetMs },
          })
        ))
      ) {
        return;
      }

      if (
        !(await applyFieldUpdate(
          currentProcessor.cooldownMs !== cooldownMs,
          () =>
            Request.create({
              setCooldownMs: { id: selectedProcessorId, cooldownMs },
            })
        ))
      ) {
        return;
      }

      // Updated values will come back via notifications.
    } catch (err) {
      setError(
        `Failed to update processor: ${err instanceof Error ? err.message : "Unknown error"}`
      );
    } finally {
      setIsLoading(false);
      setIsUpdating(false);
    }
  }, [
    applyFieldUpdate,
    processors,
    selectedProcessorId,
    enabled,
    activeLayers,
    threshold,
    resetMs,
    cooldownMs,
  ]);

  const applyProcessorToForm = useCallback((proc: GestureProcessorInfo) => {
    setEnabled(proc.enabled);
    setActiveLayers(proc.activeLayers);
    setThreshold(proc.threshold);
    setResetMs(proc.resetMs);
    setCooldownMs(proc.cooldownMs);
  }, []);

  const selectProcessor = useCallback(
    (id: number) => {
      const proc = processors.find((p) => p.id === id);
      if (proc) {
        setSelectedProcessorId(id);
        applyProcessorToForm(proc);
      }
    },
    [processors, applyProcessorToForm]
  );

  useEffect(() => {
    if (subsystem) {
      // Kick off the initial fetch; results arrive asynchronously via
      // notifications, not via this call's return value.
      // eslint-disable-next-line react-hooks/set-state-in-effect
      loadProcessors();
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [subsystem]);

  // Subscribe to notifications for processor changes
  useEffect(() => {
    if (!zmkApp || !subsystem) return;

    const unsubscribe = zmkApp.onNotification({
      type: "custom",
      subsystemIndex: subsystem.index,
      callback: (notification) => {
        try {
          // notification.payload contains the encoded Notification message
          const decoded = Notification.decode(notification.payload);
          const proc = decoded.processorState;
          if (!proc) return;

          // Update or add processor to the list
          setProcessors((prev) => {
            const existingIndex = prev.findIndex((p) => p.id === proc.id);
            if (existingIndex >= 0) {
              const updated = [...prev];
              updated[existingIndex] = proc;
              return updated;
            }
            return [...prev, proc];
          });

          // If this is the currently selected processor, update form values.
          // Skip updates if we're currently updating to prevent overwriting
          // user changes.
          if (selectedProcessorId === proc.id && !isUpdating) {
            applyProcessorToForm(proc);
          }

          // If no processor is selected yet, select the first one that
          // arrives.
          if (selectedProcessorId === null) {
            setSelectedProcessorId(proc.id);
            applyProcessorToForm(proc);
          }
        } catch (err) {
          console.error("Failed to decode notification:", err);
        }
      },
    });

    return unsubscribe;
  }, [
    zmkApp,
    subsystem,
    selectedProcessorId,
    isUpdating,
    applyProcessorToForm,
  ]);

  if (!zmkApp) return null;

  if (!subsystem) {
    return (
      <section className="card">
        <div className="warning-message">
          <p>
            ⚠️ Subsystem "{SUBSYSTEM_IDENTIFIER}" not found. Make sure your
            firmware includes the gesture input processor module.
          </p>
        </div>
      </section>
    );
  }

  const selectedProcessor = processors.find(
    (p) => p.id === selectedProcessorId
  );

  return (
    <>
      <section className="card">
        <h2>Gesture Processors</h2>
        {error && (
          <div className="error-message">
            <p>🚨 {error}</p>
          </div>
        )}

        <div style={{ marginBottom: "1rem" }}>
          <button
            className="btn btn-primary"
            onClick={loadProcessors}
            disabled={isLoading}
          >
            {isLoading ? "⏳ Loading..." : "🔄 Refresh List"}
          </button>
        </div>

        {processors.length === 0 && !isLoading && (
          <p>
            No gesture processors found. Configure them in your device tree.
          </p>
        )}

        {processors.length > 0 && (
          <div className="processor-list">
            {processors.map((proc) => (
              <div
                key={proc.id}
                className={`processor-item ${selectedProcessorId === proc.id ? "selected" : ""}`}
                onClick={() => selectProcessor(proc.id)}
              >
                <strong>{proc.name}</strong>
                <span
                  className={`badge ${proc.enabled ? "badge-enabled" : "badge-disabled"}`}
                >
                  {proc.enabled ? "Enabled" : "Disabled"}
                </span>
                <div className="processor-summary">
                  Threshold: {proc.threshold} | Reset: {proc.resetMs}ms |
                  Cooldown: {proc.cooldownMs}ms | Layers:{" "}
                  {proc.activeLayers === 0
                    ? "all"
                    : `0x${proc.activeLayers.toString(16)}`}
                </div>
              </div>
            ))}
          </div>
        )}
      </section>

      {selectedProcessorId !== null && (
        <section className="card">
          <h2>Configure: {selectedProcessor?.name}</h2>

          <div className="input-group">
            <label className="checkbox-label" htmlFor="gesture-enabled">
              <input
                id="gesture-enabled"
                type="checkbox"
                checked={enabled}
                onChange={(e) => setEnabled(e.target.checked)}
              />
              Enabled
            </label>
          </div>

          <hr style={{ margin: "1.5rem 0", border: "1px solid #e0e0e0" }} />

          <h3>Active Layers</h3>
          <p className="help-text">
            Select which layers this gesture processor is active on. None
            checked = active on all layers.
          </p>

          <div className="layer-grid">
            {Array.from({ length: LAYER_COUNT }, (_, i) => i).map((layer) => (
              <label key={layer} className="layer-checkbox">
                <input
                  type="checkbox"
                  checked={(activeLayers & (1 << layer)) !== 0}
                  onChange={(e) => {
                    if (e.target.checked) {
                      setActiveLayers(activeLayers | (1 << layer));
                    } else {
                      setActiveLayers(activeLayers & ~(1 << layer));
                    }
                  }}
                />
                Layer {layer}
              </label>
            ))}
          </div>
          <div className="layer-mask-readout">
            Bitmask: 0x{activeLayers.toString(16).padStart(2, "0")}{" "}
            {activeLayers === 0 && "(active on all layers)"}
          </div>

          <hr style={{ margin: "1.5rem 0", border: "1px solid #e0e0e0" }} />

          <h3>Threshold</h3>
          <p className="help-text">
            Accumulated movement counts required to fire a gesture (
            {THRESHOLD_MIN}-{THRESHOLD_MAX}).
          </p>
          <div className="input-group">
            <label htmlFor="threshold">Threshold:</label>
            <div className="range-row">
              <input
                id="threshold-range"
                type="range"
                min={THRESHOLD_MIN}
                max={THRESHOLD_MAX}
                value={threshold}
                onChange={(e) => setThreshold(parseInt(e.target.value))}
              />
              <input
                id="threshold"
                type="number"
                min={THRESHOLD_MIN}
                max={THRESHOLD_MAX}
                value={threshold}
                onChange={(e) =>
                  setThreshold(parseInt(e.target.value) || THRESHOLD_MIN)
                }
              />
            </div>
          </div>

          <h3>Reset Time</h3>
          <p className="help-text">
            Accumulated movement resets after this many ms without motion (
            {RESET_MS_MIN}-{RESET_MS_MAX}). 0 disables the automatic reset.
          </p>
          <div className="input-group">
            <label htmlFor="reset-ms">Reset (ms):</label>
            <input
              id="reset-ms"
              type="number"
              min={RESET_MS_MIN}
              max={RESET_MS_MAX}
              value={resetMs}
              onChange={(e) =>
                setResetMs(parseInt(e.target.value) || RESET_MS_MIN)
              }
            />
          </div>

          <h3>Cooldown</h3>
          <p className="help-text">
            Minimum ms between consecutive gesture firings ({COOLDOWN_MS_MIN}-
            {COOLDOWN_MS_MAX}).
          </p>
          <div className="input-group">
            <label htmlFor="cooldown-ms">Cooldown (ms):</label>
            <input
              id="cooldown-ms"
              type="number"
              min={COOLDOWN_MS_MIN}
              max={COOLDOWN_MS_MAX}
              value={cooldownMs}
              onChange={(e) =>
                setCooldownMs(parseInt(e.target.value) || COOLDOWN_MS_MIN)
              }
            />
          </div>

          <button
            className="btn btn-primary"
            onClick={updateProcessor}
            disabled={isLoading}
          >
            {isLoading ? "⏳ Applying..." : "✅ Apply Settings"}
          </button>
        </section>
      )}
    </>
  );
}

export default App;
