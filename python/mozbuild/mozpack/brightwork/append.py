# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

"""Build a brightwork delta package from two SDK/package trees.

Given a previous release and a newer one, this collects only the files that were
added or changed in the new tree into a separate folder (mirroring their relative
paths) and, optionally, a .zip.

Both inputs may be a directory or a .zip, and may be whole SDKs (adk-*/, src/,
tools/, build.py, ...) or built packages (the dist/ output with omni.ja pairs).

Removed files (present in old, gone in new) are reported in the returned summary
but not copied, since they no longer exist in the new tree.
"""

import hashlib
import os
import shutil
import tempfile
import zipfile

# Build artifacts/caches that should never be part of a diff
_SKIP_DIR_NAMES = {"__pycache__", ".git"}
_SKIP_SUFFIXES = (".pyc",)
# Skipped only at the tree root (the SDK's build output), not nested
_SKIP_ROOT_DIRS = {"dist"}


def _sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def _hash_tree(root):
    out = {}
    for dirpath, dirnames, filenames in os.walk(root):
        rel_dir = os.path.relpath(dirpath, root).replace(os.sep, "/")
        if rel_dir == ".":
            rel_dir = ""
        # Prune skipped directories in place so os.walk doesn't descend them.
        dirnames[:] = [
            d
            for d in dirnames
            if d not in _SKIP_DIR_NAMES
            and not (rel_dir == "" and d in _SKIP_ROOT_DIRS)
        ]
        for fn in filenames:
            if fn.endswith(_SKIP_SUFFIXES):
                continue
            rel = f"{rel_dir}/{fn}" if rel_dir else fn
            out[rel] = _sha256(os.path.join(dirpath, fn))
    return out


def _resolve_tree(path):
    if os.path.isdir(path):
        return path, None
    if os.path.isfile(path) and zipfile.is_zipfile(path):
        tmp = tempfile.mkdtemp(prefix="bw-append-src-")
        with zipfile.ZipFile(path) as z:
            z.extractall(tmp)
        children = os.listdir(tmp)
        if len(children) == 1 and os.path.isdir(os.path.join(tmp, children[0])):
            return os.path.join(tmp, children[0]), lambda: shutil.rmtree(
                tmp, ignore_errors=True
            )
        return tmp, lambda: shutil.rmtree(tmp, ignore_errors=True)
    raise ValueError("%s is neither a directory nor a .zip" % path)


def _zip_dir(src_dir, zip_path):
    zip_path = os.path.abspath(zip_path)
    os.makedirs(os.path.dirname(zip_path) or ".", exist_ok=True)
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as z:
        for dirpath, _dirs, files in os.walk(src_dir):
            for fn in sorted(files):
                full = os.path.join(dirpath, fn)
                arc = os.path.relpath(full, src_dir).replace(os.sep, "/")
                z.write(full, arc)


def build_append_package(old_path, new_path, output_dir, zip_path=None):
    old_root, old_cleanup = _resolve_tree(old_path)
    new_root, new_cleanup = _resolve_tree(new_path)
    try:
        old_h = _hash_tree(old_root)
        new_h = _hash_tree(new_root)

        added = sorted(r for r in new_h if r not in old_h)
        changed = sorted(r for r in new_h if r in old_h and new_h[r] != old_h[r])
        removed = sorted(r for r in old_h if r not in new_h)

        output_dir = os.path.abspath(output_dir)
        if os.path.isdir(output_dir):
            shutil.rmtree(output_dir)
        os.makedirs(output_dir)

        nbytes = 0
        for rel in added + changed:
            src = os.path.join(new_root, *rel.split("/"))
            dst = os.path.join(output_dir, *rel.split("/"))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copy2(src, dst)
            nbytes += os.path.getsize(src)

        if zip_path:
            _zip_dir(output_dir, zip_path)

        return {
            "added": added,
            "changed": changed,
            "removed": removed,
            "bytes": nbytes,
            "output_dir": output_dir,
            "zip": os.path.abspath(zip_path) if zip_path else None,
        }
    finally:
        if old_cleanup:
            old_cleanup()
        if new_cleanup:
            new_cleanup()
