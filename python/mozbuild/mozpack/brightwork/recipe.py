# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

import json
import os
import re
import shutil
from dataclasses import asdict, dataclass, field

import mozpack.path as mozpath
from mozpack.copier import FileCopier, FileRegistry
from mozpack.manifests import InstallManifest

RECIPE_VERSION = 2
RECIPE_FILENAME = "recipe.json"

# Toplevel directories that our standalone package needs to have bundled as
# resources inside omni.ja. These files must mirror what was stored in
# OmniJarSubFormatter in formats.py (mozpack/packager), since skipping these
# files will cause extraction to be incomplete and missing files on launch.
RESOURCE_TOPLEVEL = frozenset(
    [
        "modules",
        "moz-src",
        "actors",
        "dictionaries",
        "hyphenation",
        "localization",
        "default.locale",
        "contentaccessible",
    ]
)


# Marble ships for Windows and Linux only; anything else is out of scope.
def platform_from_defines(defines):
    d = defines or {}
    if d.get("XP_WIN") or str(d.get("OS_TARGET", "")).upper() in ("WINNT", "WIN64"):
        return "win"
    # Marble's only other target.
    return "linux"


@dataclass
class BrightworkRecipe:
    topsrcdir: str
    omnijar_name: str = "omni.ja"
    manifests: list = field(default_factory=list)
    non_resources: list = field(default_factory=list)
    defines: dict = field(default_factory=dict)
    # Which platform this ADK was exported for ("win" / "linux").
    platform: str = ""
    recipe_version: int = RECIPE_VERSION

    def to_json(self):
        return json.dumps(asdict(self), indent=2, sort_keys=True)

    @classmethod
    def from_json(cls, data):
        obj = json.loads(data)
        ver = obj.get("recipe_version")
        if ver != RECIPE_VERSION:
            raise ValueError(
                "Unsupported brightwork recipe version %r (expected %d)"
                % (ver, RECIPE_VERSION)
            )
        return cls(**obj)

    @classmethod
    def load(cls, adk_dir):
        with open(os.path.join(adk_dir, RECIPE_FILENAME)) as fh:
            return cls.from_json(fh.read())

    def save(self, adk_dir):
        os.makedirs(adk_dir, exist_ok=True)
        with open(os.path.join(adk_dir, RECIPE_FILENAME), "w") as fh:
            fh.write(self.to_json())


def is_resource_dest(dest):
    parts = mozpath.split(mozpath.normsep(dest))
    if parts and parts[0] == "browser":
        parts = parts[1:]
    if not parts:
        return False

    if parts[-1].endswith(".manifest") or parts[-1] == "greprefs.js":
        return True

    head = parts[0]
    if head == "chrome":
        return len(parts) == 1 or parts[1] != "icons"
    if head == "components":
        return parts[-1].endswith((".js", ".xpt"))
    if head == "res":
        return len(parts) == 1 or parts[1] not in (
            "cursors",
            "touchbar",
            "MainMenu.nib",
        )
    if head == "defaults":
        return len(parts) != 3 or not (
            parts[2] == "channel-prefs.js" and parts[1] in ("pref", "preferences")
        )
    return head in RESOURCE_TOPLEVEL


def rebase_source(source, old_root, new_root):
    source = mozpath.normsep(os.path.normpath(source))
    old_root = mozpath.normsep(os.path.normpath(old_root))
    if source == old_root:
        return new_root
    prefix = old_root.rstrip("/") + "/"
    if not source.startswith(prefix):
        return None
    return mozpath.join(new_root, source[len(prefix) :])

def export_recipe(buildconfig, output_dir, manifest_paths):
    os.makedirs(output_dir, exist_ok=True)
    substs = buildconfig.substs

    bundled = []
    manifests_dir = os.path.join(output_dir, "manifests")
    os.makedirs(manifests_dir, exist_ok=True)
    for src in manifest_paths:
        name = os.path.basename(src)
        shutil.copy2(src, os.path.join(manifests_dir, name))
        bundled.append(mozpath.join("manifests", name))

    recipe = BrightworkRecipe(
        topsrcdir=mozpath.normsep(buildconfig.topsrcdir),
        omnijar_name=substs.get("OMNIJAR_NAME", "omni.ja"),
        manifests=bundled,
        defines=dict(buildconfig.defines),
        platform=platform_from_defines(buildconfig.defines),
    )
    recipe.save(output_dir)
    _bundle_generated(buildconfig, output_dir, recipe)
    return recipe


GENERATED_DIRNAME = "generated"


