import { ask } from "@tauri-apps/plugin-dialog";
import { useEffect, useState } from "react";
import { api, errorText, type AgentSettings, type Guidelines } from "./api";
import { formatTime } from "./format";

const DEFAULT_PORT = 47800;

function snippets(url: string, token: string) {
  return {
    "Claude Code": `claude mcp add --transport http cardputer-sync ${url} --header "Authorization: Bearer ${token}"`,
    "Claude Desktop (claude_desktop_config.json)": JSON.stringify(
      {
        mcpServers: {
          "cardputer-sync": {
            command: "npx",
            args: ["-y", "mcp-remote", url, "--header", "Authorization:${AUTH}"],
            env: { AUTH: `Bearer ${token}` },
          },
        },
      },
      null,
      2,
    ),
    "Other clients (.mcp.json)": JSON.stringify(
      { mcpServers: { "cardputer-sync": { type: "http", url, headers: { Authorization: `Bearer ${token}` } } } },
      null,
      2,
    ),
  };
}

/** Settings > Agent access: the local MCP server, its tokens, and the user's guidelines for agents. */
export function AgentAccess() {
  const [settings, setSettings] = useState<AgentSettings | null>(null);
  const [port, setPort] = useState(String(DEFAULT_PORT));
  const [guidelines, setGuidelines] = useState<Guidelines>({ general: "", recording: "", day: "" });
  const [tokenName, setTokenName] = useState("Claude Code");
  const [canWrite, setCanWrite] = useState(true);
  const [newToken, setNewToken] = useState<{ id: number; token: string } | null>(null);
  const [error, setError] = useState("");
  const [note, setNote] = useState("");

  async function load() {
    try {
      const s = await api.agentSettings();
      setSettings(s);
      setPort(String(s.port));
      setGuidelines(s.guidelines);
    } catch (e) {
      setError(errorText(e));
    }
  }

  useEffect(() => {
    load();
  }, []);

  async function apply(enabled: boolean) {
    setError("");
    try {
      setSettings(await api.setAgentAccess(enabled, Number(port) || DEFAULT_PORT));
    } catch (e) {
      setError(errorText(e));
    }
  }

  async function createToken(e: React.FormEvent) {
    e.preventDefault();
    setError("");
    try {
      const created = await api.createAgentToken(tokenName, canWrite);
      setNewToken({ id: created.row.id, token: created.token });
      await load();
    } catch (err) {
      setError(errorText(err));
    }
  }

  async function revoke(id: number, name: string) {
    if (!(await ask(`Revoke "${name}"? Agents using it lose access immediately.`, { kind: "warning" }))) return;
    try {
      await api.revokeAgentToken(id);
      // The one-time secret panel belongs to the token just created: if that
      // one is revoked, its secret and setup snippets must go too.
      setNewToken((shown) => (shown?.id === id ? null : shown));
      await load();
    } catch (e) {
      setError(errorText(e));
    }
  }

  async function saveGuidelines(e: React.FormEvent) {
    e.preventDefault();
    setError("");
    try {
      await api.setAgentGuidelines(guidelines);
      setNote("Guidelines saved. Agents read them with get_guidelines.");
    } catch (err) {
      setError(errorText(err));
    }
  }

  if (!settings) return error ? <div className="error">{error}</div> : null;
  const url = settings.url ?? `http://127.0.0.1:${settings.port}/mcp`;

  return (
    <div className="agent-access">
      <h3>Agent access</h3>
      <p className="muted small">
        Lets AI agents on this computer (Claude Code, Claude Desktop, n8n…) read your recordings over MCP and save
        transcripts, summaries and day notes back. The app itself runs no AI. Only agents with a token below get in,
        and only while you are logged in here.
      </p>
      <div className="row">
        <label className="toggle inline">
          <input type="checkbox" checked={settings.enabled} onChange={(e) => apply(e.target.checked)} />
          <b>Agent access (MCP server)</b>
        </label>
        <label className="inline">
          Port
          <input className="port" value={port} onChange={(e) => setPort(e.target.value.replace(/\D/g, ""))} />
        </label>
        {String(settings.port) !== port && <button onClick={() => apply(settings.enabled)}>Apply port</button>}
      </div>
      <div className="muted small">
        {settings.url ? (
          <>
            Running at <code>{settings.url}</code>
          </>
        ) : settings.enabled ? (
          "Not running."
        ) : (
          "Off."
        )}
      </div>
      {settings.error && <div className="error">{settings.error}</div>}

      <h4>Tokens</h4>
      {settings.tokens.length === 0 && <p className="muted small">No tokens yet.</p>}
      {settings.tokens.map((t) => (
        <div key={t.id} className="token-row">
          <span>
            <b>{t.name}</b> <span className="tag">{t.canWrite ? "read + write" : "read only"}</span>
          </span>
          <span className="muted small">{t.lastUsedMs ? `last used ${formatTime(t.lastUsedMs)}` : "never used"}</span>
          <button className="danger" onClick={() => revoke(t.id, t.name)}>
            Revoke
          </button>
        </div>
      ))}
      <form className="row" onSubmit={createToken}>
        <input value={tokenName} onChange={(e) => setTokenName(e.target.value)} placeholder="Token name" />
        <label className="inline small">
          <input type="checkbox" checked={canWrite} onChange={(e) => setCanWrite(e.target.checked)} />
          can write attributes
        </label>
        <button className="primary">Create token</button>
      </form>
      {newToken && (
        <div className="note">
          <p>
            <b>Copy this token now;</b> it is not shown again.
          </p>
          <code className="block">{newToken.token}</code>
          {Object.entries(snippets(url, newToken.token)).map(([label, text]) => (
            <div key={label}>
              <div className="small muted">{label}</div>
              <pre className="block">{text}</pre>
              <button type="button" onClick={() => navigator.clipboard.writeText(text)}>
                Copy
              </button>
            </div>
          ))}
          <button type="button" className="link" onClick={() => setNewToken(null)}>
            Done
          </button>
        </div>
      )}

      <h4>Guidelines for agents</h4>
      <p className="muted small">
        What agents should prioritize: who you are, what matters in your recordings, language and format. Agents get
        this text from the get_guidelines tool before they work.
      </p>
      <form className="settings-form" onSubmit={saveGuidelines}>
        <label>
          General
          <textarea
            rows={3}
            value={guidelines.general}
            placeholder="e.g. I run a small bakery. Supplier orders and customer complaints matter most. Write in English; recordings may be in Hindi."
            onChange={(e) => setGuidelines({ ...guidelines, general: e.target.value })}
          />
        </label>
        <label>
          For a recording
          <textarea
            rows={3}
            value={guidelines.recording}
            placeholder="e.g. Transcript with timestamps; summary in 5 bullets max; list every amount and date mentioned."
            onChange={(e) => setGuidelines({ ...guidelines, recording: e.target.value })}
          />
        </label>
        <label>
          For a day
          <textarea
            rows={3}
            value={guidelines.day}
            placeholder="e.g. One page: decisions, open tasks with owners, follow-ups for tomorrow."
            onChange={(e) => setGuidelines({ ...guidelines, day: e.target.value })}
          />
        </label>
        <div className="row">
          <button className="primary">Save guidelines</button>
        </div>
      </form>
      {note && <div className="note">{note}</div>}
      {error && <div className="error">{error}</div>}
    </div>
  );
}
