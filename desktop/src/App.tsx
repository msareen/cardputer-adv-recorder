import { useCallback, useEffect, useState } from "react";
import { convertFileSrc } from "@tauri-apps/api/core";
import {
  api,
  errorText,
  onPresenceChanged,
  onBleEstimate,
  onSyncFinished,
  onSyncProgress,
  type BleEstimate,
  onSyncStarted,
  type Device,
  type Recording,
  type SyncEvent,
  type SyncReport,
  type User,
} from "./api";
import { Devices } from "./Devices";
import { Library } from "./Library";
import { Login } from "./Login";
import { Settings } from "./Settings";
import "./App.css";

type Page = "library" | "devices" | "settings";

function App() {
  const [user, setUser] = useState<User | null | undefined>(undefined);
  const [page, setPage] = useState<Page>("library");
  const [recordings, setRecordings] = useState<Recording[]>([]);
  const [devices, setDevices] = useState<Device[]>([]);
  const [playing, setPlaying] = useState<Recording | null>(null);
  const [syncing, setSyncing] = useState<Set<string>>(new Set());
  const [progress, setProgress] = useState<Record<string, SyncEvent | undefined>>({});
  const [results, setResults] = useState<
    Record<string, { report?: SyncReport; error?: string; automatic?: boolean } | undefined>
  >({});
  const [present, setPresent] = useState<Set<string>>(new Set());
  const [estimates, setEstimates] = useState<Record<string, BleEstimate | undefined>>({});

  useEffect(() => {
    api.session().then(setUser);
  }, []);

  const refresh = useCallback(async () => {
    const [r, d] = await Promise.all([api.listRecordings(), api.listDevices()]);
    setRecordings(r);
    setDevices(d);
  }, []);

  useEffect(() => {
    if (user) {
      refresh();
      api.presentDevices().then((ids) => setPresent(new Set(ids)));
    }
  }, [user, refresh]);

  // Syncs can start in the background (discovery worker), so the UI follows
  // events rather than only the calls it made itself.
  useEffect(() => {
    const subscriptions = [
      onSyncStarted((e) => {
        setSyncing((s) => new Set(s).add(e.deviceId));
        setProgress((p) => ({ ...p, [e.deviceId]: undefined }));
        setResults((r) => ({ ...r, [e.deviceId]: undefined }));
        setEstimates((m) => ({ ...m, [e.deviceId]: undefined }));
      }),
      onBleEstimate((e) => setEstimates((m) => ({ ...m, [e.deviceId]: e }))),
      onSyncFinished((e) => {
        setSyncing((s) => {
          const next = new Set(s);
          next.delete(e.deviceId);
          return next;
        });
        setResults((r) => ({
          ...r,
          [e.deviceId]: { report: e.report ?? undefined, error: e.error ?? undefined, automatic: e.automatic },
        }));
        refresh();
      }),
      onPresenceChanged((ids) => {
        setPresent(new Set(ids));
        refresh();
      }),
    ];
    return () => {
      subscriptions.forEach((s) => s.then((f) => f()));
    };
  }, [refresh]);

  useEffect(() => {
    const unlisten = onSyncProgress((p) => {
      setProgress((prev) => ({ ...prev, [p.deviceId]: p.event }));
      if (p.event.phase === "saved") refresh();
    });
    return () => {
      unlisten.then((f) => f());
    };
  }, [refresh]);

  const sync = useCallback(async (deviceId: string, bluetooth = false) => {
    try {
      await (bluetooth ? api.syncDeviceBle(deviceId) : api.syncDevice(deviceId));
    } catch (e) {
      // Errors after the sync started arrive as sync-finished; this covers
      // ones before it (no address, already syncing).
      setResults((r) => (r[deviceId] ? r : { ...r, [deviceId]: { error: errorText(e) } }));
    }
  }, []);

  if (user === undefined) return null;
  if (user === null) return <Login onLoggedIn={setUser} />;

  return (
    <div className="shell">
      <nav className="sidebar">
        <div className="brand">
          <span className="brand-dot" />
          Cardputer Sync
        </div>
        {(["library", "devices", "settings"] as Page[]).map((p) => (
          <button key={p} className={`nav ${page === p ? "current" : ""}`} onClick={() => setPage(p)}>
            {p[0].toUpperCase() + p.slice(1)}
            {p === "devices" && syncing.size > 0 && <span className="pulse" />}
          </button>
        ))}
        <div className="sidebar-foot">
          <div className="muted">{user.username}</div>
          <button
            className="link"
            onClick={async () => {
              await api.logout();
              setPlaying(null);
              setRecordings([]);
              setDevices([]);
              setUser(null);
            }}
          >
            Log out
          </button>
        </div>
      </nav>
      <main className="content">
        {page === "library" && (
          <Library
            recordings={recordings}
            playing={playing}
            onPlay={(r) => setPlaying(playing?.id === r.id && playing.deviceId === r.deviceId ? null : r)}
            onChanged={refresh}
          />
        )}
        {page === "devices" && (
          <Devices
            devices={devices}
            progress={progress}
            syncing={syncing}
            present={present}
            results={results}
            estimates={estimates}
            onSync={sync}
            onChanged={refresh}
          />
        )}
        {page === "settings" && <Settings user={user} onUser={setUser} />}
      </main>
      {playing && (
        <footer className="player">
          <div className="player-title">{playing.displayName ?? playing.name}</div>
          <audio key={playing.path} src={convertFileSrc(playing.path)} controls autoPlay onEnded={() => setPlaying(null)} />
        </footer>
      )}
    </div>
  );
}

export default App;
