#!/usr/bin/env python3
"""Build a HiBy .compas database off-device for a large music library.

The scanner parses tags once, streams bounded records to the production
metadata_db writer, and leaves the result in a staging directory. It never
touches an existing card database unless --deploy is explicitly supplied.
The writer format requires a little-endian host; deployment requires Linux.
"""
from __future__ import annotations

import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import ctypes
import ctypes.util
import json
from itertools import zip_longest
from pathlib import Path

if sys.byteorder != "little":
    raise SystemExit("offline database writer requires a little-endian host (matches the player format)")

SUPPORTED = {".mp3", ".aac", ".m4a", ".m4b", ".flac", ".wav", ".ape", ".ogg", ".opus", ".wma", ".aiff", ".aif", ".dsf", ".dff"}
HEADER = struct.Struct("<Iqq5Hii")
try:
    from mutagen import File as mutagen_file  # type: ignore
except ImportError:
    mutagen_file = None
tag_warnings = 0

def text(value: object, fallback: str = "") -> str:
    if isinstance(value, list): value = value[0] if value else fallback
    return str(value or fallback).replace("\x00", "")

def riff_info(path: Path) -> dict[str, str]:
    """Read small RIFF/WAVE INFO chunks without loading audio data."""
    result: dict[str, str] = {}
    try:
        with path.open("rb") as stream:
            if stream.read(4) != b"RIFF" or len(stream.read(4)) != 4 or stream.read(4) != b"WAVE":
                return result
            while True:
                header = stream.read(8)
                if len(header) != 8:
                    break
                chunk, size = struct.unpack("<4sI", header)
                if chunk != b"LIST" or size < 4:
                    stream.seek(size + (size & 1), os.SEEK_CUR)
                    continue
                subtype = stream.read(4)
                if subtype != b"INFO" or size - 4 > 16 * 1024 * 1024:
                    stream.seek(size - 4 + ((size - 4) & 1), os.SEEK_CUR)
                    continue
                payload = subtype + stream.read(size - 4)
                if len(payload) != size:
                    break
                if payload[:4] == b"INFO":
                    pos = 4
                    while pos + 8 <= len(payload):
                        key, length = struct.unpack_from("<4sI", payload, pos)
                        pos += 8
                        if length > len(payload) - pos:
                            break
                        value = payload[pos:pos + length].rstrip(b"\0").decode("utf-8", "replace").strip()
                        pos += length + (length & 1)
                        field = {b"INAM": "title", b"IART": "artist", b"IPRD": "album", b"IGNR": "genre", b"ITRK": "tracknumber"}.get(key)
                        if field and value:
                            result[field] = value
                if size & 1:
                    stream.seek(1, os.SEEK_CUR)
    except (OSError, struct.error):
        return {}
    return result

def tags(path: Path) -> tuple[str, str, str, str, str, int, int]:
    title, artist, album, album_artist, genre = path.stem, "Unknown Artist", "Unknown Album", "Unknown Artist", "Unknown Genre"
    track = disc = -1
    info = riff_info(path) if path.suffix.lower() == ".wav" else {}
    title = info.get("title", title)
    artist = info.get("artist", artist)
    album = info.get("album", album)
    album_artist = artist
    genre = info.get("genre", genre)
    try:
        track = int(info.get("tracknumber", "-1").split("/", 1)[0])
    except ValueError:
        pass
    try:
        audio = mutagen_file(path, easy=True) if mutagen_file else None
        if audio is not None:
            title = text(audio.get("title"), title)
            artist = text(audio.get("artist"), artist)
            album = text(audio.get("album"), album)
            album_artist = text(audio.get("albumartist"), artist)
            genre = text(audio.get("genre"), genre)
            try: track = int(text(audio.get("tracknumber"), "-1").split("/", 1)[0])
            except ValueError: pass
            try: disc = int(text(audio.get("discnumber"), "-1").split("/", 1)[0])
            except ValueError: pass
    except Exception as exc:
        global tag_warnings
        tag_warnings += 1
        if tag_warnings <= 10:
            print(f"warning: tag read failed for {path}: {exc}", file=sys.stderr)
    return title, artist, album, album_artist, genre, track, disc

def enc(value: str) -> bytes:
    return value.encode("utf-8", "replace")[:127].decode("utf-8", "ignore").encode("utf-8")

def library_files(root: Path):
    """Yield supported files with device scan ignore/unignore inheritance."""
    stack = [(root, True, 0)]
    while stack:
        directory, include, depth = stack.pop()
        try: entries = list(os.scandir(directory))
        except OSError as exc: raise OSError(f"cannot read {directory}: {exc}") from exc
        marker_ignore = (directory / "database.ignore").is_file()
        marker_unignore = (directory / "database.unignore").is_file()
        if marker_ignore != marker_unignore: include = marker_unignore
        for entry in sorted(entries, key=lambda e: e.name, reverse=True):
            if entry.name.startswith(".") or entry.is_symlink(): continue
            if entry.is_dir(follow_symlinks=False):
                if depth == 0 and entry.name.casefold() == "audiobooks": continue
                stack.append((Path(entry.path), include, depth + 1)); continue
            if include and Path(entry.name).suffix.lower() in SUPPORTED: yield Path(entry.path)

def rename_noreplace(dir_fd: int, source: str, target: str) -> None:
    libc = ctypes.CDLL(ctypes.util.find_library("c"), use_errno=True)
    renameat2 = getattr(libc, "renameat2", None)
    if renameat2 is None: raise OSError("renameat2 is unavailable; refusing unsafe deployment")
    renameat2.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_uint]
    renameat2.restype = ctypes.c_int
    if renameat2(dir_fd, source.encode(), dir_fd, target.encode(), 1) != 0:
        err = ctypes.get_errno(); raise OSError(err, os.strerror(err))

