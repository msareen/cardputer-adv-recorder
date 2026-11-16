import { useMemo, useState } from "react";
import { revealItemInDir } from "@tauri-apps/plugin-opener";
import { api, errorText, type Recording } from "./api";
import { formatBytes, formatDay, formatDuration, formatTime } from "./format";

interface Props {
  recordings: Recording[];
  playing: Recording | null;
  onPlay: (r: Recording) => void;
  onChanged: () => void;
}

function title(r: Recording) {
  return r.displayName ?? r.name.replace(/\.wav$/i, "");
}

function when(r: Recording) {
  return r.createdMs > 0 ? r.createdMs : r.syncedAtMs;
}

export function Library({ recordings, playing, onPlay, onChanged }: Props) {
  const [query, setQuery] = useState("");
  const [editing, setEditing] = useState<string | null>(null);
  const [draft, setDraft] = useState("");
  const [error, setError] = useState("");

  // Grouped by device, then by day, newest first (the list arrives sorted).
  const groups = useMemo(() => {
    const q = query.trim().toLowerCase();
    const matches = recordings.filter(
      (r) => !q || title(r).toLowerCase().includes(q) || r.name.toLowerCase().includes(q) || r.deviceName.toLowerCase().includes(q),
    );
    const byDevice = new Map<string, Map<string, Recording[]>>();
    for (const r of matches) {
      const days = byDevice.get(r.deviceName) ?? new Map<string, Recording[]>();
      const day = formatDay(when(r));
      days.set(day, [...(days.get(day) ?? []), r]);
      byDevice.set(r.deviceName, days);
    }
    return byDevice;
  }, [recordings, query]);

  async function saveRename(r: Recording) {
    try {
      await api.renameRecording(r.deviceId, r.id, draft);
      setEditing(null);
      onChanged();
    } catch (e) {
      setError(errorText(e));
    }
  }

  async function deleteFromDevice(r: Recording) {
    if (!window.confirm(`Delete "${title(r)}" from the device? The copy in your library is kept.`)) return;
    try {
      await api.deleteFromDevice(r.deviceId, r.id);
      onChanged();
    } catch (e) {
      setError(errorText(e));
    }
  }

  return (
    <section className="page">
      <header className="page-head">
        <div>
          <h2>Library</h2>
          <p className="muted">
            {recordings.length} recording{recordings.length === 1 ? "" : "s"} ·{" "}
            {formatDuration(recordings.reduce((sum, r) => sum + r.durationMs, 0))} total
          </p>
        </div>
        <input className="search" placeholder="Search" value={query} onChange={(e) => setQuery(e.target.value)} />
      </header>
      {error && (
        <div className="error" onClick={() => setError("")}>
          {error}
        </div>
      )}
      {recordings.length === 0 && (
        <div className="empty">
          <p>No recordings yet.</p>
          <p className="muted">Add your Cardputer under Devices and sync it.</p>
        </div>
      )}
      {[...groups.entries()].map(([device, days]) => (
        <div key={device} className="device-group">
          <h3>{device}</h3>
          {[...days.entries()].map(([day, items]) => (
            <div key={day} className="day-group">
              <div className="day">{day}</div>
              {items.map((r) => {
                const key = `${r.deviceId}/${r.id}`;
                const isPlaying = playing?.id === r.id && playing.deviceId === r.deviceId;
                return (
                  <div key={key} className={`rec ${isPlaying ? "active" : ""}`}>
                    <button className="play" onClick={() => onPlay(r)} title="Play">
                      {isPlaying ? "■" : "▶"}
                    </button>
                    <div className="rec-main">
                      {editing === key ? (
                        <form
                          onSubmit={(e) => {
                            e.preventDefault();
                            saveRename(r);
                          }}
                        >
                          <input
                            autoFocus
                            value={draft}
                            onChange={(e) => setDraft(e.target.value)}
                            onBlur={() => setEditing(null)}
                            onKeyDown={(e) => e.key === "Escape" && setEditing(null)}
                          />
                        </form>
                      ) : (
                        <div className="rec-title">{title(r)}</div>
                      )}
                      <div className="rec-meta">
                        {r.createdMs > 0 ? formatTime(r.createdMs) : "time unknown"} · {formatDuration(r.durationMs)} ·{" "}
                        {formatBytes(r.size)}
                        {r.displayName && <> · on device as {r.name}</>}
                        {!r.onDevice && <span className="tag">removed from device</span>}
                        {!r.acked && <span className="tag warn">ack pending</span>}
                      </div>
                    </div>
                    <div className="rec-actions">
                      <button
                        onClick={() => {
                          setEditing(key);
                          setDraft(title(r));
                        }}
                      >
                        Rename
                      </button>
                      <button onClick={() => revealItemInDir(r.path).catch((e) => setError(errorText(e)))}>
                        Show file
                      </button>
                      {r.onDevice && (
                        <button className="danger" onClick={() => deleteFromDevice(r)}>
                          Delete on device
                        </button>
                      )}
                    </div>
                  </div>
                );
              })}
            </div>
          ))}
        </div>
      ))}
    </section>
  );
}