def _source_stageable_dests(adk_dir, recipe):
    """Return the set of dests the consumer can reproduce from the shipped src.

    Used by _bundle_generated to decide which dist/bin resources must be captured
    into generated/ (i.e. everything the consumer CANNOT reproduce). Correctness
    hinges on this set being precise:

    - LINK/COPY whose source lives under topsrcdir -> reproducible (in src).
    - CONTENT -> reproduced inline from the manifest itself.
    - PATTERN_* -> reproducible only for the *actual files the pattern matches*
      under its source base. We expand the pattern with the same FileFinder the
      consumer uses (_replay_manifest) rather than treating the whole dest_base
      as a prefix -- a coarse prefix would wrongly mark unrelated files under
      that dir (e.g. a preprocessed modules/policies/schema.sys.mjs sitting under
      a browser/modules/ pattern) as reproducible and skip bundling them.
    - PREPROCESS -> intentionally excluded. A preprocessed file routinely
      #includes generated objdir headers (e.g. AppConstants.sys.mjs pulls in
      @TOPOBJDIR@/brightwork-abi.h) absent from src, so it is never reproducible
      on the consumer; its built output must be bundled from dist/bin.
    """
    from mozpack.files import FileFinder

    reproducible = set()
    for rel in recipe.manifests:
        manifest = InstallManifest(path=os.path.join(adk_dir, rel))
        for dest, entry in manifest._dests.items():
            itype = entry[0]
            if itype in (InstallManifest.LINK, InstallManifest.COPY):
                under = rebase_source(entry[1], recipe.topsrcdir, "/__p__")
                if under is not None and os.path.isfile(entry[1]):
                    reproducible.add(mozpath.normsep(dest))
            elif itype in (InstallManifest.PATTERN_LINK, InstallManifest.PATTERN_COPY):
                _, base, pattern, dest_base = entry
                under = rebase_source(base, recipe.topsrcdir, "/__p__")
                if under is not None and os.path.isdir(base):
                    for found, _ in FileFinder(base).find(pattern):
                        reproducible.add(
                            mozpath.normsep(mozpath.join(dest_base, found))
                        )
            elif itype == InstallManifest.CONTENT:
                # Inline content is reproduced from the manifest itself
                reproducible.add(mozpath.normsep(dest))
    return reproducible


def _added_dir_map(adk_dir, recipe):
    from collections import defaultdict

    top = recipe.topsrcdir
    rel_dests = defaultdict(set)
    staged_rels = set()
    for relm in recipe.manifests:
        manifest = InstallManifest(path=os.path.join(adk_dir, relm))
        for dest, entry in manifest._dests.items():
            if entry[0] not in (
                InstallManifest.LINK,
                InstallManifest.COPY,
                InstallManifest.PREPROCESS,
            ):
                continue
            src = mozpath.normsep(entry[1])
            if not src.startswith(top + "/"):
                continue
            rel = src[len(top) + 1 :]
            staged_rels.add(rel)
            rel_dests[mozpath.dirname(rel)].add(
                mozpath.dirname(mozpath.normsep(dest))
            )
    dir_map = {d: next(iter(v)) for d, v in rel_dests.items() if len(v) == 1}
    return dir_map, staged_rels


_ADDED_DENY_NAMES = frozenset(
    ["moz.build", "jar.mn", "jar.inc.mn", "thumbs.db", "desktop.ini"]
)
_ADDED_DENY_SUFFIXES = ("~", ".orig", ".rej", ".swp", ".bak", ".in")


def _is_ignorable_added(name):
    if name.startswith("."):
        return True
    low = name.lower()
    return low in _ADDED_DENY_NAMES or low.endswith(_ADDED_DENY_SUFFIXES)


def _nearest_mapped_dir(reldir, dir_map):
    """
    Puzzle solving for mapping jars into subdirectories incase a new subfolder
    gets added inside an already existing directory.

    Tries an exact mapping first (preserving the build's captured layout, incl.
    any flattening), then the nearest mapped ancestor. This is so a file in a
    brand-new subfolder lands under its parent's ja location with the new
    subpath preserved.
    """
    parts = mozpath.split(reldir) if reldir else []
    for i in range(len(parts), -1, -1):
        ancestor = "/".join(parts[:i])
        if ancestor in dir_map:
            return dir_map[ancestor], "/".join(parts[i:])
    return None


