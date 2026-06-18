# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

"""Compare a reference omni.ja against a brightwork-built one.

A brightwork package is platform-specific: it carries the exact set of resources
that the build it was exported from produced. Building it for the wrong platform
(or from an incomplete ADK) yields an omni.ja that *opens fine* but is missing
files, which shows up as visual glitches rather than a crash (e.g. a Linux-
exported package run on Windows loses browser-tabsintitlebar.js / browser-aero.css
and the title bar disappears).

This tool diffs the entry sets so missing/extra resources are obvious. Use it to
sanity-check a package against the platform's stock omni.ja before shipping.

Usage:
    python -m mozpack.brightwork.compare REFERENCE CANDIDATE

REFERENCE/CANDIDATE may each be either a single omni.ja file, or a directory
containing `omni.ja` (and optionally `browser/omni.ja`) -- in the directory form
both the GRE and APP jars are compared as a pair.
"""

import os
import sys

import mozpack.path as mozpath
from mozpack.mozjar import JarReader


def omnijar_entries(path):
    """Return {entry_name: uncompressed_size} for an omni.ja."""
    reader = JarReader(path)
    sizes = {}
    for name, entry in reader.entries.items():
        sizes[name] = entry["uncompressed_size"]
    return sizes


def diff_entries(ref, cand):
    """Diff two {name: size} maps. Returns a dict describing the differences."""
    ref_names = set(ref)
    cand_names = set(cand)
    missing = sorted(ref_names - cand_names)
    extra = sorted(cand_names - ref_names)
    changed = sorted(
        n for n in (ref_names & cand_names) if ref[n] != cand[n]
    )
    return {
        "ref_count": len(ref),
        "cand_count": len(cand),
        "ref_bytes": sum(ref.values()),
        "cand_bytes": sum(cand.values()),
        "missing": missing,
        "extra": extra,
        "changed": changed,
    }


def _group_by_top(names):
    """Group entry names by their first two path segments, with counts."""
    groups = {}
    for n in names:
        parts = mozpath.split(n)
        key = "/".join(parts[:2]) if len(parts) > 1 else parts[0]
        groups[key] = groups.get(key, 0) + 1
    return sorted(groups.items(), key=lambda kv: (-kv[1], kv[0]))


def _resolve_pair(path):
    """Resolve an arg to a list of (label, gre_path, app_path_or_None)."""
    if os.path.isdir(path):
        gre = os.path.join(path, "omni.ja")
        app = os.path.join(path, "browser", "omni.ja")
        if not os.path.isfile(gre):
            raise SystemExit("no omni.ja under %s" % path)
        return (gre, app if os.path.isfile(app) else None)
    return (path, None)


def _print_diff(label, ref_path, cand_path, sample):
    ref = omnijar_entries(ref_path)
    cand = omnijar_entries(cand_path)
    d = diff_entries(ref, cand)

    def mib(n):
        return n / (1024 * 1024)

    print("=== %s ===" % label)
    print("  reference: %s" % ref_path)
    print("  candidate: %s" % cand_path)
    print(
        "  entries:   reference %d (%.1f MiB)  vs  candidate %d (%.1f MiB)"
        % (d["ref_count"], mib(d["ref_bytes"]), d["cand_count"], mib(d["cand_bytes"]))
    )
    print(
        "  missing from candidate: %d   extra in candidate: %d   "
        "size-changed: %d" % (len(d["missing"]), len(d["extra"]), len(d["changed"]))
    )
    if d["missing"]:
        print("  -- MISSING (in reference, absent from candidate), by area:")
        for key, n in _group_by_top(d["missing"]):
            print("       %5d  %s/" % (n, key))
        if sample:
            print("     sample of missing files:")
            for n in d["missing"][:sample]:
                print("       - %s" % n)
    if d["extra"]:
        print("  -- EXTRA (in candidate, not in reference): %d" % len(d["extra"]))
        if sample:
            for n in d["extra"][:sample]:
                print("       + %s" % n)
    print()
    return len(d["missing"])


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    sample = 20
    if "--full" in argv:
        argv.remove("--full")
        sample = 10**9
    if len(argv) != 2:
        print(__doc__)
        return 2

    ref_arg, cand_arg = argv
    ref_gre, ref_app = _resolve_pair(ref_arg)
    cand_gre, cand_app = _resolve_pair(cand_arg)

    total_missing = _print_diff("GRE omni.ja", ref_gre, cand_gre, sample)
    if ref_app and cand_app:
        total_missing += _print_diff("APP (browser) omni.ja", ref_app, cand_app, sample)
    elif ref_app or cand_app:
        print(
            "note: one side has a browser/omni.ja and the other does not; "
            "compared GRE jars only.\n"
        )

    if total_missing:
        print(
            "RESULT: candidate is missing %d file(s) the reference ships. "
            "If the reference is the stock omni.ja for the platform you run on, "
            "those gaps explain missing UI/visual glitches.\n"
            "If the platforms differ, re-export the ADK on the target platform "
            "(a brightwork package is platform-specific)." % total_missing
        )
        return 1
    print("RESULT: candidate contains every entry the reference does. ✓")
    return 0


if __name__ == "__main__":
    sys.exit(main())
