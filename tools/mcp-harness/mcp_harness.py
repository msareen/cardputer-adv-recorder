#!/usr/bin/env python3
"""End-to-end test of the agent MCP server with a real client: Claude Code.

    python tools/mcp-harness/mcp_harness.py [--model haiku] [--keep]

What it does:
  1. Builds examples/mcp_serve_cli (into src-tauri/target-test, so it does not
     fight a running `tauri dev` for the cargo lock).
  2. Writes two short test-tone WAVs and serves them on a free port with a
     throwaway database, plus a general guideline that tells agents to put a
     marker in everything they save. The task prompt never mentions the
     marker, so finding it proves the agent read get_guidelines.
  3. Runs `claude -p` headless with only this server's tools allowed.
  4. Checks the result independently of what Claude says:
       - the MCP tools it called (from the stream-json log),
       - a summary file next to each recording copy,
       - a day summary under _days/<day>/,
       - the guideline marker in all of them.

Exit code 0 when every check passes. It calls a real model, so it uses a
little of your Claude quota; it is not part of `cargo test`.
Needs: Rust, and the `claude` CLI logged in. Works on Windows, macOS, Linux.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import time
import wave
from pathlib import Path

TAURI = Path(__file__).resolve().parents[2] / "desktop" / "src-tauri"
TARGET = TAURI / "target-test"
EXE = TARGET / "debug" / "examples" / ("mcp_serve_cli.exe" if os.name == "nt" else "mcp_serve_cli")
SERVER = "cardputer-sync"
MARKER = "HARNESS-7Q2"
GUIDELINE = (
    f"House rule: every attribute you save must begin with the line '{MARKER}'. "
    "Keep summaries to one or two sentences."
)
PROMPT = (
    "You are working on the user's voice recordings through the cardputer-sync MCP tools. "
    "Follow the user's guidelines. For every recording that has no summary yet, save a 'summary' "
    "attribute describing it (these are short test tones without speech). Then save a 'summary' "
    "attribute on the day those recordings belong to. Finally reply with one line listing what you saved."
)
EXPECTED_TOOLS = {"get_guidelines", "set_recording_attribute", "set_day_attribute"}


def fail(msg: str) -> None:
    print(f"FAIL: {msg}")
    sys.exit(1)


def write_tone(path: Path, freq: float, seconds: float = 1.5) -> None:
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(16000)
        frames = b"".join(
            struct.pack("<h", int(6000 * math.sin(2 * math.pi * freq * i / 16000))) for i in range(int(16000 * seconds))
        )
        w.writeframes(frames)


def stop_server(server: subprocess.Popen) -> None:
    """Enter stops mcp_serve_cli (and makes it delete its temporary library)."""
    try:
        server.stdin.write("\n")
        server.stdin.flush()
        server.wait(timeout=10)
    except Exception:
        server.kill()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", default="haiku", help="model for claude -p (default haiku)")
    parser.add_argument("--keep", action="store_true", help="keep the temp folders for inspection")
    args = parser.parse_args()

    claude = shutil.which("claude")
    cargo = shutil.which("cargo")
    if not claude:
        fail("the `claude` CLI is not on PATH")
    if not cargo:
        fail("cargo is not on PATH")

    print("==> building mcp_serve_cli")
    env = dict(os.environ, CARGO_TARGET_DIR=str(TARGET))
    if subprocess.run([cargo, "build", "--example", "mcp_serve_cli"], cwd=TAURI, env=env).returncode != 0:
        fail("cargo build failed")

    work = Path(tempfile.mkdtemp(prefix="cprec-harness-"))
    wavs = work / "wavs"
    wavs.mkdir()
    write_tone(wavs / "REC0001.WAV", 440)
    write_tone(wavs / "REC0002.WAV", 660)

    print("==> starting the MCP server")
    server = subprocess.Popen(
        [str(EXE), str(wavs), "0"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        env=dict(os.environ, MCP_SERVE_GUIDELINES=GUIDELINE),
    )
    lines: list[str] = []
    info: dict[str, str] = {}
    deadline = time.time() + 30
    while time.time() < deadline and "token" not in info:
        line = server.stdout.readline()
        if not line:
            break
        lines.append(line.rstrip())
        if line.startswith("MCP endpoint: "):
            info["url"] = line.split(": ", 1)[1].strip()
        elif line.startswith("Token: "):
            info["token"] = line.split(": ", 1)[1].strip()
        elif "library copy in " in line:
            info["library"] = line.split("library copy in ", 1)[1].strip()
    if "token" not in info:
        server.kill()
        fail("server did not start:\n" + "\n".join(lines))
    # Keep draining its output (attribute-change lines) so it never blocks.
    threading.Thread(target=lambda: [lines.append(l.rstrip()) for l in server.stdout], daemon=True).start()
    print(f"    {info['url']}")

    config = work / "mcp.json"
    config.write_text(
        json.dumps(
            {
                "mcpServers": {
                    SERVER: {
                        "type": "http",
                        "url": info["url"],
                        "headers": {"Authorization": "Bearer ${CPREC_HARNESS_TOKEN}"},
                    }
                }
            }
        ),
        encoding="utf-8",
    )

    print(f"==> running claude -p ({args.model})")
    started = time.time()
    result = subprocess.run(
        [
            claude, "-p", PROMPT,
            "--strict-mcp-config", "--mcp-config", str(config),
            "--allowedTools", f"mcp__{SERVER}",
            "--permission-prompts", "none",
            "--model", args.model,
            "--output-format", "stream-json", "--verbose",
        ],
        cwd=work,
        capture_output=True,
        text=True,
        encoding="utf-8",
        timeout=600,
        env=dict(os.environ, CPREC_HARNESS_TOKEN=info["token"]),
    )
    elapsed = time.time() - started

    # --- checks -----------------------------------------------------------
    tools: list[str] = []
    final = ""
    mcp_status = None
    for raw in result.stdout.splitlines():
        try:
            event = json.loads(raw)
        except json.JSONDecodeError:
            continue
        if event.get("type") == "system" and event.get("subtype") == "init":
            mcp_status = {s["name"]: s.get("status") for s in event.get("mcp_servers", [])}
        if event.get("type") == "assistant":
            for block in event.get("message", {}).get("content", []):
                if block.get("type") == "tool_use" and block.get("name", "").startswith(f"mcp__{SERVER}__"):
                    tools.append(block["name"].split("__", 2)[2])
        if event.get("type") == "result":
            final = event.get("result", "")

    library = Path(info["library"])
    checks: list[tuple[str, bool, str]] = []
    checks.append(("claude exited cleanly", result.returncode == 0, f"exit {result.returncode}"))
    checks.append(("MCP server connected", (mcp_status or {}).get(SERVER) == "connected", str(mcp_status)))
    missing = EXPECTED_TOOLS - set(tools)
    checks.append(("called the expected tools", not missing, f"called {tools}; missing {sorted(missing)}"))

    summaries = sorted(library.rglob("REC000*.summary.*")) if library.exists() else []
    checks.append(("a summary next to each recording", len(summaries) == 2, f"found {[p.name for p in summaries]}"))
    day_files = sorted((library / "_days").rglob("summary.*")) if (library / "_days").exists() else []
    checks.append(("a day summary", len(day_files) == 1, f"found {[str(p.relative_to(library)) for p in day_files]}"))
    marked = [p for p in summaries + day_files if MARKER in p.read_text(encoding="utf-8")]
    checks.append(
        ("guideline marker in every attribute (agent read get_guidelines)",
         bool(summaries + day_files) and len(marked) == len(summaries + day_files),
         f"{len(marked)}/{len(summaries + day_files)} files have it"),
    )

    print(f"\n==> results ({elapsed:.0f} s)")
    for name, ok, detail in checks:
        print(f"  [{'PASS' if ok else 'FAIL'}] {name}" + ("" if ok else f"  ({detail})"))
    print(f"\n  tool calls: {', '.join(tools) or '(none)'}")
    print(f"  claude said: {final.strip()[:300]}")
    for p in summaries + day_files:
        print(f"\n  --- {p.relative_to(library)} ---\n  " + p.read_text(encoding="utf-8").strip().replace("\n", "\n  "))

    ok = all(c[1] for c in checks)
    if not ok and result.stderr.strip():
        print("\n  claude stderr:\n  " + result.stderr.strip()[-1500:])
    if args.keep:
        # Copy the library out first: the server deletes its own copy on exit.
        kept = work / "library"
        if library.exists():
            shutil.copytree(library, kept)
        print(f"\n  kept: {work}")
    stop_server(server)
    if not args.keep:
        shutil.rmtree(work, ignore_errors=True)
    print("\nPASS" if ok else "\nFAIL")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