def _infer_added_files(adk_dir, recipe, src_root, registry, resources_only):
    """
    
    File-adding logic to allow including new files for the ja

    """
    from mozpack.files import File

    dir_map, staged_rels = _added_dir_map(adk_dir, recipe)
    added = 0
    for root, _dirs, files in os.walk(src_root):
        reldir = mozpath.relpath(mozpath.normsep(root), src_root)
        if reldir == ".":
            reldir = ""
        mapped = _nearest_mapped_dir(reldir, dir_map)
        if not mapped:
            continue
        dest_dir, subpath = mapped
        for name in files:
            if _is_ignorable_added(name):
                continue
            rel = mozpath.join(reldir, name) if reldir else name
            if rel in staged_rels:
                continue  # already placed by a manifest entry
            dest = mozpath.join(dest_dir, subpath, name) if subpath else mozpath.join(
                dest_dir, name
            )
            if resources_only and not is_resource_dest(dest):
                continue
            if registry.contains(dest):
                continue
            registry.add(dest, File(os.path.join(root, name)))
            added += 1
    return added


_NS_NAME_RE = re.compile(r"^[a-z0-9][a-z0-9_-]*$")


def apply_namespaces(adk_dir, src_root, staging):
    # Namespace registering:
    # this is basically used when we add it directly to chrome:// as a new namespace folder,
    # which means the list should upgrade to account all the files inside.

    import shutil

    recipe = BrightworkRecipe.load(adk_dir)
    _, staged_rels = _added_dir_map(adk_dir, recipe)
    known_top = {r.split("/", 1)[0] for r in staged_rels}
    src_root = os.path.abspath(src_root)
    if not os.path.isdir(src_root):
        return []
    browser_root = os.path.join(staging, "browser")

    lines = []
    registered = []
    for child in sorted(os.listdir(src_root)):
        if child in known_top:
            continue
        pkgdir = os.path.join(src_root, child)
        if not os.path.isdir(pkgdir):
            continue
        providers = [
            p
            for p in ("content", "skin", "locale")
            if os.path.isdir(os.path.join(pkgdir, p))
        ]
        if not providers:
            continue  # not a chrome package
        if not _NS_NAME_RE.match(child):
            print(
                "brightwork: skipping namespace %r -- not a valid chrome "
                "package name ([a-z0-9_-])" % child
            )
            continue
        for prov in providers:
            src_prov = os.path.join(pkgdir, prov)
            if prov == "locale":
                for loc in sorted(os.listdir(src_prov)):
                    loc_dir = os.path.join(src_prov, loc)
                    if not os.path.isdir(loc_dir):
                        continue
                    dest = os.path.join(
                        browser_root, "chrome", child, "locale", loc
                    )
                    os.makedirs(dest, exist_ok=True)
                    shutil.copytree(loc_dir, dest, dirs_exist_ok=True)
                    lines.append(
                        "locale %s %s chrome/%s/locale/%s/"
                        % (child, loc, child, loc)
                    )
            else:
                dest = os.path.join(browser_root, "chrome", child, prov)
                os.makedirs(dest, exist_ok=True)
                shutil.copytree(src_prov, dest, dirs_exist_ok=True)
                if prov == "skin":
                    lines.append(
                        "skin %s classic/1.0 chrome/%s/skin/" % (child, child)
                    )
                else:
                    lines.append("content %s chrome/%s/content/" % (child, child))
        registered.append(child)

    if lines:
        manifest_path = os.path.join(browser_root, "chrome.manifest")
        with open(manifest_path, "a") as fh:
            fh.write("\n# Brightwork custom namespaces\n")
            for line in lines:
                fh.write(line + "\n")
    return registered


def _bundle_generated(buildconfig, adk_dir, recipe):
    from mozpack.files import FileFinder

    distbin = os.path.join(buildconfig.topobjdir, "dist", "bin")
    gen_root = os.path.join(adk_dir, GENERATED_DIRNAME)
    reproducible = _source_stageable_dests(adk_dir, recipe)

    count = 0
    finder = FileFinder(distbin, ignore_broken_symlinks=True)
    for dest, _ in finder.find("**"):
        dest = mozpath.normsep(dest)
        if not is_resource_dest(dest):
            continue
        if dest in reproducible:
            continue  # the consumer's source tree can produce this exactly
        src = os.path.join(distbin, *mozpath.split(dest))
        if not os.path.isfile(src):
            continue
        out = os.path.join(gen_root, *mozpath.split(dest))
        os.makedirs(os.path.dirname(out), exist_ok=True)
        shutil.copy2(src, out)
        count += 1
    return count


