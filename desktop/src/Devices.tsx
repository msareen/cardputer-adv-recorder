import { useState } from "react";
import { api, errorText, type BleEstimate, type Device, type SyncEvent, type SyncReport } from "./api";
import { formatAgo, formatBytes } from "./format";
import { PairWizard, type PairMode } from "./PairWizard";

interface Props {
  devices: Device[];
  progress: Record<string, SyncEvent | undefined>;
  syncing: Set<string>;
  present: Set<string>;
  results: Record<string, { report?: SyncReport; error?: string; automatic?: boolean } | undefined>;
  estimates: Record<string, BleEstimate | undefined>;
  onSync: (deviceId: string, bluetooth?: boolean) => void;
  onChanged: () => void;
}

function progressText(e: SyncEvent | undefined): string {
  if (!e) return "Connecting…";
  switch (e.phase) {
    case "connected":
      return "Connected";
    case "listed":
      return e.count === 0 ? "Nothing new" : `${e.count} to check`;
    case "downloading":
      return `${e.name} (${e.index + 1}/${e.count}) ${Math.floor((e.done / Math.max(e.total, 1)) * 100)}%`;
    case "verifying":
      return `Verifying ${e.name}`;
    case "saved":
      return `Saved ${e.name}`;
    case "fileFailed":
      return `Failed: ${e.name}`;
  }
}

function resultText(report: SyncReport): string {
  const parts = [`${report.downloaded} new`];
  if (report.renamed) parts.push(`${report.renamed} renamed`);
  if (report.deletedOnDevice) parts.push(`${report.deletedOnDevice} cleared from device`);
  if (report.failed.length) parts.push(`${report.failed.length} failed`);
  return parts.join(" · ");
}

function estimateText(e: BleEstimate): string {
  if (e.bytes === 0) return "Over Bluetooth: nothing new to transfer";
  const minutes = Math.max(1, Math.round(e.secs / 60));
  return `Over Bluetooth: ${formatBytes(e.bytes)}, about ${minutes} min`;
}

