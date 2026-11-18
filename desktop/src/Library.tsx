import { useCallback, useEffect, useMemo, useState } from "react";
import { revealItemInDir } from "@tauri-apps/plugin-opener";
import { api, errorText, onAttributesChanged, type Attribute, type Recording } from "./api";
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

/** Local YYYY-MM-DD, the key day attributes use (same as the library folders). */
function isoDay(ms: number) {
  const d = new Date(ms);
  return `${d.getFullYear()}-${String(d.getMonth() + 1).padStart(2, "0")}-${String(d.getDate()).padStart(2, "0")}`;
}

function attrKey(scope: string, target: string) {
  return `${scope}:${target}`;
}

/** Chips for the attributes agents saved; clicking one opens it. */
function AttributeChips({ items, onOpen }: { items: Attribute[] | undefined; onOpen: (a: Attribute) => void }) {
  if (!items?.length) return null;
  return (
    <span className="attr-chips">
      {items.map((a) => (
        <button key={a.kind} className="attr-chip" title={`${a.mime} · by ${a.author}`} onClick={() => onOpen(a)}>
          {a.kind}
        </button>
      ))}
    </span>
  );
}

export function Library({ recordings, playing, onPlay, onChanged }: Props) {
  const [query, setQuery] = useState("");
  const [editing, setEditing] = useState<string | null>(null);
  const [draft, setDraft] = useState("");
  const [error, setError] = useState("");
  const [attributes, setAttributes] = useState<Map<string, Attribute[]>>(new Map());
  const [viewing, setViewing] = useState<{ attr: Attribute; content: string } | null>(null);

  const loadAttributes = useCallback(async () => {
    try {
      const map = new Map<string, Attribute[]>();
      for (const a of await api.listAttributes()) {
        const key = attrKey(a.scope, a.target);
        map.set(key, [...(map.get(key) ?? []), a]);
      }
      setAttributes(map);
    } catch (e) {
      setError(errorText(e));
    }
  }, []);

  useEffect(() => {
    loadAttributes();
    const unlisten = onAttributesChanged(loadAttributes);
    return () => {
      unlisten.then((f) => f());
    };
  }, [loadAttributes, recordings]);

  async function openAttribute(attr: Attribute) {
    try {
      setViewing({ attr, content: await api.readAttribute(attr.scope, attr.target, attr.kind) });
    } catch (e) {
      setError(errorText(e));
    }
  }

  async function deleteAttribute(attr: Attribute) {
    if (!window.confirm(`Delete the "${attr.kind}" attribute? The recording is not affected.`)) return;
    try {
      await api.deleteAttribute(attr.scope, attr.target, attr.kind);
      setViewing(null);
      loadAttributes();
    } catch (e) {
      setError(errorText(e));
    }
  }

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
      {viewing && (
        <div className="attr-viewer">
          <div className="attr-viewer-head">
            <b>{viewing.attr.kind}</b>
            <span className="muted small">
              {viewing.attr.scope === "day" ? `day ${viewing.attr.target}` : "recording"} · {viewing.attr.mime} · by{" "}
              {viewing.attr.author} · {new Date(viewing.attr.updatedMs).toLocaleString()}
            </span>
            <span className="spacer" />
            <button onClick={() => revealItemInDir(viewing.attr.path).catch((e) => setError(errorText(e)))}>
              Show file
            </button>
            <button className="danger" onClick={() => deleteAttribute(viewing.attr)}>
              Delete
            </button>
            <button onClick={() => setViewing(null)}>Close</button>
          </div>
          <pre className="attr-content">{viewing.content}</pre>
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
              <div className="day">
                {day}
                <AttributeChips items={attributes.get(attrKey("day", isoDay(when(items[0]))))} onOpen={openAttribute} />
              </div>
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
                      <AttributeChips
                        items={attributes.get(attrKey("recording", `${r.deviceId}/${r.id}`))}
                        onOpen={openAttribute}
                      />
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
