//! Serves the agent MCP server without the app, over a folder of WAVs and a
//! throwaway in-memory database. For trying MCP clients against it:
//!
//!   cargo run --example mcp_serve_cli -- <folder with .wav files> [port]
//!
//! Prints the endpoint and a read+write token. Attributes are written next to
//! copies of the WAVs in a temporary library, never next to the originals.
//! Port 0 picks a free port. `MCP_SERVE_GUIDELINES` sets the general
//! guidelines agents get from `get_guidelines` (used by tools/mcp-harness).

use std::{
    path::{Path, PathBuf},
    sync::{Arc, Mutex},
    time::UNIX_EPOCH,
};

use cprec_desktop_lib::{
    agent_mcp,
    agents::AgentCore,
    db::{Db, RecordingRow},
    library,
};
use sha2::{Digest, Sha256};

#[tokio::main]
async fn main() {
    let mut args = std::env::args().skip(1);
    let source = PathBuf::from(args.next().expect("usage: mcp_serve_cli <folder with .wav files> [port]"));
    let port: u16 = args.next().map(|p| p.parse().expect("port")).unwrap_or(agent_mcp::DEFAULT_PORT);

    let lib = std::env::temp_dir().join(format!("cprec-mcp-cli-{}", std::process::id()));
    let db = Db::open_in_memory().unwrap();
    let user = db.create_user("dev", "-", &lib.to_string_lossy(), 0).unwrap();
    let mut count = 0;
    for entry in std::fs::read_dir(&source).expect("read folder") {
        let path = entry.unwrap().path();
        if !path.extension().is_some_and(|e| e.eq_ignore_ascii_case("wav")) {
            continue;
        }
        let bytes = std::fs::read(&path).unwrap();
        let id = hex::encode(&Sha256::digest(&bytes)[..16]);
        let modified = std::fs::metadata(&path)
            .and_then(|m| m.modified())
            .map(|t| t.duration_since(UNIX_EPOCH).unwrap().as_millis() as i64)
            .unwrap_or(0);
        let name = path.file_name().unwrap().to_string_lossy().to_string();
        let dest = library::final_path(Path::new(&user.library_path), "Dev", &name, &id, modified, modified);
        std::fs::create_dir_all(dest.parent().unwrap()).unwrap();
        std::fs::copy(&path, &dest).unwrap();
        // 16 kHz mono 16-bit after a 44-byte header: close enough for a dev tool.
        let duration_ms = (bytes.len().saturating_sub(44) as u64) * 1000 / 32_000;
        db.insert_recording(
            user.id,
            &RecordingRow {
                id,
                device_id: "00000000000000de".into(),
                device_name: "Dev".into(),
                name,
                display_name: None,
                size: bytes.len() as u64,
                sha256: hex::encode(Sha256::digest(&bytes)),
                created_ms: modified,
                duration_ms,
                path: dest.to_string_lossy().to_string(),
                synced_at_ms: modified,
                acked: true,
                on_device: false,
            },
        )
        .unwrap();
        count += 1;
    }

    let core = Arc::new(AgentCore::new(
        Arc::new(Mutex::new(db)),
        Arc::new(Mutex::new(Some(user.clone()))),
        Arc::new(|| println!("(attribute changed)")),
    ));
    // Optional guidelines, so tests can check that agents follow them.
    if let Ok(general) = std::env::var("MCP_SERVE_GUIDELINES") {
        let g = cprec_desktop_lib::agents::Guidelines { general, ..Default::default() };
        core.set_guidelines(user.id, &g).unwrap();
    }
    let (token, _) = core.create_token(user.id, "mcp_serve_cli", true, 0).unwrap();
    let server = agent_mcp::start(core, port).await.expect("bind");
    println!("{count} recordings, library copy in {}", lib.display());
    println!("MCP endpoint: http://127.0.0.1:{}/mcp", server.port);
    println!("Token: {token}");
    println!("Press Enter to stop.");
    let _ = tokio::task::spawn_blocking(|| std::io::stdin().read_line(&mut String::new())).await;
    server.stop().await;
    let _ = std::fs::remove_dir_all(&lib);
}
