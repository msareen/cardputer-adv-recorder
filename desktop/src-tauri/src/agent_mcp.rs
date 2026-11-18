//! The local MCP server agents connect to (Streamable HTTP on
//! `127.0.0.1:<port>/mcp`). Every request needs `Authorization: Bearer
//! <token>` from Settings > Agent access; requests with any `Origin` header
//! (browsers) and non-loopback `Host` headers are refused, per the MCP
//! transport security rules. Audio downloads go over plain HTTP at
//! `/files/<ticket>` with a short-lived ticket from `get_audio`.
//!
//! Tools are thin: the logic lives in `agents.rs`.

use std::{net::SocketAddr, sync::Arc};

use axum::{
    Router,
    body::Body,
    extract::{Path as UrlPath, Request, State},
    http::{StatusCode, header},
    middleware::{self, Next},
    response::{IntoResponse, Response},
    routing::get,
};
use rmcp::{
    ErrorData, Json, ServerHandler,
    handler::server::{
        router::{prompt::PromptRouter, tool::ToolRouter},
        tool::Extension,
        wrapper::Parameters,
    },
    model::{Implementation, PromptMessage, Role, ServerCapabilities, ServerConfig},
    prompt, prompt_handler, prompt_router, tool, tool_handler, tool_router,
    transport::streamable_http_server::{
        StreamableHttpServerConfig, StreamableHttpService, session::local::LocalSessionManager,
    },
};
use schemars::JsonSchema;
use serde::{Deserialize, Serialize};
use tokio_util::sync::CancellationToken;
use tower::ServiceExt;

use crate::{
    agents::{
        AgentCore, AgentError, AttributeInfo, DayDetail, DaySummary, Principal, RecordingDetail, RecordingFilter,
        RecordingSummary, SCOPE_DAY, SCOPE_RECORDING, TICKET_TTL, guidelines_text, recording_ref,
    },
    sync::now_ms,
};

pub const DEFAULT_PORT: u16 = 47800;

const INSTRUCTIONS: &str = "Cardputer Sync: voice recordings from the user's Cardputer recorder, grouped by day. \
This server stores no AI: you download audio, do the work (transcribe, translate, summarize...) and save results \
back as attributes on a recording or on a day. Call get_guidelines first: it holds the user's priorities and the \
naming conventions for attributes.";

// --- tool parameters and results ---------------------------------------------

#[derive(Debug, Deserialize, JsonSchema)]
pub struct RecordingArg {
    /// The `recording` value from list_recordings or get_day.
    pub recording: String,
}

#[derive(Debug, Deserialize, JsonSchema)]
pub struct DayArg {
    /// Local date, YYYY-MM-DD.
    pub day: String,
}

#[derive(Debug, Deserialize, JsonSchema)]
pub struct ListDaysArgs {
    /// From this day, inclusive (YYYY-MM-DD).
    pub from: Option<String>,
    /// Up to this day, inclusive (YYYY-MM-DD).
    pub to: Option<String>,
    /// At most this many days, newest first (default 31).
    pub limit: Option<usize>,
}

#[derive(Debug, Deserialize, JsonSchema)]
pub struct RecordingKindArgs {
    pub recording: String,
    /// Attribute kind, e.g. "transcript" or "summary".
    pub kind: String,
}

#[derive(Debug, Deserialize, JsonSchema)]
pub struct DayKindArgs {
    /// Local date, YYYY-MM-DD.
    pub day: String,
    /// Attribute kind, e.g. "summary".
    pub kind: String,
}

#[derive(Debug, Deserialize, JsonSchema)]
pub struct SetRecordingAttributeArgs {
    pub recording: String,
    /// Lowercase letters, digits and dashes, e.g. "transcript", "summary", "translation-en".
    pub kind: String,
    /// The full text. Writing the same kind again replaces it.
    pub content: String,
    /// text/markdown (default), text/plain, text/vtt, application/x-subrip, application/json or text/csv.
    pub mime: Option<String>,
}

#[derive(Debug, Deserialize, JsonSchema)]
pub struct SetDayAttributeArgs {
    /// Local date, YYYY-MM-DD.
    pub day: String,
    /// Lowercase letters, digits and dashes, e.g. "summary", "action-items".
    pub kind: String,
    /// The full text. Writing the same kind again replaces it.
    pub content: String,
    /// text/markdown (default), text/plain, text/vtt, application/x-subrip, application/json or text/csv.
    pub mime: Option<String>,
}