def compile_writer(root: Path, build: Path) -> Path:
    out = build / "offline-library-writer"
    common = ["-std=gnu11", "-O2", "-Wall", "-Wextra", "-DHOST_BUILD=1", "-ffunction-sections", "-fdata-sections",
              f"-I{root}", f"-I{root / 'src'}", f"-I{root / 'src/library'}", f"-I{root / 'src/core'}", f"-I{root / 'src/ui'}", f"-I{root / 'lvgl'}"]
    cmd = ["cc", *common, str(root / "scripts/offline_library_scan.c"), str(root / "src/library/metadata_db.c"), str(root / "src/library/tagcache.c"), str(root / "src/core/db_log.c"), "-Wl,--gc-sections", "-pthread", "-o", str(out)]
    subprocess.run(cmd, check=True)
    return out

def deploy_database(staging: Path, card: Path, manifest: Path) -> None:
    card_fd = os.open(card, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
    temp_target = None
    try:
        card_anchor = Path(f"/proc/self/fd/{card_fd}")
        target = card_anchor / ".compas"
        if target.exists():
            raise SystemExit("refusing deploy: card already has .compas; remove it explicitly after backup")

        def scanned_rows():
            with manifest.open(encoding="utf-8") as source:
                for line in source:
                    row = json.loads(line)
                    yield (row["path"], int(row["mtime"]), int(row["size"]))

        def destination_rows():
            for source in library_files(card_anchor):
                relative = source.relative_to(card_anchor).as_posix()
                stat = source.stat()
                yield (relative, int(stat.st_mtime), stat.st_size)

        for expected, actual in zip_longest(scanned_rows(), destination_rows()):
            if expected != actual:
                raise SystemExit("refusing deploy: destination library differs from scanned source")

        temp_target = Path(tempfile.mkdtemp(prefix=".compas.offline-", dir=card_anchor))
        shutil.copytree(staging / ".compas", temp_target, dirs_exist_ok=True)
        for base, _, names in os.walk(temp_target):
            for name in names:
                with (Path(base) / name).open("rb") as copied:
                    os.fsync(copied.fileno())
        temp_fd = os.open(temp_target, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
        try:
            os.fsync(temp_fd)
        finally:
            os.close(temp_fd)
        os.fsync(card_fd)
        rename_noreplace(card_fd, temp_target.name, ".compas")
        temp_target = None
        os.fsync(card_fd)
    finally:
        if temp_target is not None:
            shutil.rmtree(temp_target, ignore_errors=True)
        os.close(card_fd)

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("library", type=Path, help="mounted card root containing Music/")
    ap.add_argument("--player-root", default="/data/mnt/sd_0", help="path prefix used by the player (default: /data/mnt/sd_0)")
    ap.add_argument("--staging", type=Path, help="output directory; default: temporary directory")
    ap.add_argument("--deploy", action="store_true", help="copy staged .compas into --card only after validation")
    ap.add_argument("--card", type=Path, help="mounted card destination required with --deploy")
    ap.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1], help=argparse.SUPPRESS)
    args = ap.parse_args()
    library = args.library.resolve()
    if not library.is_dir() or not (library / "Music").is_dir():
        ap.error("library must be a card root containing Music/")
    if args.deploy and not args.card: ap.error("--deploy requires --card")
    staging = args.staging.resolve() if args.staging else Path(tempfile.mkdtemp(prefix="hiby-offline-db-"))
    staging.mkdir(parents=True, exist_ok=True)
    if any(staging.iterdir()): ap.error(f"staging directory is not empty: {staging}")
    spool = staging / "records.bin"
    manifest = staging / "manifest.jsonl"
    count = 0
    try:
        with spool.open("wb") as out, manifest.open("w", encoding="utf-8") as mf:
            for source in library_files(library):
                    try: st = source.stat()
                    except OSError as exc: raise OSError(f"stat failed for {source}: {exc}") from exc
                    rel = source.relative_to(library).as_posix()
                    mf.write(json.dumps({"path": rel, "mtime": int(st.st_mtime), "size": st.st_size}, ensure_ascii=False) + "\n")
                    device_path = args.player_root.rstrip("/") + "/" + rel
                    values = tags(source)
                    encoded = [enc(v) for v in values[:5]]
                    out.write(HEADER.pack(len(device_path.encode()), int(st.st_mtime), st.st_size, *(len(v) for v in encoded), values[5], values[6]))
                    out.write(device_path.encode())
                    for value in encoded: out.write(value)
                    count += 1
        if mutagen_file is None:
            print("warning: python-mutagen is unavailable; using filename/default metadata", file=sys.stderr)
        elif tag_warnings > 10:
            print(f"warning: {tag_warnings - 10} additional tag read failures suppressed", file=sys.stderr)
        print(f"scanned {count} audio files; writing staged database", file=sys.stderr)
        build = staging / ".build"; build.mkdir()
        writer = compile_writer(args.root.resolve(), build)
        subprocess.run([str(writer), str(spool), str(count)], cwd=staging, check=True)
        spool.unlink()
        shutil.rmtree(build)
        if args.deploy:
            if sys.platform != "linux": raise SystemExit("refusing deploy: atomic card publication requires Linux renameat2/procfs")
            card = args.card.resolve()
            if not card.is_dir() or not (card / "Music").is_dir() or not os.path.ismount(card):
                raise SystemExit("refusing deploy: --card must be a mounted card root containing Music/")
            deploy_database(staging, card, manifest)
            print(f"deployed validated database to {card / '.compas'}")
        else:
            print(f"validated database: {staging / '.compas'}")
        return 0
    finally:
        pass

if __name__ == "__main__": raise SystemExit(main())
