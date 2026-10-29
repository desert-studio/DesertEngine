#!/usr/bin/env python3
"""Download the vendor runtime binaries the build loads, verify them and unpack them into ThirdParty.

    python scripts/Windows/FetchVendorBinaries.py [--check]

Vendor binaries are DOWNLOADED from the vendor's official release, never committed (owner decision 2026-10-15).
Each vendor directory carries a manifest.json that pins one release: its URL, the SHA-256 of the archive and the
SHA-256 of every file taken out of it, and optionally the Platform it serves ("windows" / "macos"; absent: windows):
a manifest for another platform than the one running the script is skipped. The archive is a .zip or a .tar.gz; a
symbolic link in a tarball is written as the file it points to, under the link's name (the name the libraries load
each other by, e.g. macOS @rpath/libtbb.12.dylib). This script is the only thing that writes those files, and it writes a
file only after its hash matched, so a file on disk under a manifest's Destination is always the pinned one.

Idempotent: a manifest whose files are all present with the pinned hashes is left alone (no network access).
A file that is missing or differs makes the script fetch the archive once and rewrite the listed files.

    --check   Verify only; download nothing. Exit 0 when every file matches.

Exit codes (each failure is also printed by name):
    0  every manifest's files are present and verified
    1  HashMismatch      the archive or a file in it is not the pinned one (nothing was written for that file)
    2  NetworkUnavailable the archive could not be downloaded
    3  ManifestInvalid   a manifest is missing, unreadable, or names a file the archive does not contain
    4  NotFetched        --check found a missing or differing file

Used by scripts/Windows/Setup.bat (step 4b) and scripts/MacOS/Setup.sh. The engine copes with an absent directory: premake still generates,
the build copies nothing and says so, the packager refuses by name and the runtime reports DLSS unavailable.
"""

import argparse
import hashlib
import json
import os
import shutil
import sys
import tarfile
import tempfile
import urllib.error
import urllib.request
import zipfile
from pathlib import Path, PurePosixPath

ROOT = Path(__file__).resolve().parents[2]

# Every vendor directory whose binaries are fetched. One row per vendor SDK.
MANIFESTS = [
    ROOT / "ThirdParty" / "Streamline" / "manifest.json",
    # Intel Open Image Denoise 2.x: the path tracer's OIDN denoiser (Engine/Graphic/Denoise/OidnDenoiser.cpp).
    ROOT / "ThirdParty" / "OIDN" / "manifest.windows.json",
    ROOT / "ThirdParty" / "OIDN" / "manifest.macos.json",
]

# The Platform value of the machine running the script.
HOST_PLATFORM = "macos" if sys.platform == "darwin" else "windows" if sys.platform == "win32" else sys.platform

EXIT_OK = 0
EXIT_HASH_MISMATCH = 1
EXIT_NETWORK = 2
EXIT_MANIFEST = 3
EXIT_NOT_FETCHED = 4

CHUNK = 1 << 20
DOWNLOAD_TIMEOUT_SECONDS = 60


class FetchError(Exception):
    def __init__(self, code, name, message):
        super().__init__(message)
        self.code = code
        self.name = name


def sha256_of_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for block in iter(lambda: stream.read(CHUNK), b""):
            digest.update(block)
    return digest.hexdigest()


def load_manifest(path):
    try:
        manifest = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise FetchError(EXIT_MANIFEST, "ManifestInvalid", f"{path}: {error}") from error
    for key in ("Name", "Version", "Url", "ArchiveSha256", "Destination", "Files"):
        if key not in manifest:
            raise FetchError(EXIT_MANIFEST, "ManifestInvalid", f"{path}: no '{key}'")
    for entry in manifest["Files"]:
        if "Archive" not in entry or "Sha256" not in entry:
            raise FetchError(EXIT_MANIFEST, "ManifestInvalid", f"{path}: a Files entry lacks 'Archive' or 'Sha256'")
    return manifest


def destination_of(manifest_path, manifest, entry):
    return manifest_path.parent / manifest["Destination"] / PurePosixPath(entry["Archive"]).name


def stale_files(manifest_path, manifest):
    """The Files entries whose destination is missing or does not carry the pinned hash."""
    stale = []
    for entry in manifest["Files"]:
        target = destination_of(manifest_path, manifest, entry)
        if not target.is_file() or sha256_of_file(target) != entry["Sha256"].lower():
            stale.append(entry)
    return stale