#[derive(Debug, Serialize, JsonSchema)]
pub struct GuidelinesResult {
    pub guidelines: String,
}

#[derive(Debug, Serialize, JsonSchema)]
pub struct DaysResult {
    pub days: Vec<DaySummary>,
}

#[derive(Debug, Serialize, JsonSchema)]
pub struct RecordingsResult {
    pub recordings: Vec<RecordingSummary>,
    /// How many match in total (for paging with offset).
    pub total: usize,
}

#[derive(Debug, Serialize, JsonSchema)]
pub struct AudioResult {
    /// Local file path; read it directly if you run on this computer.
    pub path: String,
    /// HTTP download of the same WAV, no auth header needed, valid until `expires_at`.
    pub url: String,
    pub expires_at: String,
    pub mime: String,
    pub size_bytes: u64,
    pub duration_ms: u64,
}

#[derive(Debug, Serialize, JsonSchema)]
pub struct AttributeContent {
    pub kind: String,
    pub mime: String,
    pub author: String,
    pub updated_at: String,
    pub content: String,
}

#[derive(Debug, Serialize, JsonSchema)]
pub struct Deleted {
    pub deleted: bool,
}

#[derive(Debug, Deserialize, JsonSchema)]
pub struct ProcessRecordingArgs {
    /// The `recording` value from list_recordings.
    pub recording: String,
}

#[derive(Debug, Deserialize, JsonSchema)]
pub struct SummarizeDayArgs {
    /// Local date, YYYY-MM-DD.
    pub day: String,
}

type ToolResult<T> = Result<Json<T>, String>;

fn principal(parts: &axum::http::request::Parts) -> Result<Principal, String> {
    parts
        .extensions
        .get::<Principal>()
        .cloned()
        .ok_or_else(|| "Not authenticated.".to_string())
}

fn fail(e: AgentError) -> String {
    e.to_string()
}

fn attribute_content(row: crate::db::AttributeRow, content: String) -> AttributeContent {
    AttributeContent {
        kind: row.kind,
        mime: row.mime,
        author: row.author,
        updated_at: crate::agents::iso_time(row.updated_ms),
        content,
    }
}

// --- the MCP service -----------------------------------------------------------

#[derive(Clone)]
pub struct RecorderMcp {
    core: Arc<AgentCore>,
    port: u16,
    tool_router: ToolRouter<Self>,
    prompt_router: PromptRouter<Self>,
}

#[tool_router]
impl RecorderMcp {
    pub fn new(core: Arc<AgentCore>, port: u16) -> Self {
        Self { core, port, tool_router: Self::tool_router(), prompt_router: Self::prompt_router() }
    }

