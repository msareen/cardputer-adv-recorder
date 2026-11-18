// Drill-down views for the Library: one day (across devices) and one
// recording, with the attributes agents saved (summary, transcript...) as tabs.
import { ask } from "@tauri-apps/plugin-dialog";
import { useEffect, useState, type ReactNode } from "react";
import { revealItemInDir } from "@tauri-apps/plugin-opener";
import { api, errorText, type Attribute, type Recording } from "./api";
import { formatBytes, formatDay, formatDuration, formatTime } from "./format";

export function recordingTitle(r: Recording) {
  return r.displayName ?? r.name.replace(/\.wav$/i, "");
}

export function recordingWhen(r: Recording) {
  return r.createdMs > 0 ? r.createdMs : r.syncedAtMs;
}

/** Local YYYY-MM-DD, the key day attributes use (same as the library folders). */
export function isoDay(ms: number) {
  const d = new Date(ms);
  return `${d.getFullYear()}-${String(d.getMonth() + 1).padStart(2, "0")}-${String(d.getDate()).padStart(2, "0")}`;
}

export function dayLabel(day: string) {
  const [y, m, d] = day.split("-").map(Number);
  return formatDay(new Date(y, m - 1, d).getTime());
}

/** Summary first, then transcript and the other well-known kinds, then the rest. */
const KIND_ORDER = ["summary", "transcript", "action-items", "highlights", "title"];
function kindRank(kind: string) {
  const i = KIND_ORDER.indexOf(kind);
  if (i >= 0) return i;
  return kind.startsWith("translation-") ? KIND_ORDER.length : KIND_ORDER.length + 1;
}
export function sortAttributes(items: Attribute[] | undefined) {
  return [...(items ?? [])].sort((a, b) => kindRank(a.kind) - kindRank(b.kind) || a.kind.localeCompare(b.kind));
}

function kindLabel(kind: string) {
  if (kind.startsWith("translation-")) return `Translation (${kind.slice("translation-".length)})`;
  return kind.charAt(0).toUpperCase() + kind.slice(1).replace(/-/g, " ");
}

// --- rendering ------------------------------------------------------------------

/** **bold** inside a line; everything else stays text (no HTML is injected). */
function inline(text: string): ReactNode[] {
  return text.split(/(\*\*[^*]+\*\*)/g).map((part, i) =>
    part.startsWith("**") && part.endsWith("**") && part.length > 4 ? <b key={i}>{part.slice(2, -2)}</b> : part,
  );
}

/** A small Markdown subset: headings, bullet and numbered lists, checkboxes, bold. */
function Markdown({ text }: { text: string }) {
  const blocks: ReactNode[] = [];
  let list: ReactNode[] = [];
  const flush = () => {
    if (list.length) blocks.push(<ul key={`l${blocks.length}`}>{list}</ul>);
    list = [];
  };
  text.split(/\r?\n/).forEach((raw, i) => {
    const line = raw.trimEnd();
    const heading = /^(#{1,6})\s+(.*)$/.exec(line);
    const check = /^\s*[-*]\s+\[( |x|X)\]\s+(.*)$/.exec(line);
    const bullet = /^\s*(?:[-*]|\d+[.)])\s+(.*)$/.exec(line);
    if (heading) {
      flush();
      blocks.push(
        heading[1].length <= 2 ? <h4 key={i}>{inline(heading[2])}</h4> : <h5 key={i}>{inline(heading[2])}</h5>,
      );
    } else if (check) {
      list.push(
        <li key={i} className="check">
          <input type="checkbox" checked={check[1] !== " "} readOnly /> {inline(check[2])}
        </li>,
      );
    } else if (bullet) {
      list.push(<li key={i}>{inline(bullet[1])}</li>);
    } else if (line.trim() === "") {
      flush();
    } else {
      flush();
      blocks.push(<p key={i}>{inline(line)}</p>);
    }
  });
  flush();
  return <div className="md">{blocks}</div>;
}

/** WebVTT / SRT as "[m:ss] text" lines. */
function Transcript({ text }: { text: string }) {
  const cues: { at: string; lines: string[] }[] = [];
  let current: { at: string; lines: string[] } | null = null;
  for (const raw of text.split(/\r?\n/)) {
    const line = raw.trim();
    const timing = /^(\d{1,2}:)?(\d{1,2}):(\d{2})[.,]\d{3}\s+-->/.exec(line);
    if (timing) {
      const hours = timing[1] ? Number(timing[1].slice(0, -1)) : 0;
      const minutes = hours * 60 + Number(timing[2]);
      current = { at: `${minutes}:${timing[3]}`, lines: [] };
      cues.push(current);
    } else if (line === "" || line === "WEBVTT" || /^\d+$/.test(line) || line.startsWith("NOTE")) {
      continue;
    } else if (current) {
      current.lines.push(line);
    }
  }
  if (!cues.length) return <pre className="attr-content">{text}</pre>;
  return (
    <div className="transcript">
      {cues.map((c, i) => (
        <div key={i} className="cue">
          <span className="cue-at">{c.at}</span>
          <span>{c.lines.join(" ")}</span>
        </div>
      ))}
    </div>
  );
}

