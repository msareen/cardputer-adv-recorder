#!/usr/bin/env python3
"""Build a full release into ./dist (git-ignored) with one command:

    python build.py

Produces:
    dist/firmware/   flashable image (full, at 0x0) + M5Launcher app image
    dist/desktop/    the desktop app, ready to double-click:
                       Windows: "Cardputer Sync.exe" (portable) + installer
                       macOS:   "Cardputer Sync.app" + .dmg
                       Linux:   .AppImage (+ .deb)
    dist/SHA256SUMS.txt, dist/README.txt

Works on Windows, macOS and Linux. Needs Python 3.9+, PlatformIO (for the
firmware), Node.js 18+ with npm and a Rust toolchain (for the desktop app).

Options:
    --skip-firmware     only build the desktop app
    --skip-desktop      only build the firmware
    --skip-tests        do not run the Rust and native firmware tests first
    --no-installer      desktop: build the app only, no installer/dmg
    --universal         macOS: one app for Apple Silicon and Intel
    --with-mock         also build tools/mock-device (dev tool)
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
DIST = ROOT / "dist"
FIRMWARE = ROOT / "firmware"
DESKTOP = ROOT / "desktop"
TAURI = DESKTOP / "src-tauri"
MOCK = ROOT / "tools" / "mock-device"
FW_ENV = "cardputer-adv-recorder"

IS_WINDOWS = os.name == "nt"
IS_MAC = sys.platform == "darwin"


def log(msg: str) -> None:
    print(f"\n==> {msg}", flush=True)


def fail(msg: str) -> "NoReturn":  # type: ignore[name-defined]
    print(f"\nERROR: {msg}", file=sys.stderr)
    sys.exit(1)


def run(cmd: list[str], cwd: Path, env: dict | None = None) -> None:
    print(f"$ {' '.join(cmd)}   (in {cwd.relative_to(ROOT) if cwd != ROOT else '.'})", flush=True)
    result = subprocess.run(cmd, cwd=cwd, env=env)
    if result.returncode != 0:
        fail(f"command failed with exit code {result.returncode}: {' '.join(cmd)}")


def which(*names: str) -> str | None:
    for name in names:
        found = shutil.which(name)
        if found:
            return found
    return None


def version() -> str:
    """The git tag if HEAD is tagged, else the desktop version plus the commit."""
    try:
        return subprocess.check_output(
            ["git", "describe", "--tags", "--exact-match"], cwd=ROOT, stderr=subprocess.DEVNULL, text=True
        ).strip()
    except (subprocess.CalledProcessError, FileNotFoundError):
        pass
    conf = json.loads((TAURI / "tauri.conf.json").read_text(encoding="utf-8"))
    try:
        commit = subprocess.check_output(["git", "rev-parse", "--short", "HEAD"], cwd=ROOT, text=True).strip()
        dirty = subprocess.run(["git", "diff", "--quiet", "HEAD"], cwd=ROOT).returncode != 0
        return f"v{conf['version']}-{commit}{'-dirty' if dirty else ''}"
    except (subprocess.CalledProcessError, FileNotFoundError):
        return f"v{conf['version']}"


# --- firmware -----------------------------------------------------------------


def platformio_cmd() -> list[str]:
    exe = which("platformio", "pio")
    if exe:
        return [exe]
    # pip install --user puts it outside PATH on Windows.
    if IS_WINDOWS:
        appdata = Path(os.environ.get("APPDATA", ""))
        for scripts in sorted(appdata.glob("Python/Python3*/Scripts"), reverse=True):
            for name in ("platformio.exe", "pio.exe"):
                if (scripts / name).exists():
                    return [str(scripts / name)]
    penv = Path.home() / ".platformio" / "penv" / ("Scripts" if IS_WINDOWS else "bin")
    for name in ("platformio", "pio"):
        candidate = penv / (name + (".exe" if IS_WINDOWS else ""))
        if candidate.exists():
            return [str(candidate)]
    try:
        subprocess.run([sys.executable, "-m", "platformio", "--version"], check=True, capture_output=True)
        return [sys.executable, "-m", "platformio"]
    except (subprocess.CalledProcessError, FileNotFoundError):
        fail("PlatformIO not found. Install it with: python -m pip install platformio")


def native_test_env() -> dict | None:
    """Env for the host-side firmware tests, or None if no C++ compiler."""
    env = os.environ.copy()
    if not IS_WINDOWS:
        return env if which("g++", "c++") else None
    # Windows: MinGW's linker breaks when its install path contains a space
    # (e.g. WinGet's under C:\Users\<First Last>), so prefer a space-free copy.
    for candidate in (os.environ.get("CPREC_MINGW"), r"D:\tools\mingw64\bin", r"C:\mingw64\bin", r"C:\msys64\ucrt64\bin"):
        if candidate and Path(candidate, "g++.exe").exists() and " " not in candidate:
            env["PATH"] = candidate + os.pathsep + env["PATH"]
            return env
    found = which("g++")
    return env if found and " " not in found else None


def build_firmware(ver: str, tests: bool) -> list[Path]:
    pio = platformio_cmd()
    log("Firmware: build")
    run(pio + ["run", "-e", FW_ENV], FIRMWARE)
    if tests:
        env = native_test_env()
        if env is None:
            print("   (skipping native firmware tests: no g++ found; set CPREC_MINGW on Windows)")
        else:
            log("Firmware: native tests")
            run(pio + ["test", "-e", "native-tests"], FIRMWARE, env)

    log("Firmware: package")
    build = FIRMWARE / ".pio" / "build" / FW_ENV
    # PlatformIO reports its own Python (which has pyserial for esptool) and home dir.
    info = json.loads(subprocess.check_output(pio + ["system", "info", "--json-output"], text=True))
    pio_python = info["python_exe"]["value"]
    pio_home = Path(info["core_dir"]["value"])
    boot_app0 = pio_home / "packages" / "framework-arduinoespressif32" / "tools" / "partitions" / "boot_app0.bin"
    esptool = pio_home / "packages" / "tool-esptoolpy" / "esptool.py"
    for needed in (build / "bootloader.bin", build / "partitions.bin", build / "firmware.bin", boot_app0, esptool):
        if not needed.exists():
            fail(f"missing build output: {needed}")
    out = DIST / "firmware"
    out.mkdir(parents=True, exist_ok=True)
    image = out / f"cardputer-adv-recorder-{ver}.bin"
    launcher = out / f"cardputer-adv-recorder-{ver}-m5launcher.bin"
    # Same layout as firmware/scripts/package-release.sh, with PlatformIO's own esptool.
    run(
        [pio_python, str(esptool), "--chip", "esp32s3", "merge_bin",
           "-o", str(image), "--flash_mode", "dio", "--flash_freq", "80m", "--flash_size", "8MB",
           "0x0000", str(build / "bootloader.bin"),
           "0x8000", str(build / "partitions.bin"),
           "0xe000", str(boot_app0),
           "0x10000", str(build / "firmware.bin")],
        FIRMWARE,
    )
    shutil.copy2(build / "firmware.bin", launcher)
    return [image, launcher]


# --- desktop --------------------------------------------------------------------


def build_desktop(tests: bool, installer: bool, universal: bool) -> list[Path]:
    npm = which("npm")
    cargo = which("cargo")
    if not npm:
        fail("npm not found. Install Node.js 18+ from https://nodejs.org")
    if not cargo:
        fail("cargo not found. Install Rust from https://rustup.rs")

    log("Desktop: install frontend dependencies")
    run([npm, "ci" if (DESKTOP / "package-lock.json").exists() else "install"], DESKTOP)
    if tests:
        log("Desktop: tests")
        run([cargo, "test"], TAURI)

    target = None
    if IS_MAC and universal:
        target = "universal-apple-darwin"
        rustup = which("rustup")
        if rustup:
            run([rustup, "target", "add", "aarch64-apple-darwin", "x86_64-apple-darwin"], ROOT)

    if not installer:
        bundles = ["app"] if IS_MAC else []
    elif IS_WINDOWS:
        bundles = ["nsis"]
    elif IS_MAC:
        bundles = ["app", "dmg"]
    else:
        bundles = ["appimage", "deb"]

    log("Desktop: release build (takes a few minutes)")
    cmd = [npm, "run", "tauri", "--", "build"]
    if target:
        cmd += ["--target", target]
    cmd += ["--bundles", ",".join(bundles)] if bundles else ["--no-bundle"]
    run(cmd, DESKTOP)

    release = TAURI / "target" / (target or "") / "release"
    out = DIST / "desktop"
    out.mkdir(parents=True, exist_ok=True)
    produced: list[Path] = []
    conf = json.loads((TAURI / "tauri.conf.json").read_text(encoding="utf-8"))
    product = conf["productName"]

    if IS_WINDOWS:
        # The exe embeds the frontend, so it runs on its own (WebView2 ships with Windows 10/11).
        exe = out / f"{product}.exe"
        shutil.copy2(release / "cprec-desktop.exe", exe)
        produced.append(exe)
    elif not IS_MAC:
        binary = out / "cardputer-sync"
        shutil.copy2(release / "cprec-desktop", binary)
        binary.chmod(0o755)
        produced.append(binary)

    bundle_dir = release / "bundle"
    if bundle_dir.exists():
        for kind in ("nsis", "msi", "dmg", "appimage", "deb"):
            for f in sorted((bundle_dir / kind).glob("*")) if (bundle_dir / kind).exists() else []:
                if f.is_file() and f.suffix.lower() in {".exe", ".msi", ".dmg", ".appimage", ".deb"}:
                    dest = out / f.name
                    shutil.copy2(f, dest)
                    produced.append(dest)
        for app in sorted((bundle_dir / "macos").glob("*.app")) if (bundle_dir / "macos").exists() else []:
            dest = out / app.name
            if dest.exists():
                shutil.rmtree(dest)
            # symlinks=True keeps the frameworks inside the bundle intact.
            shutil.copytree(app, dest, symlinks=True)
            produced.append(dest)
    return produced


def build_mock() -> list[Path]:
    cargo = which("cargo") or fail("cargo not found")
    log("Mock device: release build")
    run([cargo, "build", "--release"], MOCK)
    name = "mock-device.exe" if IS_WINDOWS else "mock-device"
    src = MOCK / "target" / "release" / name
    out = DIST / "tools"
    out.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, out / name)
    return [out / name]


# --- outputs --------------------------------------------------------------------


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def write_readme(ver: str, files: list[Path]) -> None:
    lines = [
        f"Cardputer ADV Recorder {ver}",
        f"Built {time.strftime('%Y-%m-%d %H:%M')} on {platform.system()} {platform.machine()}",
        "",
        "Firmware (firmware/):",
        "  *-<version>.bin              full image: flash at 0x0, e.g.",
        "                               esptool.py --chip esp32s3 write_flash 0x0 cardputer-adv-recorder-<version>.bin",
        "  *-m5launcher.bin             app-only image for M5Launcher",
        "",
        "Desktop app (desktop/):",
        "  Windows: double-click 'Cardputer Sync.exe' (no install needed), or run the setup .exe to install.",
        "  macOS:   double-click 'Cardputer Sync.app' (drag it to Applications), or open the .dmg.",
        "           Unsigned builds copied from another Mac: right-click > Open the first time.",
        "  Linux:   run the .AppImage (chmod +x first), or install the .deb.",
        "",
        "Files:",
    ]
    lines += [f"  {p.relative_to(DIST).as_posix()}" for p in files]
    (DIST / "README.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser(description="Build the full release into ./dist")
    parser.add_argument("--skip-firmware", action="store_true")
    parser.add_argument("--skip-desktop", action="store_true")
    parser.add_argument("--skip-tests", action="store_true")
    parser.add_argument("--no-installer", action="store_true")
    parser.add_argument("--universal", action="store_true", help="macOS: Apple Silicon + Intel")
    parser.add_argument("--with-mock", action="store_true", help="also build tools/mock-device")
    args = parser.parse_args()

    started = time.time()
    ver = version()
    log(f"Release {ver} -> {DIST}")
    if DIST.exists():
        shutil.rmtree(DIST)
    DIST.mkdir()

    files: list[Path] = []
    if not args.skip_firmware:
        files += build_firmware(ver, tests=not args.skip_tests)
    if not args.skip_desktop:
        files += build_desktop(tests=not args.skip_tests, installer=not args.no_installer, universal=args.universal)
    if args.with_mock:
        files += build_mock()

    sums = [f"{sha256(p)}  {p.relative_to(DIST).as_posix()}" for p in files if p.is_file()]
    (DIST / "SHA256SUMS.txt").write_text("\n".join(sums) + "\n", encoding="utf-8")
    write_readme(ver, files)

    log(f"Done in {int(time.time() - started)} s. Contents of dist/:")
    for p in files:
        print(f"   {p.relative_to(DIST).as_posix()}")


if __name__ == "__main__":
    main()