    #[tool(
        description = "Call this first. The user's priorities for handling recordings and days (who they are, \
                       what matters, language), plus the conventions for attribute kinds and formats.",
        annotations(read_only_hint = true)
    )]
    async fn get_guidelines(&self, Extension(parts): Extension<axum::http::request::Parts>) -> ToolResult<GuidelinesResult> {
        let p = principal(&parts)?;
        let g = self.core.guidelines(p.user.id).map_err(fail)?;
        Ok(Json(GuidelinesResult { guidelines: guidelines_text(&g) }))
    }

    #[tool(
        description = "Days that have recordings, newest first, with how many recordings, total length, the day's \
                       own attributes and how many recordings already have each attribute kind.",
        annotations(read_only_hint = true)
    )]
    async fn list_days(
        &self,
        Extension(parts): Extension<axum::http::request::Parts>,
        Parameters(args): Parameters<ListDaysArgs>,
    ) -> ToolResult<DaysResult> {
        let p = principal(&parts)?;
        let days = self.core.list_days(p.user.id, args.from.as_deref(), args.to.as_deref(), args.limit).map_err(fail)?;
        Ok(Json(DaysResult { days }))
    }

    #[tool(
        description = "One day in detail: its recordings in time order (with their attribute kinds) and the day's \
                       own attributes.",
        annotations(read_only_hint = true)
    )]
    async fn get_day(
        &self,
        Extension(parts): Extension<axum::http::request::Parts>,
        Parameters(args): Parameters<DayArg>,
    ) -> ToolResult<DayDetail> {
        let p = principal(&parts)?;
        Ok(Json(self.core.day_detail(p.user.id, &args.day).map_err(fail)?))
    }

    #[tool(
        description = "Search recordings, newest first. Filter by day or date range, text, and by attribute kind \
                       present or missing (missing_attribute=\"transcript\" lists work left to do).",
        annotations(read_only_hint = true)
    )]
    async fn list_recordings(
        &self,
        Extension(parts): Extension<axum::http::request::Parts>,
        Parameters(filter): Parameters<RecordingFilter>,
    ) -> ToolResult<RecordingsResult> {
        let p = principal(&parts)?;
        let (recordings, total) = self.core.list_recordings(p.user.id, &filter).map_err(fail)?;
        Ok(Json(RecordingsResult { recordings, total }))
    }

    #[tool(description = "One recording in detail, with its attributes (kind, format, author, time).", annotations(read_only_hint = true))]
    async fn get_recording(
        &self,
        Extension(parts): Extension<axum::http::request::Parts>,
        Parameters(args): Parameters<RecordingArg>,
    ) -> ToolResult<RecordingDetail> {
        let p = principal(&parts)?;
        Ok(Json(self.core.recording_detail(p.user.id, &args.recording).map_err(fail)?))
    }

    #[tool(
        description = "Where to get a recording's audio (16-bit PCM WAV): a local file path, and an HTTP URL valid \
                       for 10 minutes for agents that cannot read this computer's disk.",
        annotations(read_only_hint = true)
    )]
    async fn get_audio(
        &self,
        Extension(parts): Extension<axum::http::request::Parts>,
        Parameters(args): Parameters<RecordingArg>,
    ) -> ToolResult<AudioResult> {
        let p = principal(&parts)?;
        let r = self.core.find_recording(p.user.id, &args.recording).map_err(fail)?;
        let ticket = self.core.audio_ticket(p.user.id, &r).map_err(fail)?;
        Ok(Json(AudioResult {
            path: r.path.clone(),
            url: format!("http://127.0.0.1:{}/files/{ticket}", self.port),
            expires_at: crate::agents::iso_time(now_ms() + TICKET_TTL.as_millis() as i64),
            mime: "audio/wav".into(),
            size_bytes: r.size,
            duration_ms: r.duration_ms,
        }))
    }

    #[tool(description = "Read one attribute of a recording, e.g. its transcript.", annotations(read_only_hint = true))]
    async fn get_recording_attribute(
        &self,
        Extension(parts): Extension<axum::http::request::Parts>,
        Parameters(args): Parameters<RecordingKindArgs>,
    ) -> ToolResult<AttributeContent> {
        let p = principal(&parts)?;
        let r = self.core.find_recording(p.user.id, &args.recording).map_err(fail)?;
        let (row, content) =
            self.core.read_attribute(p.user.id, SCOPE_RECORDING, &recording_ref(&r), &args.kind).map_err(fail)?;
        Ok(Json(attribute_content(row, content)))
    }

    #[tool(description = "Read one attribute of a day, e.g. its summary.", annotations(read_only_hint = true))]
    async fn get_day_attribute(
        &self,
        Extension(parts): Extension<axum::http::request::Parts>,
        Parameters(args): Parameters<DayKindArgs>,
    ) -> ToolResult<AttributeContent> {
        let p = principal(&parts)?;
        let day = crate::agents::validate_day(&args.day).map_err(fail)?;
        let (row, content) = self.core.read_attribute(p.user.id, SCOPE_DAY, &day, &args.kind).map_err(fail)?;
        Ok(Json(attribute_content(row, content)))
    }

    #[tool(
        description = "Save text on a recording: a transcript, summary, translation, title suggestion... Replaces \
                       an existing attribute of the same kind. Needs a token with write access.",
        annotations(read_only_hint = false, destructive_hint = false, idempotent_hint = true)
    )]
    async fn set_recording_attribute(
        &self,
        Extension(parts): Extension<axum::http::request::Parts>,
        Parameters(args): Parameters<SetRecordingAttributeArgs>,
    ) -> ToolResult<AttributeInfo> {
        let p = principal(&parts)?;
        let target = self.core.target_recording(p.user.id, &args.recording).map_err(fail)?;
        let info = self
            .core
            .set_attribute(&p, &target, &args.kind, args.mime.as_deref(), &args.content, now_ms())
            .map_err(fail)?;
        Ok(Json(info))
    }

    #[tool(
        description = "Save text on a day: a day summary, action items across the day's recordings... Replaces an \
                       existing attribute of the same kind. Needs a token with write access.",
        annotations(read_only_hint = false, destructive_hint = false, idempotent_hint = true)
    )]
    async fn set_day_attribute(
        &self,
        Extension(parts): Extension<axum::http::request::Parts>,
        Parameters(args): Parameters<SetDayAttributeArgs>,
    ) -> ToolResult<AttributeInfo> {
        let p = principal(&parts)?;
        let target = AgentCore::target_day(&args.day).map_err(fail)?;
        let info = self
            .core
            .set_attribute(&p, &target, &args.kind, args.mime.as_deref(), &args.content, now_ms())
            .map_err(fail)?;
        Ok(Json(info))
    }

    #[tool(
        description = "Remove one attribute from a recording. The recording itself is never touched.",
        annotations(read_only_hint = false, destructive_hint = true, idempotent_hint = true)
    )]
    async fn delete_recording_attribute(
        &self,
        Extension(parts): Extension<axum::http::request::Parts>,
        Parameters(args): Parameters<RecordingKindArgs>,
    ) -> ToolResult<Deleted> {
        let p = principal(&parts)?;
        let r = self.core.find_recording(p.user.id, &args.recording).map_err(fail)?;
        self.core
            .delete_attribute(Some(&p), p.user.id, SCOPE_RECORDING, &recording_ref(&r), &args.kind)
            .map_err(fail)?;
        Ok(Json(Deleted { deleted: true }))
    }

    #[tool(
        description = "Remove one attribute from a day.",
        annotations(read_only_hint = false, destructive_hint = true, idempotent_hint = true)
    )]
    async fn delete_day_attribute(
        &self,
        Extension(parts): Extension<axum::http::request::Parts>,
        Parameters(args): Parameters<DayKindArgs>,
    ) -> ToolResult<Deleted> {
        let p = principal(&parts)?;
        let day = crate::agents::validate_day(&args.day).map_err(fail)?;
        self.core.delete_attribute(Some(&p), p.user.id, SCOPE_DAY, &day, &args.kind).map_err(fail)?;
        Ok(Json(Deleted { deleted: true }))
    }
}