function AttributeBody({ attr, content }: { attr: Attribute; content: string }) {
  if (attr.mime === "text/vtt" || attr.mime === "application/x-subrip") return <Transcript text={content} />;
  if (attr.mime === "text/markdown") return <Markdown text={content} />;
  if (attr.mime === "application/json") {
    try {
      return <pre className="attr-content">{JSON.stringify(JSON.parse(content), null, 2)}</pre>;
    } catch {
      /* show as text */
    }
  }
  return <pre className="attr-content">{content}</pre>;
}

/** Tabs over the attributes of one recording or day; content loads on demand. */
function AttributeTabs({
  items,
  initialKind,
  empty,
  onError,
}: {
  items: Attribute[];
  initialKind?: string;
  empty: ReactNode;
  onError: (e: string) => void;
}) {
  const [kind, setKind] = useState<string | undefined>(initialKind ?? items[0]?.kind);
  const [content, setContent] = useState<string | null>(null);
  const selected = items.find((a) => a.kind === kind) ?? items[0];

  useEffect(() => {
    if (!items.some((a) => a.kind === kind)) setKind(items[0]?.kind);
  }, [items, kind]);

  useEffect(() => {
    setContent(null);
    if (!selected) return;
    let live = true;
    api
      .readAttribute(selected.scope, selected.target, selected.kind)
      .then((c) => live && setContent(c))
      .catch((e) => onError(errorText(e)));
    return () => {
      live = false;
    };
    // Reload when an agent rewrites it (hash changes).
  }, [selected?.scope, selected?.target, selected?.kind, selected?.sha256]); // eslint-disable-line react-hooks/exhaustive-deps

  if (!selected) return <div className="attr-empty">{empty}</div>;

  async function remove(attr: Attribute) {
    if (!(await ask(`Delete the "${attr.kind}" attribute? The recording is not affected.`, { kind: "warning" }))) return;
    try {
      await api.deleteAttribute(attr.scope, attr.target, attr.kind);
    } catch (e) {
      onError(errorText(e));
    }
  }

  return (
    <div className="attr-tabs">
      <div className="tab-row">
        {items.map((a) => (
          <button key={a.kind} className={`tab ${a.kind === selected.kind ? "current" : ""}`} onClick={() => setKind(a.kind)}>
            {kindLabel(a.kind)}
          </button>
        ))}
      </div>
      <div className="attr-panel">
        <div className="attr-meta muted small">
          by {selected.author} · {new Date(selected.updatedMs).toLocaleString()} · {formatBytes(selected.size)}
          <span className="spacer" />
          <button onClick={() => navigator.clipboard.writeText(content ?? "")} disabled={content === null}>
            Copy
          </button>
          <button onClick={() => revealItemInDir(selected.path).catch((e) => onError(errorText(e)))}>Show file</button>
          <button className="danger" onClick={() => remove(selected)}>
            Delete
          </button>
        </div>
        {content === null ? <p className="muted">Loading…</p> : <AttributeBody attr={selected} content={content} />}
      </div>
    </div>
  );
}

const AGENT_HINT = (
  <span className="muted small">
    Agents connected under Settings &gt; Agent access can create these (for example with the{" "}
    <code>process_recording</code> or <code>summarize_day</code> prompt).
  </span>
);

// --- day ----------------------------------------------------------------------------

