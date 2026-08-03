#!/usr/bin/env python3
"""Package Grajek firmware for GitHub Releases, Pages and M5Launcher."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
from pathlib import Path


RELEASE_FILES = {
    "grajek-cardputer-adv.bin": ("en", "firmware.factory.bin"),
    "grajek-cardputer-adv-pl.bin": ("pl", "firmware.factory.bin"),
}
SEMVER_TAG = re.compile(
    r"^v\d+\.\d+\.\d+(?:-[0-9A-Za-z]+(?:[.-][0-9A-Za-z]+)*)?"
    r"(?:\+[0-9A-Za-z]+(?:[.-][0-9A-Za-z]+)*)?$"
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--version", required=True, help="release tag, e.g. v0.1.0")
    parser.add_argument("--en-build", type=Path, required=True)
    parser.add_argument("--pl-build", type=Path, required=True)
    parser.add_argument("--web-source", type=Path, default=Path("web"))
    parser.add_argument("--output", type=Path, required=True)
    return parser.parse_args()


def source_file(builds: dict[str, Path], language: str, name: str) -> Path:
    path = builds[language] / name
    if not path.is_file():
        raise SystemExit(f"missing build output: {path}")
    image = path.read_bytes()
    if len(image) <= 0x10000:
        raise SystemExit(f"factory image is too short: {path}")
    if image[0] != 0xE9:
        raise SystemExit(f"missing ESP bootloader at 0x0: {path}")
    if image[0x8000:0x8002] != b"\xaaP":
        raise SystemExit(f"missing partition table at 0x8000: {path}")
    if image[0x10000] != 0xE9:
        raise SystemExit(f"missing application image at 0x10000: {path}")
    return path


def write_manifest(path: Path, version: str, firmware_name: str) -> None:
    manifest = {
        "name": "Grajek",
        "version": version.removeprefix("v"),
        "new_install_prompt_erase": False,
        "new_install_improv_wait_time": 0,
        "builds": [
            {
                "chipFamily": "ESP32-S3",
                "parts": [
                    {
                        "path": f"firmware/{firmware_name}",
                        "offset": 0,
                    }
                ],
            }
        ],
    }
    path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main() -> None:
    args = parse_args()
    if not SEMVER_TAG.fullmatch(args.version):
        raise SystemExit(f"release version must be a SemVer tag such as v0.1.0: {args.version}")
    if args.output.exists() and (
        not args.output.is_dir() or any(args.output.iterdir())
    ):
        raise SystemExit(f"output path must be an empty directory: {args.output}")

    builds = {"en": args.en_build, "pl": args.pl_build}
    if sha256(source_file(builds, "en", "firmware.factory.bin")) == sha256(
        source_file(builds, "pl", "firmware.factory.bin")
    ):
        raise SystemExit("English and Polish factory images are unexpectedly identical")

    release_dir = args.output / "release"
    site_dir = args.output / "site"
    firmware_dir = site_dir / "firmware"

    if not args.web_source.is_dir():
        raise SystemExit(f"missing web source: {args.web_source}")

    release_dir.mkdir(parents=True, exist_ok=True)
    firmware_dir.mkdir(parents=True, exist_ok=True)
    shutil.copytree(args.web_source, site_dir, dirs_exist_ok=True)

    packaged: list[Path] = []
    for target_name, (language, source_name) in RELEASE_FILES.items():
        target = release_dir / target_name
        shutil.copy2(source_file(builds, language, source_name), target)
        packaged.append(target)

    # The browser installer uses self-contained merged images. Keeping them in
    # the Pages artifact avoids cross-origin downloads from GitHub Releases.
    for target_name in ("grajek-cardputer-adv.bin", "grajek-cardputer-adv-pl.bin"):
        shutil.copy2(release_dir / target_name, firmware_dir / target_name)

    write_manifest(
        site_dir / "manifest.json",
        args.version,
        "grajek-cardputer-adv.bin",
    )
    write_manifest(
        site_dir / "manifest-pl.json",
        args.version,
        "grajek-cardputer-adv-pl.bin",
    )

    sums = "".join(f"{sha256(path)}  {path.name}\n" for path in sorted(packaged))
    (release_dir / "SHA256SUMS").write_text(sums, encoding="utf-8")

    notes = f"""# Grajek {args.version}

Firmware for **M5Stack Cardputer-ADV only**.

- `grajek-cardputer-adv.bin` — English (default).
- `grajek-cardputer-adv-pl.bin` — Polish.

Both are merged factory images, truncated after the application payload. They
work with the browser installer, esptool at flash offset `0x0` and M5Launcher.
A direct USB/web installation resets saved Grajek settings and phrases;
M5Launcher safely extracts and installs the application from the merged image.
"""
    (release_dir / "RELEASE_NOTES.md").write_text(notes, encoding="utf-8")


if __name__ == "__main__":
    main()