#[prompt_router]
impl RecorderMcp {
    /// Transcribe and summarize one recording, following the user's guidelines.
    #[prompt(name = "process_recording", description = "Transcribe, summarize and file one recording per the user's guidelines.")]
    async fn process_recording(
        &self,
        Extension(parts): Extension<axum::http::request::Parts>,
        Parameters(args): Parameters<ProcessRecordingArgs>,
    ) -> Result<Vec<PromptMessage>, ErrorData> {
        let p = principal(&parts).map_err(|e| ErrorData::invalid_request(e, None))?;
        let g = self.core.guidelines(p.user.id).map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
        Ok(vec![PromptMessage::new_text(
            Role::User,
            format!(
                "{}\n\n# Task\nWork on recording `{}` in Cardputer Sync:\n\
                 1. get_recording to see what it already has.\n\
                 2. If there is no `transcript`, get_audio, transcribe it, and save it with set_recording_attribute.\n\
                 3. Save a `summary` (and `action-items` if any were said) following the guidelines above.\n\
                 4. Do anything else the guidelines ask for, then tell me briefly what you saved.",
                guidelines_text(&g),
                args.recording
            ),
        )])
    }

    /// Summarize one day across its recordings.
    #[prompt(name = "summarize_day", description = "Summarize a day across all its recordings per the user's guidelines.")]
    async fn summarize_day(
        &self,
        Extension(parts): Extension<axum::http::request::Parts>,
        Parameters(args): Parameters<SummarizeDayArgs>,
    ) -> Result<Vec<PromptMessage>, ErrorData> {
        let p = principal(&parts).map_err(|e| ErrorData::invalid_request(e, None))?;
        let g = self.core.guidelines(p.user.id).map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
        Ok(vec![PromptMessage::new_text(
            Role::User,
            format!(
                "{}\n\n# Task\nSummarize {} in Cardputer Sync:\n\
                 1. get_day to list its recordings and what they already have.\n\
                 2. For recordings without a transcript or summary, create them first (get_audio, then \
                    set_recording_attribute).\n\
                 3. Read the recordings' summaries/transcripts and save a day `summary` with set_day_attribute, \
                    plus `action-items` for the whole day if there are any.\n\
                 4. Tell me briefly what you saved.",
                guidelines_text(&g),
                args.day
            ),
        )])
    }
}

