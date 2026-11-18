# Agent integration

Status (November 2026): the local MCP server is **implemented**
(`desktop/src-tauri/src/agent_mcp.rs`, `agents.rs`). Jobs, plugins, runners
and automatic triggers (milestones C to E below) are still a design.
Desktop app only; nothing here touches the firmware or the `/v1` protocol.

## What is implemented

- **Settings > Agent access:** on/off, port (default 47800), tokens (named,
  read-only or read+write, shown once, revocable, last-used time), copyable
  setup for Claude Code, Claude Desktop (`mcp-remote`) and `.mcp.json`, and
  **guidelines for agents** (general / per recording / per day).
- **Attributes** instead of the "artifacts" named below: text an agent saves
  on a **recording** or on a **day** (a local date across all devices). The
  Library shows them as chips on each recording and day; click to read or
  delete.
- **Tools:** `get_guidelines`, `list_days`, `get_day`, `list_recordings`
  (filters: day, range, text, `has_attribute`, `missing_attribute`),
  `get_recording`, `get_audio` (path + 10-minute URL), `get_recording_attribute`,
  `get_day_attribute`, `set_recording_attribute`, `set_day_attribute`,
  `delete_recording_attribute`, `delete_day_attribute`.
  **Prompts:** `process_recording`, `summarize_day` (they embed the guidelines).
- **Security:** loopback only, `Host` checked, any `Origin` refused (403),
  bearer token required (401), tokens hashed, valid only while their user is
  logged in, write access per token, no tool deletes recordings.
- **Tested:** unit tests in `agents.rs`, HTTP tests in `agent_mcp.rs`
  (stateless 2026-07-28 calls, legacy `initialize`, 401/403, ranged audio
  download), and Claude Code headless against `examples/mcp_serve_cli.rs`.

Typical agent flow: `get_guidelines` → `list_days` or
`list_recordings(missing_attribute="transcript")` → `get_audio` → transcribe →
`set_recording_attribute(kind="transcript")`, `summary`… → `get_day` →
`set_day_attribute(kind="summary")`.

Cloud chatbots (claude.ai, ChatGPT…) cannot reach `127.0.0.1`. That needs a
tunnel or relay plus OAuth; see "Remote agents" under open questions.

The rest of this document is the original design.

## Goal

The desktop app stays free of AI. It becomes a place that agents in other
programs can use:

1. **Agents come in.** Any MCP client (Claude Code, Claude Desktop, n8n, a
   custom agent) can list recordings, fetch audio, and save results such as a
   transcript, translation or summary back next to the recording.
