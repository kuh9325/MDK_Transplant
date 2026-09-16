#!/usr/bin/env python3
"""hash_manifest.py — recursive SHA-256 manifest for an explicitly supplied directory.

Read-only forensic aid for the MDK-Native project:
  - lists all regular files under the supplied root (relative paths)
  - records size and SHA-256 for each
  - never modifies, uploads, or deletes anything
  - never follows symlinks, and refuses to read anything that resolves
    outside the explicitly supplied root

Output: JSON (default) or TSV to stdout.

Usage:
    python3 tools/hash_manifest.py <directory> [--format json|tsv]
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
from pathlib import Path

_CHUNK = 1 << 20  # 1 MiB read chunks


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(_CHUNK), b""):
            h.update(chunk)
    return h.hexdigest()


def build_manifest(root: Path) -> dict:
    """Return manifest dict for all regular files under root.

    Symlinks are not followed; a symlink entry is recorded with
    size/sha256 = None and flagged, so manifests stay complete without
    ever dereferencing outside the root.
    """
    root = root.resolve()
    if not root.is_dir():
        raise NotADirectoryError(f"not a directory: {root}")

    entries = []
    for dirpath, dirnames, filenames in os.walk(root, followlinks=False):
        dirnames.sort()
        for name in sorted(filenames):
            p = Path(dirpath) / name
            rel = p.relative_to(root).as_posix()
            # Defense in depth: resolved path must stay under root.
            resolved = p.resolve()
            if not str(resolved).startswith(str(root) + os.sep):
                entries.append(
                    {"path": rel, "size": None, "sha256": None,
                     "note": "skipped: resolves outside root"}
                )
                continue
            if p.is_symlink():
                entries.append(
                    {"path": rel, "size": None, "sha256": None,
                     "note": "symlink (not followed)"}
                )
                continue
            if not p.is_file():
                continue
            entries.append(
                {"path": rel, "size": p.stat().st_size,
                 "sha256": sha256_file(p)}
            )
    return {"root": str(root), "file_count": len(entries), "files": entries}


def emit_json(manifest: dict, out) -> None:
    json.dump(manifest, out, indent=2, sort_keys=False)
    out.write("\n")


def emit_tsv(manifest: dict, out) -> None:
    out.write("path\tsize\tsha256\n")
    for e in manifest["files"]:
        size = "" if e["size"] is None else e["size"]
        sha = "" if e["sha256"] is None else e["sha256"]
        out.write(f"{e['path']}\t{size}\t{sha}\n")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        description="Recursive SHA-256 manifest of an explicitly supplied directory."
    )
    ap.add_argument("directory", help="root directory to inventory")
    ap.add_argument("--format", choices=["json", "tsv"], default="json")
    args = ap.parse_args(argv)

    try:
        manifest = build_manifest(Path(args.directory))
    except (NotADirectoryError, FileNotFoundError, PermissionError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    if args.format == "json":
        emit_json(manifest, sys.stdout)
    else:
        emit_tsv(manifest, sys.stdout)
    return 0


if __name__ == "__main__":
    sys.exit(main())
