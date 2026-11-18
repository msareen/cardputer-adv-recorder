import { useCallback, useEffect, useMemo, useState } from "react";
import { revealItemInDir } from "@tauri-apps/plugin-opener";
import { api, errorText, onAttributesChanged, type Attribute, type Recording } from "./api";
import { formatBytes, formatDay, formatDuration, formatTime } from "./format";
import { DayView, RecordingView, isoDay, recordingTitle as title, recordingWhen as when, sortAttributes } from "./Details";

interface Props {
  recordings: Recording[];
  playing: Recording | null;
  onPlay: (r: Recording) => void;
  onChanged: () => void;
}

/** What the Library shows: the list, one day, or one recording. */
type View =
  | { kind: "list" }
  | { kind: "day"; day: string }
  | { kind: "recording"; key: string; attrKind?: string; from?: View };

function attrKey(scope: string, target: string) {
  return `${scope}:${target}`;
}

/** Chips for the attributes agents saved; clicking one opens it. */
function AttributeChips({ items, onOpen }: { items: Attribute[] | undefined; onOpen: (a: Attribute) => void }) {
  if (!items?.length) return null;
  return (
    <span className="attr-chips">
      {sortAttributes(items).map((a) => (
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
  const [view, setView] = useState<View>({ kind: "list" });

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

  const openRecording = (r: Recording, attrKind?: string, from: View = view) =>
    setView({ kind: "recording", key: `${r.deviceId}/${r.id}`, attrKind, from });

  /** A chip in the list: recording attributes open the recording, day ones the day. */
  function openAttribute(attr: Attribute) {
    if (attr.scope === "day") {
      setView({ kind: "day", day: attr.target });
      return;
    }
    const r = recordings.find((x) => `${x.deviceId}/${x.id}` === attr.target);
    if (r) openRecording(r, attr.kind);
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

  if (view.kind === "day") {
    return (
      <DayView
        day={view.day}
        recordings={recordings}
        attributes={attributes}
        playing={playing}
        onPlay={onPlay}
        onOpenRecording={(r, k) => openRecording(r, k, view)}
        onBack={() => setView({ kind: "list" })}
      />
    );
  }
  if (view.kind === "recording") {
    const r = recordings.find((x) => `${x.deviceId}/${x.id}` === view.key);
    if (r) {
      return (
        <RecordingView
          key={view.key}
          recording={r}
          attributes={attributes}
          initialKind={view.attrKind}
          playing={playing}
          onPlay={onPlay}
          onOpenDay={(day) => setView({ kind: "day", day })}
          onBack={() => setView(view.from ?? { kind: "list" })}
        />
      );
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
              <div className="day">
                <button className="link day-link" onClick={() => setView({ kind: "day", day: isoDay(when(items[0])) })}>
                  {day} ›
                </button>
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
                        <button className="rec-title link-title" onClick={() => openRecording(r)}>
                          {title(r)}
                        </button>
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
                      <button onClick={() => openRecording(r)}>Details</button>
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
