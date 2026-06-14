# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

import os
import re
import shutil

import mozpack.path as mozpath
from mozpack.manifests import InstallManifest

from mozpack.brightwork.recipe import export_recipe, rebase_source

# Preprocessor directives
_INCLUDE_RE = re.compile(r'^\s*[#%@]\s*include(?:subst)?\s+"?([^"\s]+)"?')

# Python the standalone build imports, which were snatched from running
# this script like a thousand times and finding out which files were there.
# we include the entire mozpack since its small, but mozbuild only needs these.
MBLIBS = (
    "__init__.py",
    "dirutils.py",
    "makeutil.py",
    "preprocessor.py",
    "util.py",
)
TPLIBS = ("jsmin", "packaging", "python-hglib")


def extract_standalone(buildconfig, manifest_paths, output_dir):
    output_dir = os.path.abspath(output_dir)
    os.makedirs(output_dir, exist_ok=True)

    adk_dir = os.path.join(output_dir, "adk")
    if os.path.isdir(adk_dir):
        shutil.rmtree(adk_dir)
    recipe = export_recipe(buildconfig, adk_dir, manifest_paths)

    src_files, src_bytes = _copy_source_closure(
        adk_dir, recipe, os.path.join(output_dir, "src")
    )
    _vendor_python(buildconfig.topsrcdir, os.path.join(output_dir, "tools"))
    _write_driver(output_dir)

    return {
        "source_files": src_files,
        "source_bytes": src_bytes,
    }


def _copy_source_closure(dadir, recipe, src_out):
    if os.path.isdir(src_out):
        shutil.rmtree(src_out)
    top = recipe.topsrcdir
    sources = set()
    for rel in recipe.manifests:
        manifest = InstallManifest(path=os.path.join(dadir, rel))
        for dest, entry in manifest._dests.items():
            itype = entry[0]
            if itype in (
                InstallManifest.LINK,
                InstallManifest.COPY,
                InstallManifest.PREPROCESS,
            ):
                src = entry[1]
                if rebase_source(src, top, "/__p__") is not None and os.path.isfile(
                    src
                ):
                    sources.add(src)
                    if itype == InstallManifest.PREPROCESS:
                        _collect_includes(src, top, sources)
            elif itype in (InstallManifest.PATTERN_LINK, InstallManifest.PATTERN_COPY):
                _, base, pattern, _dest = entry
                if rebase_source(base, top, "/__p__") is None or not os.path.isdir(
                    base
                ):
                    continue
                from mozpack.files import FileFinder

                finder = FileFinder(base)
                for found, _ in finder.find(pattern):
                    sources.add(os.path.join(base, found))

    nbytes = 0
    sources = {s for s in sources if rebase_source(s, top, "/__p__") is not None}
    for src in sources:
        rel = mozpath.relpath(mozpath.normsep(src), mozpath.normsep(top))
        out = os.path.join(src_out, *mozpath.split(rel))
        os.makedirs(os.path.dirname(out), exist_ok=True)
        shutil.copy2(src, out)
        nbytes += os.path.getsize(src)
    return len(sources), nbytes


def _collect_includes(path, top, acc):
    try:
        with open(path, "r", errors="replace") as fh:
            lines = fh.readlines()
    except OSError:
        return
    base = os.path.dirname(path)
    for line in lines:
        m = _INCLUDE_RE.match(line)
        if not m:
            continue
        inc = m.group(1)
        if inc.startswith("/"):
            cand = os.path.join(top, inc.lstrip("/"))
        else:
            cand = os.path.normpath(os.path.join(base, inc))
        if os.path.isfile(cand) and cand not in acc:
            acc.add(cand)
            _collect_includes(cand, top, acc)


def _vendor_python(topsrcdir, tools_dir):
    if os.path.isdir(tools_dir):
        shutil.rmtree(tools_dir)
    os.makedirs(tools_dir, exist_ok=True)

    mozbuild_src = os.path.join(topsrcdir, "python", "mozbuild")
    shutil.copytree(
        os.path.join(mozbuild_src, "mozpack"),
        os.path.join(tools_dir, "mozpack"),
        ignore=shutil.ignore_patterns("test", "__pycache__", "*.pyc"),
    )

    dst_mb = os.path.join(tools_dir, "mozbuild")
    os.makedirs(dst_mb, exist_ok=True)
    for name in MBLIBS:
        shutil.copy2(os.path.join(mozbuild_src, "mozbuild", name),
                     os.path.join(dst_mb, name))

    # third party libs that we defined earlier
    tp_src = os.path.join(topsrcdir, "third_party", "python")
    for name in TPLIBS:
        src = os.path.join(tp_src, name)
        if os.path.isdir(src):
            shutil.copytree(
                src,
                os.path.join(tools_dir, os.path.basename(name)),
                ignore=shutil.ignore_patterns("test", "tests", "__pycache__", "*.pyc"),
            )


_BUILD_PY = '''\
#!/usr/bin/env python3
# Build the brightwork omni.ja pair from this directory using this script.
# modify all metadata from metadata.json. ONLY USE THIS TOOL TO PACK.
# Hijacking this thing to make it do something else is not recommended
 
import argparse
import os
import sys

HERE = os.path.abspath(os.path.dirname(__file__))
sys.path.insert(0, os.path.join(HERE, "tools"))
for entry in sorted(os.listdir(os.path.join(HERE, "tools"))):
    p = os.path.join(HERE, "tools", entry)
    if os.path.isdir(p):
        sys.path.insert(0, p)

from mozpack.brightwork.build import build_from_recipe  # noqa: E402
from mozpack.brightwork.token import BrightworkToken  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", default=os.path.join(HERE, "dist"),
                    help="output directory (default: ./dist)")
    ap.add_argument("--metadata", default=os.path.join(HERE, "metadata.json"),
                    help="package metadata.json (default: ./metadata.json)")
    args = ap.parse_args()

    with open(args.metadata, "rb") as fh:
        token = BrightworkToken.from_metadata_bytes(fh.read())

    gre, skipped = build_from_recipe(
        os.path.join(HERE, "adk"),
        os.path.join(HERE, "src"),
        args.out,
        token,
        icon_source_dir=os.path.dirname(os.path.abspath(args.metadata)),
    )
    print("Built", gre)
    print("Built", os.path.join(args.out, "browser", "omni.ja"))
    print("Wrote", os.path.join(args.out, "brightwork.json"))
    if skipped:
        print("%d resource(s) not in the source tree (built only by full"
              " packaging; usually harmless):" % len(skipped))
        for dest, _ in skipped:
            print("   ", dest)


if __name__ == "__main__":
    main()
'''


def _readme():
    return """\
# My new addon
Update this if needed!

# Installation instructions
- On the add-ons manager, go to the brightwork section and install the package after building it or downloading it.
- You can do this by using the cog icon on the top and pressing the Brightwork-related options.
- If nothing failed, activate it and restart your browser!
- enjoi.
"""


def _write_driver(output_dir):
    from mozpack.brightwork.token import default_metadata_bytes

    with open(os.path.join(output_dir, "build.py"), "w") as fh:
        fh.write(_BUILD_PY)
    os.chmod(os.path.join(output_dir, "build.py"), 0o755)
    with open(os.path.join(output_dir, "README.md"), "w") as fh:
        fh.write(_readme())
    metadata_path = os.path.join(output_dir, "metadata.json")
    if not os.path.exists(metadata_path):
        with open(metadata_path, "wb") as fh:
            fh.write(default_metadata_bytes())
    with open(os.path.join(output_dir, ".gitignore"), "w") as fh:
        fh.write("/dist/\n__pycache__/\n*.pyc\n")