#[tool_handler(router = self.tool_router)]
#[prompt_handler(router = self.prompt_router)]
impl ServerHandler for RecorderMcp {
    fn get_info(&self) -> ServerConfig {
        ServerConfig::new(ServerCapabilities::builder().enable_tools().enable_prompts().build())
            .with_server_info(Implementation::new("cardputer-sync", env!("CARGO_PKG_VERSION")).with_title("Cardputer Sync"))
            .with_instructions(INSTRUCTIONS)
    }
}

// --- HTTP ------------------------------------------------------------------------

fn bearer(req: &Request) -> Option<String> {
    let value = req.headers().get(header::AUTHORIZATION)?.to_str().ok()?;
    let (scheme, token) = value.split_once(' ')?;
    scheme.eq_ignore_ascii_case("bearer").then(|| token.trim().to_string())
}

/// Resolves the token and hands the principal to the tools through the
/// request extensions (rmcp passes the HTTP parts to every handler).
async fn authenticate(State(core): State<Arc<AgentCore>>, mut req: Request, next: Next) -> Response {
    match core.authenticate(bearer(&req).as_deref(), now_ms()) {
        Ok(principal) => {
            req.extensions_mut().insert(principal);
            next.run(req).await
        }
        Err(e) => (
            StatusCode::UNAUTHORIZED,
            [(header::WWW_AUTHENTICATE, "Bearer realm=\"cardputer-sync\"")],
            e.message(),
        )
            .into_response(),
    }
}

async fn download(State(core): State<Arc<AgentCore>>, UrlPath(ticket): UrlPath<String>, req: Request) -> Response {
    let Some(path) = core.resolve_ticket(&ticket) else {
        return (StatusCode::NOT_FOUND, "Unknown or expired link. Call get_audio again.").into_response();
    };
    match tower_http::services::ServeFile::new_with_mime(&path, &"audio/wav".parse().unwrap()).oneshot(req).await {
        Ok(resp) => resp.map(Body::new),
        Err(e) => (StatusCode::INTERNAL_SERVER_ERROR, e.to_string()).into_response(),
    }
}

pub fn router(core: Arc<AgentCore>, port: u16, cancel: CancellationToken) -> Router {
    let mcp_core = core.clone();
    let config = StreamableHttpServerConfig::default()
        .with_json_response(true)
        .enforce_origin_validation()
        .with_cancellation_token(cancel);
    let mcp = StreamableHttpService::new(
        move || Ok(RecorderMcp::new(mcp_core.clone(), port)),
        Arc::new(LocalSessionManager::default()),
        config,
    );
    Router::new()
        .nest_service("/mcp", mcp)
        .layer(middleware::from_fn_with_state(core.clone(), authenticate))
        .route("/files/{ticket}", get(download))
        .with_state(core)
}

/// The server while it runs; dropping it does not stop it, call `stop`.
pub struct RunningServer {
    pub port: u16,
    cancel: CancellationToken,
    task: tokio::task::JoinHandle<()>,
}

impl RunningServer {
    pub async fn stop(self) {
        self.cancel.cancel();
        let _ = self.task.await;
    }
}

/// Binds `127.0.0.1:port` (0 picks a free port, for tests) and serves until
/// stopped.
pub async fn start(core: Arc<AgentCore>, port: u16) -> std::io::Result<RunningServer> {
    let listener = tokio::net::TcpListener::bind(SocketAddr::from(([127, 0, 0, 1], port))).await?;
    let port = listener.local_addr()?.port();
    let cancel = CancellationToken::new();
    let app = router(core, port, cancel.child_token());
    let shutdown = cancel.clone();
    let task = tokio::spawn(async move {
        let _ = axum::serve(listener, app).with_graceful_shutdown(async move { shutdown.cancelled().await }).await;
    });
    Ok(RunningServer { port, cancel, task })
}

#[cfg(test)]
mod tests {
    use std::{
        path::Path,
        sync::{Arc, Mutex},
    };

    use serde_json::{Value, json};

    use super::*;
    use crate::{
        db::{Db, RecordingRow, User},
        library,
    };