def stage_from_recipe(adk_dir, src_root, staging_dir, resources_only=True):
    recipe = BrightworkRecipe.load(adk_dir)
    src_root = mozpath.normsep(os.path.abspath(src_root))
    generated_root = os.path.join(adk_dir, GENERATED_DIRNAME)

    registry = FileRegistry()
    skipped = []
    for rel in recipe.manifests:
        manifest = InstallManifest(path=os.path.join(adk_dir, rel))
        _replay_manifest(
            manifest, recipe, src_root, generated_root, registry, skipped,
            resources_only,
        )

    if os.path.isdir(generated_root):
        from mozpack.files import File, FileFinder

        for dest, _ in FileFinder(generated_root).find("**"):
            dest = mozpath.normsep(dest)
            if not registry.contains(dest):
                registry.add(dest, File(os.path.join(generated_root,
                                                     *mozpath.split(dest))))

    _infer_added_files(adk_dir, recipe, src_root, registry, resources_only)

    # drop entries that the generated bundle ultimately resolved 
    reg_paths = set(dest for dest, _ in registry)
    truly_skipped = []
    for dest, source in skipped:
        d = mozpath.normsep(dest)
        prefix = d.rstrip("/") + "/"
        if d in reg_paths or any(p.startswith(prefix) for p in reg_paths):
            continue
        truly_skipped.append((dest, source))

    copier = FileCopier()
    for dest, f in registry:
        copier.add(dest, f)
    copier.copy(staging_dir)

    return registry, truly_skipped


def _generated_fallback(generated_root, dest):
    candidate = os.path.join(generated_root, *mozpath.split(dest))
    return candidate if os.path.isfile(candidate) else None


def _preprocess_or_none(path, marker, defines, silence):
    import io

    from mozbuild.preprocessor import Preprocessor

    try:
        pp = Preprocessor(defines=defines, marker=marker)
        pp.setSilenceDirectiveWarnings(silence)
        out = io.StringIO()
        with open(path, "r", encoding="utf-8") as inp:
            pp.processFile(input=inp, output=out)
        return out.getvalue().encode("utf-8")
    except Exception:
        return None


def _replay_manifest(manifest, recipe, src_root, generated_root, registry,
                     skipped, resources_only):
    from mozpack.files import File, FileFinder, GeneratedFile

    for dest in sorted(manifest._dests):
        if resources_only and not is_resource_dest(dest):
            continue
        entry = manifest._dests[dest]
        install_type = entry[0]

        if install_type in (InstallManifest.LINK, InstallManifest.COPY):
            rebased = rebase_source(entry[1], recipe.topsrcdir, src_root)
            if rebased is None or not os.path.exists(rebased):
                gen = _generated_fallback(generated_root, dest)
                if gen:
                    registry.add(dest, File(gen))
                else:
                    skipped.append((dest, entry[1]))
                continue
            registry.add(dest, File(rebased))
        elif install_type == InstallManifest.PREPROCESS:
            # Re-run the preprocessor from the shipped src so edits to a
            # preprocessed file -- and to the sources it #includes (e.g. editing
            # navigator-toolbox.inc.xhtml, pulled into browser.xhtml) -- take
            # effect on rebuild. If preprocessing can't be reproduced here (the
            # file #includes an objdir-only header such as
            # @TOPOBJDIR@/brightwork-abi.h that is absent from src), fall back to
            # the already-preprocessed copy captured into generated/ at export
            # time; failing that, skip (the caller guards startup-critical ones).
            data = None
            rebased = rebase_source(entry[1], recipe.topsrcdir, src_root)
            if rebased is not None and os.path.isfile(rebased):
                defines = dict(recipe.defines)
                defines.update(manifest._decode_field_entry(entry[4]))
                data = _preprocess_or_none(
                    rebased, entry[3], defines, bool(int(entry[5]))
                )
            if data is not None:
                registry.add(dest, GeneratedFile(data))
            else:
                gen = _generated_fallback(generated_root, dest)
                if gen:
                    registry.add(dest, File(gen))
                else:
                    skipped.append((dest, entry[1]))
        elif install_type == InstallManifest.CONTENT:
            content = manifest._decode_field_entry(entry[1]).encode("utf-8")
            registry.add(dest, GeneratedFile(content))
        elif install_type in (InstallManifest.PATTERN_LINK, InstallManifest.PATTERN_COPY):
            _, base, pattern, dest_base = entry
            rebased_base = rebase_source(base, recipe.topsrcdir, src_root)
            if rebased_base is None or not os.path.isdir(rebased_base):
                skipped.append((dest, base))
                continue
            finder = FileFinder(rebased_base)
            for found, _ in finder.find(pattern):
                fdest = mozpath.join(dest_base, found)
                if resources_only and not is_resource_dest(fdest):
                    continue
                registry.add(fdest, File(mozpath.join(rebased_base, found)))