/** First lines of a recording's summary, for the day view's list. */
function SummaryPreview({ attr }: { attr: Attribute | undefined }) {
  const [text, setText] = useState("");
  useEffect(() => {
    if (!attr) return;
    let live = true;
    api
      .readAttribute(attr.scope, attr.target, attr.kind)
      .then((c) => {
        if (!live) return;
        const plain = c
          .split(/\r?\n/)
          .map((l) => l.replace(/^#+\s*|^\s*[-*]\s+(\[.\]\s+)?/g, "").replace(/\*\*/g, "").trim())
          .filter(Boolean)
          .join(" · ");
        setText(plain.length > 220 ? `${plain.slice(0, 220)}…` : plain);
      })
      .catch(() => setText(""));
    return () => {
      live = false;
    };
  }, [attr?.target, attr?.sha256]); // eslint-disable-line react-hooks/exhaustive-deps
  if (!attr) return null;
  return <div className="rec-preview">{text}</div>;
}

export function DayView({
  day,
  recordings,
  attributes,
  playing,
  onPlay,
  onOpenRecording,
  onBack,
}: {
  day: string;
  recordings: Recording[];
  attributes: Map<string, Attribute[]>;
  playing: Recording | null;
  onPlay: (r: Recording) => void;
  onOpenRecording: (r: Recording, kind?: string) => void;
  onBack: () => void;
}) {
  const [error, setError] = useState("");
  const items = recordings.filter((r) => isoDay(recordingWhen(r)) === day).sort((a, b) => recordingWhen(a) - recordingWhen(b));
  const dayAttrs = sortAttributes(attributes.get(`day:${day}`));
  const devices = [...new Set(items.map((r) => r.deviceName))];
  const recAttrs = (r: Recording) => sortAttributes(attributes.get(`recording:${r.deviceId}/${r.id}`));
  const withTranscript = items.filter((r) => recAttrs(r).some((a) => a.kind === "transcript")).length;
  const withSummary = items.filter((r) => recAttrs(r).some((a) => a.kind === "summary")).length;

  return (
    <section className="page">
      <button className="link back" onClick={onBack}>
        ← Library
      </button>
      <header className="page-head">
        <div>
          <h2>{dayLabel(day)}</h2>
          <p className="muted">
            {items.length} recording{items.length === 1 ? "" : "s"} ·{" "}
            {formatDuration(items.reduce((sum, r) => sum + r.durationMs, 0))}
            {devices.length > 0 && <> · {devices.join(", ")}</>} · {withSummary}/{items.length} summarized ·{" "}
            {withTranscript}/{items.length} transcribed
          </p>
        </div>
      </header>
      {error && (
        <div className="error" onClick={() => setError("")}>
          {error}
        </div>
      )}
      <h3>Day</h3>
      <AttributeTabs
        key={`day:${day}`}
        items={dayAttrs}
        empty={<>No day summary yet. {AGENT_HINT}</>}
        onError={setError}
      />
      <h3>Recordings</h3>
      {items.length === 0 && <p className="muted">No recordings on this day.</p>}
      {items.map((r) => {
        const attrs = recAttrs(r);
        const isPlaying = playing?.id === r.id && playing.deviceId === r.deviceId;
        return (
          <div key={`${r.deviceId}/${r.id}`} className={`rec ${isPlaying ? "active" : ""}`}>
            <button className="play" onClick={() => onPlay(r)} title="Play">
              {isPlaying ? "■" : "▶"}
            </button>
            <div className="rec-main">
              <button className="rec-title link-title" onClick={() => onOpenRecording(r)}>
                {recordingTitle(r)}
              </button>
              <div className="rec-meta">
                {r.createdMs > 0 ? formatTime(r.createdMs) : "time unknown"} · {formatDuration(r.durationMs)} ·{" "}
                {r.deviceName}
              </div>
              <SummaryPreview attr={attrs.find((a) => a.kind === "summary")} />
              {attrs.length > 0 && (
                <span className="attr-chips">
                  {attrs.map((a) => (
                    <button key={a.kind} className="attr-chip" onClick={() => onOpenRecording(r, a.kind)}>
                      {a.kind}
                    </button>
                  ))}
                </span>
              )}
            </div>
            <div className="rec-actions">
              <button onClick={() => onOpenRecording(r)}>Details</button>
            </div>
          </div>
        );
      })}
    </section>
  );
}

// --- recording ------------------------------------------------------------------------

export function RecordingView({
  recording,
  attributes,
  initialKind,
  playing,
  onPlay,
  onOpenDay,
  onBack,
}: {
  recording: Recording;
  attributes: Map<string, Attribute[]>;
  initialKind?: string;
  playing: Recording | null;
  onPlay: (r: Recording) => void;
  onOpenDay: (day: string) => void;
  onBack: () => void;
}) {
  const [error, setError] = useState("");
  const r = recording;
  const attrs = sortAttributes(attributes.get(`recording:${r.deviceId}/${r.id}`));
  const day = isoDay(recordingWhen(r));
  const isPlaying = playing?.id === r.id && playing.deviceId === r.deviceId;

  return (
    <section className="page">
      <button className="link back" onClick={onBack}>
        ← Back
      </button>
      <header className="page-head">
        <div>
          <h2>{recordingTitle(r)}</h2>
          <p className="muted">
            <button className="link" onClick={() => onOpenDay(day)}>
              {dayLabel(day)}
            </button>{" "}
            · {r.createdMs > 0 ? formatTime(r.createdMs) : "time unknown"} · {formatDuration(r.durationMs)} ·{" "}
            {formatBytes(r.size)} · {r.deviceName}
            {r.displayName && <> · on device as {r.name}</>}
          </p>
        </div>
        <div className="row">
          <button className="primary" onClick={() => onPlay(r)}>
            {isPlaying ? "■ Stop" : "▶ Play"}
          </button>
          <button onClick={() => revealItemInDir(r.path).catch((e) => setError(errorText(e)))}>Show file</button>
        </div>
      </header>
      {error && (
        <div className="error" onClick={() => setError("")}>
          {error}
        </div>
      )}
      <AttributeTabs
        key={`rec:${r.deviceId}/${r.id}`}
        items={attrs}
        initialKind={initialKind}
        empty={<>No transcript or summary yet. {AGENT_HINT}</>}
        onError={setError}
      />
    </section>
  );
}