    struct Fixture {
        _dir: tempfile::TempDir,
        server: RunningServer,
        core: Arc<AgentCore>,
        user: User,
        writer: String,
        reader: String,
        recording: String,
        http: reqwest::Client,
    }

    impl Fixture {
        fn url(&self) -> String {
            format!("http://127.0.0.1:{}/mcp", self.server.port)
        }

        /// A 2026-07-28 (stateless) tools/call.
        async fn call(&self, token: &str, tool: &str, args: Value) -> (u16, Value) {
            let body = json!({
                "jsonrpc": "2.0", "id": 1, "method": "tools/call",
                "params": {
                    "name": tool, "arguments": args,
                    "_meta": {
                        "io.modelcontextprotocol/protocolVersion": "2026-07-28",
                        "io.modelcontextprotocol/clientCapabilities": {},
                        "io.modelcontextprotocol/clientInfo": {"name": "test", "version": "1"}
                    }
                }
            });
            let resp = self
                .http
                .post(self.url())
                .bearer_auth(token)
                .header("Accept", "application/json, text/event-stream")
                .header("MCP-Protocol-Version", "2026-07-28")
                .header("Mcp-Method", "tools/call")
                .header("Mcp-Name", tool)
                .json(&body)
                .send()
                .await
                .unwrap();
            let status = resp.status().as_u16();
            let text = resp.text().await.unwrap();
            (status, serde_json::from_str(&text).unwrap_or(Value::String(text)))
        }
    }

    async fn fixture() -> Fixture {
        let dir = tempfile::tempdir().unwrap();
        let db = Db::open_in_memory().unwrap();
        let user = db.create_user("ann", "h", &dir.path().join("lib").to_string_lossy(), 0).unwrap();
        let created = 1_700_000_000_000;
        let id = "00112233445566778899aabbccddeeff";
        let path = library::final_path(Path::new(&user.library_path), "Desk", "REC0001.WAV", id, created, created);
        std::fs::create_dir_all(path.parent().unwrap()).unwrap();
        std::fs::write(&path, b"RIFF0123WAVEdata").unwrap();
        db.insert_recording(
            user.id,
            &RecordingRow {
                id: id.into(),
                device_id: "0123456789abcdef".into(),
                device_name: "Desk".into(),
                name: "REC0001.WAV".into(),
                display_name: None,
                size: 16,
                sha256: "x".into(),
                created_ms: created,
                duration_ms: 1000,
                path: path.to_string_lossy().to_string(),
                synced_at_ms: created,
                acked: true,
                on_device: true,
            },
        )
        .unwrap();
        let core = Arc::new(AgentCore::new(
            Arc::new(Mutex::new(db)),
            Arc::new(Mutex::new(Some(user.clone()))),
            Arc::new(|| {}),
        ));
        let (writer, _) = core.create_token(user.id, "Writer", true, 0).unwrap();
        let (reader, _) = core.create_token(user.id, "Reader", false, 0).unwrap();
        let server = start(core.clone(), 0).await.unwrap();
        Fixture {
            _dir: dir,
            server,
            core,
            user,
            writer,
            reader,
            recording: format!("0123456789abcdef/{id}"),
            http: reqwest::Client::new(),
        }
    }

    fn structured(v: &Value) -> &Value {
        &v["result"]["structuredContent"]
    }

    #[tokio::test]
    async fn refuses_missing_tokens_and_browser_origins() {
        let f = fixture().await;
        let resp = f.http.post(f.url()).json(&json!({})).send().await.unwrap();
        assert_eq!(resp.status().as_u16(), 401);
        let resp = f.http.post(f.url()).bearer_auth("cprec_wrong").json(&json!({})).send().await.unwrap();
        assert_eq!(resp.status().as_u16(), 401);
        let resp = f
            .http
            .post(f.url())
            .bearer_auth(&f.writer)
            .header("Origin", "https://evil.example")
            .header("Accept", "application/json, text/event-stream")
            .json(&json!({"jsonrpc": "2.0", "id": 1, "method": "tools/list", "params": {}}))
            .send()
            .await
            .unwrap();
        assert_eq!(resp.status().as_u16(), 403);
        // Logged out: the app is locked.
        *f.core.session.lock().unwrap() = None;
        let (status, _) = f.call(&f.writer, "get_guidelines", json!({})).await;
        assert_eq!(status, 401);
        f.server.stop().await;
    }