2. **The app sends work out.** The user sets up plugins that trigger an
   external agent ("when a recording syncs, transcribe it"; "when a transcript
   is saved, summarize it and send it to Notion"). The agent reports back
   through the same MCP server.

Chaining the two gives pipelines in which each step uses whichever program is
best at it, while the app only stores recordings, fires events and keeps what
comes back.

## Research summary

What was checked (November 2026) and what it means for this design. Sources
are at the end.

### MCP 2026-07-28 (current spec)

- **Stateless core.** Protocol sessions (`Mcp-Session-Id`) and the
  `initialize` handshake are gone. Every request carries its version and
  capabilities in `_meta`; servers must implement `server/discover`. State
  that spans calls uses "explicit, server-minted handles passed as ordinary
  tool arguments". *Consequence:* a job id is that kind of handle. The
  agent passes `job_id` to tools, and the server checks that the job belongs
  to the caller's token, as the spec requires ("MUST NOT treat possession of
  a state handle as authentication").
- **Sampling, Roots and Logging are deprecated** (SEP-2577; the advice is to
  "integrate directly with LLM provider APIs instead of Sampling").
  *Consequence:* the app must not borrow the client's model through MCP. That
  fits the goal: agents bring their own intelligence, and the app never asks
  for completions.
- **Streamable HTTP** is one POST endpoint; a reply is either JSON or an SSE
  stream scoped to that request. Requests carry `MCP-Protocol-Version`,
  `Mcp-Method` and `Mcp-Name` headers, which the server must check against
  the body. Security rules: servers "MUST validate the `Origin` header"
  (403 if present and invalid), "SHOULD bind only to localhost", "SHOULD
  implement proper authentication". The security best practices add, for
  local HTTP servers: "Require an authorization token".
- **Tasks** are now an extension (`io.modelcontextprotocol/tasks`). A server
  can answer a long call with a task handle, and the client polls
  `tasks/get`. Claude Code supports it in public beta (v2.1.304+); the
  desktop app and the VS Code extension do not. *Consequence:* our own tools
  are fast and don't need tasks. Tasks matter only when the app itself calls
  a slow tool on another MCP server (runner `mcp`, below), and possibly for
  `run_plugin` later.
- **Precedent:** Figma's desktop app serves MCP at
  `http://127.0.0.1:3845/mcp`, only while the app is open, on a fixed port.
  Users add it with `claude mcp add --transport http figma-desktop <url>`.
  This is the model to copy.

### Rust SDK

- `rmcp` is the official Rust SDK. It implements the stable 2026-07-28 spec
  and says it stays compatible with 2025-11-25 and earlier clients.
  Feature `transport-streamable-http-server` provides a Tower service that
  mounts on an axum router (`nest_service("/mcp", service)`). It fits the
  existing tokio runtime in `src-tauri`. Pin the exact 3.x version when
  implementing.

### Clients

- **Claude Code** connects to local HTTP servers directly, with headers:
  `claude mcp add --transport http cprec http://127.0.0.1:<port>/mcp --header "Authorization: Bearer <token>"`.
  In `.mcp.json` / `--mcp-config` an entry needs `"type": "http"`, and
  `headers` expands `${VAR}`. *Consequence:* the token can be passed in an
  environment variable instead of on the command line.
- **Claude Code headless** (`claude -p`) is the way to launch an agent from
  a plugin. It takes `--mcp-config`, `--allowedTools`, `--permission-mode`,
  `--output-format json`, `--permission-prompts none` (unattended) and
  `--bare` (skip local hooks, plugins, CLAUDE.md and `.mcp.json`;
  recommended for scripts, will become the default for `-p`). Exit code 0
  means success; SIGTERM ends the turn unfinished (code 143). Note: `--bare`
  does not use the subscription login and needs `ANTHROPIC_API_KEY`.
- **Claude Desktop** cannot reach `localhost` through Custom Connectors,
  which are opened from Anthropic's cloud. Local HTTP servers are reached
  through a stdio bridge such as `npx mcp-remote <url>` in
  `claude_desktop_config.json`. *Consequence:* the app should offer a
  copyable config snippet for each client, and maybe later a built-in stdio
  bridge (see open questions).
- **n8n** has an MCP Client Tool node (Streamable HTTP, bearer and header
  auth), so an n8n workflow can call our server, and plain webhooks can
  start n8n workflows.

### A2A (Agent2Agent) vs MCP

- A2A v1.0 is for delegating to an independent agent that plans its own
  work. It defines an Agent Card at `/.well-known/agent-card.json`, a task
  lifecycle (`submitted`, `working`, `input-required`, `completed`,
  `failed`, `canceled`, `rejected`, `auth-required`), results as
  *artifacts*, streaming, and push notifications to a webhook (with
  `Authorization` credentials, idempotent receivers).
- The consensus in 2026 write-ups: use MCP for tools and data an agent
  controls, A2A for agent-to-agent delegation, and often both ("MCP inside
  each agent, A2A between them").
- *Consequence:* the app's surface is tools and data, so **MCP is the right
  thing to expose**. Sending work out is delegation, which is A2A's domain.
  But for a single-user desktop app, launching a CLI or posting a webhook
  covers today's agents, and few local agents speak A2A yet. Design the
  runner interface so an A2A runner can be added later without changing
  jobs or artifacts. The vocabulary (task/job, artifact, input-required) is
  borrowed from A2A on purpose.

### Outbound webhooks

- Standard Webhooks is the common signing scheme: `webhook-id`,
  `webhook-timestamp`, `webhook-signature` (`v1,` + base64 HMAC-SHA256 over
  `id.timestamp.body`), with a 5-minute tolerance. Receivers can verify it
  with existing libraries.

## Patterns considered

| Pattern | Verdict |
| --- | --- |
| App embeds an LLM SDK | Rejected: goal is no AI in the app; locks to one provider. |
| MCP sampling (app asks the client's model) | Rejected: deprecated in 2026-07-28; only works while a client is connected. |
| MCP server only (agents pull) | Needed, but on its own nothing happens until a person asks an agent. |
| Webhook out only (Zapier style) | Needed for hosted agents, but results come back through a custom API instead of MCP. |
| **MCP server in + runners out + jobs as handles** | **Chosen.** One way back in (MCP) whatever started the agent; the app tracks each run as a job. |
| A2A server (app as an agent) | Not applicable: the app does no reasoning. |
| A2A client runner | Later, behind the same runner interface. |
| Visual workflow engine | Rejected: chaining by events ("artifact saved, run the next plugin") covers the cases without building an editor. |

## Architecture

```
                 ┌──────────────────────── desktop app (Tauri, Rust) ───────────────────────┐
 device ─sync──► │ library + SQLite ──events──► trigger engine ──► job queue ──► runners ───┼──► claude -p / other CLI
                 │       ▲                                                        │         │──► webhook (n8n, hosted agent)
                 │       │ put_artifact / complete_job                            │         │──► tool on another MCP server
                 │  MCP server  127.0.0.1:<port>/mcp  (rmcp + axum, bearer + Origin)        │──► (later) A2A agent
                 └───────▲──────────────────────────────────────────────────────────────────┘
                         │ Streamable HTTP
       agents started by runners, plus agents the user runs (Claude Code, Claude Desktop via mcp-remote, n8n)
```

New Rust modules (all in `desktop/src-tauri/src`, no React logic beyond UI):

- `mcp_server.rs`: axum router, Origin and bearer middleware, `rmcp` tool
  handler that calls existing `Db` and `library` functions.
- `artifacts.rs`: sidecar files plus the `artifacts` table.
- `agents/` : `plugins.rs` (config), `triggers.rs` (event to job),
  `jobs.rs` (queue, limits, state), `runners/{command,webhook,mcp}.rs`.
- `tokens.rs`: token minting, hashing, scope checks.

## MCP server

- **Address:** `127.0.0.1` only, on a fixed default port (configurable;
  error shown if it is taken). Fixed so client configs stay valid. Endpoint
  `/mcp`, plus `/files/...` for audio bytes (below).
- **Lifetime:** runs while a user is logged in and "Agent access" is on (off
  by default). The app already keeps running in the tray. Tokens belong to
  one user; logging out makes them fail.
- **Every request:** reject a present, non-matching `Origin` with 403;
  require `Authorization: Bearer`; look up the token's hash; check scope per
  tool; check header and body consistency (rmcp should do this; test it).
- **Version:** 2026-07-28, plus whatever older versions rmcp accepts, since
  `mcp-remote` and some clients may lag.

### Tools

Names stay stable; results include `structuredContent`; `tools/list` order is
deterministic (the spec says SHOULD).

| Tool | Scope | Purpose |
| --- | --- | --- |
| `list_recordings` | `read` | Filter by device, date range, text, `has_artifact` / `missing_artifact` kind; paged. |
| `get_recording` | `read` | Metadata plus list of artifacts. |
| `get_audio` | `read` | `{path, mime, size, duration_ms, url}`: local file path, and a short-lived `url` for clients that can't read the disk. |
| `get_artifact` | `read` | Text content of an artifact (or its path for binary). |
| `search` | `read` | Recording names plus text artifacts (SQLite FTS5 later). |
| `put_artifact` | `write:artifacts` | `recording_id, kind, content or content_base64, mime, job_id?`. Overwrites the same kind from the same producer, never someone else's. |
| `delete_artifact` | `write:artifacts` | Only artifacts this token's producer created. |
| `get_job` | job token | The job brief: plugin instructions, recording, input artifacts. |
| `report_progress` | job token | Status line shown in the UI. |
| `complete_job` | job token | `status: done or failed, message`. Ends the job and revokes its token. |
| `list_plugins`, `run_plugin` | `run:plugins` | Let a user's agent start a configured plugin (returns `job_id`). |
| `list_devices`, `sync_now` | `device` | Optional. |

Deliberately **not exposed**: deleting recordings or files on the device,
changing settings, and anything that creates or edits plugins. An agent that
could create a command plugin could run any program, so plugins are only
ever configured in the app's UI.

Resources mirror the read tools (`cprec://recordings/{id}`,
`.../audio`, `.../artifacts/{kind}`) for clients that browse resources. A few
MCP prompts (`summarize_recording`, `transcribe_recording`) make the
features visible in clients that list prompts.

### Audio bytes

WAVs can be tens of MB, too big for base64 in a JSON-RPC result. Local agents
get `path` and read the file. Others get `url` =
`http://127.0.0.1:<port>/files/<recording>?t=<token>`, a plain HTTP GET with
a single-use or 10-minute token that supports range requests. MCP stays the
control channel; bytes go over plain HTTP.

## Artifacts

- **Files next to the WAV:** `<name>__<id8>.<kind>.<ext>`, e.g.
  `REC0001__01234567.summary.md`, `.transcript.vtt`,
  `.translation-fr.md`. Readable without the app; backed up with the
  library. Kind is sanitized like other path components.
- **Table** `artifacts`: user_id, device_id, recording_id, kind, mime, path,
  sha256, size, producer (token or plugin name), job_id, created_ms,
  updated_ms. Migration via `PRAGMA user_version` as usual.
- Well-known kinds (free text otherwise): `transcript`, `summary`,
  `translation:<lang>`, `action-items`, `title` (a suggested display name
  that the user can accept).
- Renaming a recording in the library moves its sidecars too; deleting the
  WAV through the app asks whether to delete them.
- Write is atomic: `.partial` then rename, the same rule as WAVs.

## Plugins and runners

A plugin is configured in the UI (stored in SQLite, exportable as TOML):

```toml
name     = "Transcribe and summarize"
trigger  = "recording.synced"        # manual | recording.synced | artifact.saved:<kind>
filter   = { device = "*", min_duration_s = 5 }
scopes   = ["read", "write:artifacts"]
instructions = """
Transcribe the audio, save it as kind "transcript" (VTT), then save a short
summary as kind "summary" (Markdown).
"""

[runner]
type = "command"
program = "claude"
args = ["--bare", "-p", "You are running cprec job {job_id}. Call get_job and follow it.",
        "--mcp-config", "{mcp_config}", "--allowedTools", "mcp__cprec,Read",
        "--permission-prompts", "none", "--output-format", "json"]
timeout_min = 30
```

**Job brief pattern.** The command line stays short and generic. The
`instructions` live in the app and the agent fetches them with `get_job`.
That keeps prompts out of argv, lets the user edit instructions without
touching runner config, and works the same for every runner type.

### Runner types

| Runner | How it starts the agent | How the result comes back |
| --- | --- | --- |
| `command` | Spawns the program **without a shell**, args as an array, placeholders filled per element. Writes a temporary MCP config whose header reads `Bearer ${CPREC_MCP_TOKEN}`, and puts the token in the child's environment, not in argv. | Agent calls `put_artifact` / `complete_job`. Exit code and stdout tail go to the job log. Exit 0 without `complete_job` counts as done. |
| `webhook` | POST JSON `{event, job_id, recording, mcp: {url, token, expires}}`, signed per Standard Webhooks. | The remote agent calls back over MCP if it can reach the machine (same machine or tunnel), **or** answers the POST with `{artifacts: [...]}` inline, for hosted services that can't reach localhost. |
| `mcp` | App acts as an MCP client (rmcp client feature) and calls one tool on another server, e.g. a local Whisper server, with the audio path or URL. Supports the tasks extension (poll `tasks/get`). | The tool result is stored as an artifact of the configured kind. No agent needed. |
| `a2a` (later) | `SendMessage` to an A2A agent with the recording as a file part and the MCP pointer in metadata; push notification config points at the app. | A2A artifacts map one-to-one onto our artifacts. |

### Jobs

Table `jobs`: id (random, 128-bit), user_id, plugin_id, recording_id,
trigger, parent_job_id, depth, status (`queued`, `running`, `done`,
`failed`, `canceled`, `timed_out`), started/finished, exit code, log path,
token hash.

- **Job token:** minted per job with the plugin's scopes plus job tools,
  limited to that job's recording (writes elsewhere are rejected), and
  revoked when the job ends or times out.
- **Limits:** at most 2 running jobs (configurable), queue the rest; per-job
  timeout; cancel from the UI (kill the process tree on Windows with a job
  object).
- **Events to the UI:** `job-started`, `job-progress`, `job-finished`,
  `artifact-saved`, in the same style as the existing `sync-*` events.

### Triggers and chaining

- Events: `recording.synced` (fired by `run_sync` after the ack), `manual`
  (library "Run with…"), `artifact.saved:<kind>`.
- Chain: transcribe saves `transcript` → `artifact.saved:transcript` →
  translate or summarize → `artifact.saved:summary` → "send to Notion" plugin.
- **Loop guards:** depth limit (default 3); a plugin is never triggered by
  an artifact from its own job; dedupe on (plugin, recording, input
  artifact sha256), so re-saving identical content doesn't re-run anything.
- Automatic triggers are off per plugin until the user turns them on. The
  first run of a `command` plugin shows the exact command line and asks for
  confirmation, as the MCP security guidance requires for local commands.

## Security model

| Threat | Mitigation |
| --- | --- |
| A web page reaches the local server (DNS rebinding, CSRF) | Loopback bind, `Origin` check (403), bearer token required on every request; no cookies. |
| Another local process or user | Token required; tokens stored as SHA-256 hashes; shown once on creation. (Unix sockets or named pipes are stronger, but clients don't support them.) |
| Leaked long-lived token | Per-client tokens with scopes and optional expiry; list and revoke in Settings; last-used time shown. |
| Agent misuse or prompt injection: speech in a recording, or an earlier artifact, says "delete everything" | No destructive tools exist; job tokens limited to one recording; plugins can't be created over MCP; artifact text is data, never instructions to the app. |
| Command injection through recording names | No shell; placeholders substituted per argument; names are already sanitized for paths. |
| Webhook spoofing or replay | Standard Webhooks signature and timestamp; per-plugin secret. |
| SSRF via the `mcp` / `webhook` runners | URLs are typed in by the user, not by agents; no redirects followed by default. |
| Runaway cost or loops | Depth limit, dedupe, concurrency limit, timeouts, auto-triggers off by default. |

## UI

- **Settings > Agents:** Agent access on/off, port, status. Clients: create
  a token with a name and scopes, then copy a ready snippet for Claude Code
  (`claude mcp add ...`), Claude Desktop (`mcp-remote` entry) or generic
  `.mcp.json`. Plugins: list, add, edit, enable auto trigger, "Test on a
  recording".
- **Library:** per recording an Artifacts panel (render Markdown or text,
  open file, delete) and a "Run with…" menu; a badge for running jobs.
- **Jobs:** list with status, duration, log tail, cancel and retry.

## Milestones

Each one is usable on its own and testable without hardware (the mock device
supplies recordings).

| | Scope | Done when |
| --- | --- | --- |
| A | MCP server, read-only: tokens, Origin and auth middleware, `list_recordings`, `get_recording`, `get_audio` (path and `/files` URL), settings toggle and client snippets. | Claude Code lists and reads recordings from the running app; Rust tests cover 403 on bad Origin, 401 without token, scope denial. |
| B | Artifacts: table, sidecars, `put_artifact` / `get_artifact` / `delete_artifact`, library panel, rename moves sidecars. | "Summarize my last recording and save it" in Claude Code produces a `.summary.md` that shows in the app. |
| C | Jobs and the `command` runner, manual "Run with…", job tokens, `get_job` / `complete_job`, Jobs view. | A `claude -p` plugin run from the library saves a transcript and finishes the job. |
| D | Automatic triggers and chaining, the `webhook` runner with signing and inline results, loop guards. | Sync a recording → transcript → summary → webhook to n8n, hands-free. |
| E | `mcp` runner (app as MCP client, with tasks), `run_plugin` tool; A2A runner if wanted by then. | A local Whisper MCP server produces transcripts with no LLM agent involved. |

## Open questions

1. Which agents come first: Claude Code headless, n8n, something else? This
   decides whether `command` or `webhook` gets polished first.
2. Default port number, and whether to add a stdio bridge mode to the app
   binary (`cprec-desktop --mcp-stdio`) so Claude Desktop works without
   `npx mcp-remote`.
3. Multiple local users: one server serving whichever user is logged in
   (proposed), or none while the app is at the login screen (also
   proposed: no server without a session).
4. Should `title` artifacts be applied automatically as the display name, or
   always wait for the user?
5. Remote agents (web chatbots): expose the endpoint through a tunnel
   (Cloudflare Tunnel, ngrok, Tailscale Funnel) or an outbound relay, with
   MCP OAuth instead of static tokens, or mirror attributes to a hosted MCP
   server. Not started; static tokens are only safe on loopback.

## Sources

- MCP 2026-07-28 changelog: https://modelcontextprotocol.io/specification/2026-07-28/changelog
- MCP Streamable HTTP transport: https://modelcontextprotocol.io/specification/2026-07-28/basic/transports/streamable-http
- MCP security best practices: https://modelcontextprotocol.io/specification/2026-07-28/basic/security_best_practices
- MCP 2026-07-28 release post: https://blog.modelcontextprotocol.io/posts/2026-07-28/
- MCP Tasks extension: https://modelcontextprotocol.io/extensions/tasks/overview
- Rust SDK (`rmcp`): https://github.com/modelcontextprotocol/rust-sdk
- Claude Code headless (`claude -p`): https://code.claude.com/docs/en/headless
- Claude Code MCP config (HTTP, headers, tasks): https://code.claude.com/docs/en/mcp
- Figma desktop MCP server: https://developers.figma.com/docs/figma-mcp-server/local-server-installation
- Claude Desktop and localhost servers (mcp-remote): https://startdebugging.net/2026/05/fix-http-mcp-server-url-wont-connect-in-claude-desktop/
- A2A specification: https://a2a-protocol.org/latest/specification/
- A2A vs MCP: https://www.agno.com/blog/a2a-vs-mcp , https://dev.to/jeff_pdc/a2a-vs-mcp-in-2026-when-agents-should-talk-to-agents-instead-of-calling-tools-4b9g
- n8n MCP nodes: https://nordflux.de/en/guides/mcp-in-n8n-server-trigger-client
- Standard Webhooks: https://webhookrelay.com/glossary/standard-webhooks/
