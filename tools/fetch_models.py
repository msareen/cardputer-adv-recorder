#!/usr/bin/env python3
"""Fetches the speech models bundled into the desktop app.

The models are release artifacts of this repository (tag `models-v1`), not git
content. Files are verified against the pinned SHA-256 and unpacked into
desktop/src-tauri/resources/models (git-ignored), which tauri.conf.json bundles
into the installer. Run it before building or testing the desktop app;
build.py and the release workflow do. Safe to re-run: complete models are kept.
"""
import hashlib, sys, tarfile, tempfile, urllib.request
from pathlib import Path

RELEASE = "https://github.com/msareen/cardputer-adv-recorder/releases/download/models-v1"
DEST = Path(__file__).resolve().parent.parent / "desktop" / "src-tauri" / "resources" / "models"
MODELS = {
    "sherpa-onnx-whisper-tiny.en": "80dbfc2a3fe758a68f82d53a67f059fbcd6be585f59bc93f3f058f289a80e728",
}
NEEDED = ("encoder.int8.onnx", "decoder.int8.onnx", "tokens.txt")


def complete(folder: Path) -> bool:
    names = [p.name for p in folder.glob("*")] if folder.is_dir() else []
    return all(any(n.endswith(s) for n in names) for s in NEEDED)


def fetch(name: str, sha: str) -> None:
    if complete(DEST / name):
        return
    url = f"{RELEASE}/{name}.tar.gz"
    print(f"fetching {url}")
    with tempfile.TemporaryDirectory() as tmp:
        archive = Path(tmp) / "m.tar.gz"
        urllib.request.urlretrieve(url, archive)
        got = hashlib.sha256(archive.read_bytes()).hexdigest()
        if got != sha:
            sys.exit(f"{name}: checksum mismatch (got {got}, expected {sha})")
        DEST.mkdir(parents=True, exist_ok=True)
        with tarfile.open(archive) as tar:
            tar.extractall(DEST, filter="data")


if __name__ == "__main__":
    for model, digest in MODELS.items():
        fetch(model, digest)