    #[tokio::test]
    async fn agents_read_recordings_and_write_attributes() {
        let f = fixture().await;
        let (status, v) = f.call(&f.reader, "get_guidelines", json!({})).await;
        assert_eq!(status, 200, "{v}");
        assert!(structured(&v)["guidelines"].as_str().unwrap().contains("Suggested kinds"));

        let (_, v) = f.call(&f.reader, "list_recordings", json!({"missing_attribute": "transcript"})).await;
        assert_eq!(structured(&v)["total"], 1, "{v}");
        assert_eq!(structured(&v)["recordings"][0]["recording"], f.recording);
        let day = structured(&v)["recordings"][0]["day"].as_str().unwrap().to_string();

        // A read-only token gets a tool error, not a write.
        let (_, v) = f
            .call(&f.reader, "set_recording_attribute", json!({"recording": f.recording, "kind": "transcript", "content": "hi"}))
            .await;
        assert_eq!(v["result"]["isError"], true, "{v}");

        let (_, v) = f
            .call(&f.writer, "set_recording_attribute", json!({"recording": f.recording, "kind": "transcript", "content": "hello there"}))
            .await;
        assert_eq!(structured(&v)["author"], "Writer", "{v}");
        let (_, v) = f.call(&f.writer, "set_day_attribute", json!({"day": day, "kind": "summary", "content": "# Day"})).await;
        assert_eq!(structured(&v)["kind"], "summary", "{v}");

        let (_, v) = f.call(&f.reader, "get_day", json!({"day": day})).await;
        assert_eq!(structured(&v)["attributes"], json!(["summary"]), "{v}");
        assert_eq!(structured(&v)["recordings"][0]["attributes"], json!(["transcript"]));
        let (_, v) = f.call(&f.reader, "get_recording_attribute", json!({"recording": f.recording, "kind": "transcript"})).await;
        assert_eq!(structured(&v)["content"], "hello there", "{v}");

        let (_, v) = f.call(&f.reader, "list_days", json!({})).await;
        assert_eq!(structured(&v)["days"][0]["recordings_with"]["transcript"], 1, "{v}");

        let (_, v) = f.call(&f.writer, "delete_day_attribute", json!({"day": day, "kind": "summary"})).await;
        assert_eq!(structured(&v)["deleted"], true, "{v}");
        assert_eq!(f.core.db.lock().unwrap().list_attributes(f.user.id).unwrap().len(), 1);
        f.server.stop().await;
    }

    #[tokio::test]
    async fn audio_downloads_through_a_ticket() {
        let f = fixture().await;
        let (_, v) = f.call(&f.reader, "get_audio", json!({"recording": f.recording})).await;
        let url = structured(&v)["url"].as_str().unwrap().to_string();
        let resp = f.http.get(&url).send().await.unwrap();
        assert_eq!(resp.status().as_u16(), 200);
        assert_eq!(resp.headers()["content-type"], "audio/wav");
        assert_eq!(resp.bytes().await.unwrap().as_ref(), b"RIFF0123WAVEdata");
        let resp = f.http.get(&url).header("Range", "bytes=0-3").send().await.unwrap();
        assert_eq!(resp.status().as_u16(), 206);
        assert_eq!(resp.bytes().await.unwrap().as_ref(), b"RIFF");
        let bad = format!("http://127.0.0.1:{}/files/nope", f.server.port);
        assert_eq!(f.http.get(&bad).send().await.unwrap().status().as_u16(), 404);
        f.server.stop().await;
    }

    #[tokio::test]
    async fn older_clients_can_initialize_a_session() {
        let f = fixture().await;
        let resp = f
            .http
            .post(f.url())
            .bearer_auth(&f.reader)
            .header("Accept", "application/json, text/event-stream")
            .json(&json!({
                "jsonrpc": "2.0", "id": 1, "method": "initialize",
                "params": {"protocolVersion": "2025-11-25", "capabilities": {}, "clientInfo": {"name": "old", "version": "1"}}
            }))
            .send()
            .await
            .unwrap();
        assert_eq!(resp.status().as_u16(), 200);
        let session = resp.headers().get("mcp-session-id").map(|v| v.to_str().unwrap().to_string());
        let body = resp.text().await.unwrap();
        assert!(body.contains("cardputer-sync"), "{body}");
        assert!(session.is_some(), "legacy clients get a session id");
        f.server.stop().await;
    }
}