def download(manifest, into):
    url = manifest["Url"]
    print(f"    downloading {url}")
    digest = hashlib.sha256()
    try:
        with urllib.request.urlopen(url, timeout=DOWNLOAD_TIMEOUT_SECONDS) as response, open(into, "wb") as out:
            for block in iter(lambda: response.read(CHUNK), b""):
                digest.update(block)
                out.write(block)
    except (urllib.error.URLError, OSError, TimeoutError) as error:
        raise FetchError(EXIT_NETWORK, "NetworkUnavailable", f"{url}: {error}") from error
    actual = digest.hexdigest()
    if actual != manifest["ArchiveSha256"].lower():
        raise FetchError(EXIT_HASH_MISMATCH, "HashMismatch",
                         f"{url}: archive SHA-256 is {actual}, the manifest pins {manifest['ArchiveSha256']}")


class _ZipReader:
    def __init__(self, archive):
        self.bundle = zipfile.ZipFile(archive)

    def names(self):
        return set(self.bundle.namelist())

    def open(self, name):
        return self.bundle.open(name)

    def close(self):
        self.bundle.close()


class _TarReader:
    def __init__(self, archive):
        self.bundle = tarfile.open(archive, "r:gz")

    def names(self):
        return set(self.bundle.getnames())

    def open(self, name):
        # extractfile follows a symbolic or hard link to the member it names.
        stream = self.bundle.extractfile(self.bundle.getmember(name))
        if stream is None:
            raise FetchError(EXIT_MANIFEST, "ManifestInvalid", f"{name} is not a file in the archive")
        return stream

    def close(self):
        self.bundle.close()


def open_archive(manifest, archive):
    url = manifest["Url"]
    try:
        if url.endswith(".tar.gz") or url.endswith(".tgz"):
            return _TarReader(archive)
        return _ZipReader(archive)
    except (zipfile.BadZipFile, tarfile.TarError) as error:
        raise FetchError(EXIT_HASH_MISMATCH, "HashMismatch", f"{url}: not a readable archive: {error}") from error


def extract(manifest_path, manifest, archive, entries):
    bundle = open_archive(manifest, archive)
    try:
        names = bundle.names()
        for entry in entries:
            if entry["Archive"] not in names:
                raise FetchError(EXIT_MANIFEST, "ManifestInvalid",
                                 f"{manifest_path}: {entry['Archive']} is not in {manifest['Url']}")
            target = destination_of(manifest_path, manifest, entry)
            target.parent.mkdir(parents=True, exist_ok=True)
            # Written beside the target and renamed over it only after the hash matched: an interrupted or wrong
            # extraction never leaves a file under the pinned name.
            partial = target.with_name(target.name + ".partial")
            digest = hashlib.sha256()
            with bundle.open(entry["Archive"]) as source, open(partial, "wb") as out:
                for block in iter(lambda: source.read(CHUNK), b""):
                    digest.update(block)
                    out.write(block)
            if digest.hexdigest() != entry["Sha256"].lower():
                partial.unlink()
                raise FetchError(EXIT_HASH_MISMATCH, "HashMismatch",
                                 f"{entry['Archive']}: SHA-256 is {digest.hexdigest()}, the manifest pins {entry['Sha256']}")
            os.replace(partial, target)
            print(f"    {target.relative_to(ROOT).as_posix()}: verified")
    finally:
        bundle.close()


def process(manifest_path, check_only):
    manifest = load_manifest(manifest_path)
    if manifest.get("Platform", "windows") != HOST_PLATFORM:
        return
    label = f"{manifest['Name']} {manifest['Version']}"
    stale = stale_files(manifest_path, manifest)
    if not stale:
        print(f"--- {label}: {len(manifest['Files'])} file(s) present and verified")
        return
    if check_only:
        names = ", ".join(PurePosixPath(entry["Archive"]).name for entry in stale)
        raise FetchError(EXIT_NOT_FETCHED, "NotFetched", f"{label}: missing or differing: {names}")
    print(f"--- {label}: fetching {len(stale)} of {len(manifest['Files'])} file(s)")
    scratch = Path(tempfile.mkdtemp(prefix="desert-vendor-"))
    try:
        archive = scratch / PurePosixPath(manifest["Url"]).name
        download(manifest, archive)
        extract(manifest_path, manifest, archive, stale)
    finally:
        shutil.rmtree(scratch, ignore_errors=True)


def main():
    parser = argparse.ArgumentParser(description="Fetch and verify the vendor runtime binaries.")
    parser.add_argument("--check", action="store_true", help="verify only, download nothing")
    args = parser.parse_args()
    for manifest_path in MANIFESTS:
        try:
            process(manifest_path, args.check)
        except FetchError as error:
            print(f"[ERROR] {error.name}: {error}", file=sys.stderr)
            return error.code
    return EXIT_OK


if __name__ == "__main__":
    sys.exit(main())
