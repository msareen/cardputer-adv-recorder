# MCP harness

End-to-end test of the desktop app's agent MCP server
(`desktop/src-tauri/src/agent_mcp.rs`) with a real client, Claude Code.

```sh
python tools/mcp-harness/mcp_harness.py            # haiku, cleans up after itself
python tools/mcp-harness/mcp_harness.py --model sonnet --keep
```

What it does:

1. Builds `desktop/src-tauri/examples/mcp_serve_cli.rs` into
   `src-tauri/target-test` (no lock fight with a running `tauri dev`).
2. Writes two short test-tone WAVs and serves them on a free port with a
   throwaway database and a guideline asking agents to start every attribute
   with a marker. The task prompt never mentions the marker.
3. Runs `claude -p` headless, allowed to use only this server's tools.
4. Checks the outcome on disk and in Claude's stream-json log, not from
   what Claude says: the MCP server connected; it called `get_guidelines`,
   `set_recording_attribute` and `set_day_attribute`; there is a summary
   file next to each recording and a day summary under `_days/<day>/`; and
   every file carries the marker (proof the guidelines reached the agent).

Exit code 0 when all checks pass. It calls a real model, so it uses a little
Claude quota and is not part of `cargo test`. The protocol-level tests that
run without a model are in `agent_mcp.rs` (`cargo test agent`).

Needs Rust and the `claude` CLI, logged in. Works on Windows, macOS and Linux.
It never touches the app's real database or library.
