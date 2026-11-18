#!/usr/bin/env python3
"""Single version source: the VERSION file in the repo root.

  python tools/set_version.py            copy VERSION into the desktop app and mock device
  python tools/set_version.py 1.6.0      write VERSION, then copy it
  python tools/set_version.py --check [vTAG]   fail if anything disagrees (or the tag differs)

Targets: desktop/package.json, desktop/package-lock.json, desktop/src-tauri/tauri.conf.json,
desktop/src-tauri/Cargo.toml, tools/mock-device/Cargo.toml. Run `cargo check` after a
bump so the Cargo.lock files follow. The firmware takes its version from the git tag.
"""
import json, re, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VERSION_FILE = ROOT / "VERSION"
SEMVER = re.compile(r"^\d+\.\d+\.\d+$")


def read_json(p):
    return json.loads(p.read_text(encoding="utf-8"))


def write_json(p, d):
    p.write_text(json.dumps(d, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def targets(version, write):
    """Yields (name, current) and rewrites to `version` when write is set."""
    for rel in ("desktop/package.json", "desktop/src-tauri/tauri.conf.json"):
        p = ROOT / rel
        d = read_json(p)
        yield rel, d["version"]
        if write and d["version"] != version:
            d["version"] = version
            write_json(p, d)
    p = ROOT / "desktop/package-lock.json"
    if p.exists():
        d = read_json(p)
        yield "desktop/package-lock.json", d["version"]
        if write and (d["version"] != version or d["packages"][""]["version"] != version):
            d["version"] = d["packages"][""]["version"] = version
            write_json(p, d)
    for rel in ("desktop/src-tauri/Cargo.toml", "tools/mock-device/Cargo.toml"):
        p = ROOT / rel
        text = p.read_text(encoding="utf-8")
        m = re.search(r'(?m)^version\s*=\s*"([^"]+)"', text)
        yield rel, m.group(1)
        if write and m.group(1) != version:
            p.write_text(text[: m.start(1)] + version + text[m.end(1):], encoding="utf-8")


def main():
    args = sys.argv[1:]
    check = "--check" in args
    args = [a for a in args if a != "--check"]
    if args and not check:
        if not SEMVER.match(args[0]):
            sys.exit(f"not a version: {args[0]} (want 1.2.3)")
        VERSION_FILE.write_text(args[0] + "\n", encoding="utf-8")
    version = VERSION_FILE.read_text(encoding="utf-8").strip()
    if not SEMVER.match(version):
        sys.exit(f"VERSION must hold 1.2.3, found {version!r}")
    bad = [f"{n}: {v}" for n, v in targets(version, write=not check) if v != version]
    if check:
        if args and args[0] != f"v{version}":
            bad.append(f"tag {args[0]} is not v{version}")
        if bad:
            sys.exit(f"Version mismatch against VERSION {version}:\n  " + "\n  ".join(bad))
        print(f"all at {version}")
    else:
        print(f"version {version}" + (f" (updated: {', '.join(n for n, _ in [b.split(':',1) for b in bad])})" if bad else ""))


if __name__ == "__main__":
    main()