export function Devices({ devices, progress, syncing, present, results, estimates, onSync, onChanged }: Props) {
  const [address, setAddress] = useState("");
  const [adding, setAdding] = useState(false);
  const [error, setError] = useState("");
  const [editing, setEditing] = useState<string | null>(null);
  const [draftName, setDraftName] = useState("");
  const [draftAddress, setDraftAddress] = useState("");
  const [wizard, setWizard] = useState<PairMode | null>(null);

  async function add(e: React.FormEvent) {
    e.preventDefault();
    setError("");
    setAdding(true);
    try {
      const device = await api.addDevice(address);
      setAddress("");
      onChanged();
      onSync(device.id);
    } catch (err) {
      setError(errorText(err));
    } finally {
      setAdding(false);
    }
  }

  async function saveEdit(d: Device) {
    try {
      if (draftName.trim() !== d.name) await api.renameDevice(d.id, draftName);
      if (draftAddress.trim() && draftAddress.trim() !== d.address) await api.setDeviceAddress(d.id, draftAddress);
      setEditing(null);
      onChanged();
    } catch (err) {
      setError(errorText(err));
    }
  }

  async function remove(d: Device) {
    if (!window.confirm(`Forget ${d.name} on this computer? Synced recordings stay in your library.`)) return;
    try {
      await api.removeDevice(d.id);
      onChanged();
    } catch (err) {
      setError(errorText(err));
    }
  }

  return (
    <section className="page">
      <header className="page-head">
        <div>
          <h2>Devices</h2>
          <p className="muted">
            Pair once over Bluetooth. Then press S on the Cardputer (or plug it in, with auto-sync on) and it
            syncs by itself when it is on the same network.
          </p>
        </div>
        {!wizard && (
          <button className="primary" onClick={() => setWizard({ kind: "new" })}>
            Pair new device
          </button>
        )}
      </header>

      {wizard?.kind === "new" && (
        <PairWizard key="new" mode={wizard} onClose={() => setWizard(null)} onPaired={onChanged} />
      )}

      <details className="dev-add">
        <summary>Developer: add an unpaired device by address (plain HTTP)</summary>
        <p className="muted small">
          For tools/mock-device and other dev servers. Traffic is not encrypted or authenticated.
        </p>
        <form className="add-device" onSubmit={add}>
          <input placeholder="127.0.0.1:8080" value={address} onChange={(e) => setAddress(e.target.value)} />
          <button disabled={adding || !address.trim()}>{adding ? "Connecting…" : "Add"}</button>
        </form>
      </details>
      {error && (
        <div className="error" onClick={() => setError("")}>
          {error}
        </div>
      )}

      <div className="device-list">
        {devices.map((d) => {
          const isSyncing = syncing.has(d.id);
          const result = results[d.id];
          return (
            <div key={d.id} className="device-card">
              {wizard?.kind === "wifi" && wizard.deviceId === d.id ? (
                // Opens in the card itself: at the top of the page it was
                // off-screen when the list was scrolled.
                <PairWizard key={d.id} mode={wizard} onClose={() => setWizard(null)} onPaired={onChanged} />
              ) : editing === d.id ? (
                <div className="device-edit">
                  <label>
                    Name
                    <input value={draftName} onChange={(e) => setDraftName(e.target.value)} />
                  </label>
                  <label>
                    Address
                    <input value={draftAddress} onChange={(e) => setDraftAddress(e.target.value)} />
                  </label>
                  <div className="row">
                    <button className="primary" onClick={() => saveEdit(d)}>
                      Save
                    </button>
                    <button onClick={() => setEditing(null)}>Cancel</button>
                  </div>
                </div>
              ) : (
                <>
                  <div className="device-head">
                    <div>
                      <div className="device-name">
                        {d.name}
                        <span className={`tag ${d.paired ? "secure" : "warn"}`}>
                          {d.paired ? "paired · TLS" : "dev · HTTP"}
                        </span>
                        {d.bleOnly && <span className="tag warn">Bluetooth only</span>}
                        {present.has(d.id) && <span className="tag live">on network</span>}
                      </div>
                      <div className="muted mono">
                        {d.id} · {d.bleOnly ? "no WiFi" : (d.address ?? "no address")}
                      </div>
                    </div>
                    <button
                      className="primary"
                      disabled={isSyncing}
                      title={d.bleOnly ? "Press S on the Cardputer first. Over Bluetooth, about 30 KB/s." : undefined}
                      onClick={() => onSync(d.id, d.bleOnly)}
                    >
                      {isSyncing ? "Syncing…" : "Sync now"}
                    </button>
                  </div>
                  <div className="stats">
                    <div>
                      <span>Last seen</span>
                      {formatAgo(d.lastSeenMs)}
                    </div>
                    <div>
                      <span>Battery</span>
                      {d.batteryPct == null ? "—" : `${d.batteryPct}%`}
                    </div>
                    <div>
                      <span>Unsynced</span>
                      {d.unsynced ?? "—"}
                    </div>
                    <div>
                      <span>Free</span>
                      {d.freeBytes == null ? "—" : formatBytes(d.freeBytes)}
                    </div>
                    <div>
                      <span>Firmware</span>
                      {d.firmware ?? "—"}
                    </div>
                  </div>
                  {!isSyncing && d.bleOnly && !result && (
                    <div className="sync-line muted small">
                      Bluetooth only: press S on the Cardputer, then Sync now. Slow (about 30 KB/s), no auto-sync.
                    </div>
                  )}
                  {isSyncing && estimates[d.id] && <div className="sync-line">{estimateText(estimates[d.id]!)}</div>}
                  {isSyncing && <div className="sync-line">{progressText(progress[d.id])}</div>}
                  {isSyncing && progress[d.id]?.phase === "downloading" && (
                    <div className="bar">
                      <div
                        style={{
                          width: `${(() => {
                            const e = progress[d.id];
                            return e && e.phase === "downloading" ? (e.done / Math.max(e.total, 1)) * 100 : 0;
                          })()}%`,
                        }}
                      />
                    </div>
                  )}
                  {!isSyncing && result?.report && (
                    <div className="sync-line ok">
                      {result.automatic ? "Auto-sync: " : ""}
                      {resultText(result.report)}
                    </div>
                  )}
                  {!isSyncing && result?.report?.failed.map((f) => (
                    <div key={f} className="sync-line bad">
                      {f}
                    </div>
                  ))}
                  {!isSyncing && result?.error && <div className="sync-line bad">{result.error}</div>}
                  <div className="row">
                    <button
                      onClick={() => {
                        setEditing(d.id);
                        setDraftName(d.name);
                        setDraftAddress(d.address ?? "");
                      }}
                    >
                      Edit
                    </button>
                    {d.paired && (
                      <button
                        onClick={() => setWizard({ kind: "wifi", deviceId: d.id, name: d.name, bleOnly: d.bleOnly })}
                      >
                        Change connection
                      </button>
                    )}
                    {d.paired && !d.bleOnly && (
                      <button
                        disabled={isSyncing}
                        title="When the Cardputer cannot reach WiFi: choose Sync over Bluetooth on it first. Slower (about 30 KB/s)."
                        onClick={() => onSync(d.id, true)}
                      >
                        Sync over Bluetooth
                      </button>
                    )}
                    <button className="danger" onClick={() => remove(d)}>
                      Remove
                    </button>
                  </div>
                </>
              )}
            </div>
          );
        })}
      </div>
    </section>
  );
}
