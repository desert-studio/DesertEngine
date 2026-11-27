#!/usr/bin/env python3
"""sources_sync.py [--check] — the explicit source lists of the build (<dir>/Sources.cmake), written from git.

The build never globs first-party sources: every source directory carries a Sources.cmake that names its files
(relative to that directory), read by desert_sources() in cmake/DesertWorkspace.cmake. A file added or removed is
picked up by running this script and committing the list with the change; CMake re-configures only because a
Sources.cmake changed, never on a plain build (UBT's explicit module lists, not a CONFIGURE_DEPENDS glob).

The directories: every module FOLDER of cmake/DesertModuleTable.cmake that exists, the legacy engine root
Desert/Desert/Source, SOURCE_DIRS below (the own projects that are not module folders) and INPUT_DIRS (the
non-C++ inputs of a build step: the shaders DesertHeaderTool reads for push structs, its templates), each with its
own extensions. A directory nested in another listed one is cut out of the outer list. The files are the tracked
ones only (`git ls-files -c`) with the directory's extensions: a local untracked file never reaches a committed list
that other checkouts configure from (a new file is `git add`ed before the script runs).

Exit 0: lists written (or, with --check, every list equals the tree). 1: --check found a stale list (printed);
land_dev.sh refuses to land then.
"""
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TABLE = ROOT / "cmake" / "DesertModuleTable.cmake"
LEGACY_ROOT = "Desert/Desert/Source"
EXTENSIONS = (".cpp", ".hpp", ".h", ".inl", ".mm", ".c")
# The own projects whose directory is not a module FOLDER (their CMakeLists.txt calls desert_sources on it).
SOURCE_DIRS = [
    "Desert/Net/Source",
    "Desert/Tests",
    "Editor/Source",
    "Runtime/Source",
    "Server/Source",
    "Tools/AssetClosure/Source",
    "Tools/AssetRegistryTool/Source",
    "Tools/CloudLayoutBaker/Source",
    "Tools/CloudVolumeBaker/Source",
    "Tools/CrashReporter/Source",
    "Tools/DShaderTool/Source",
    "Tools/DesertCtl/Source",
    "Tools/DesertHeaderTool",
    "Tools/DesertNetProbe/Source",
    "Tools/DomeSheet/Source",
    "Tools/FbxMeshSplitter",
    "Tools/GamePackager/Source",
    "Tools/ImageDiff/Source",
    "Tools/ImageStat/Source",
    "Tools/LatticePeak/Source",
    "Tools/LineJump/Source",
    "Tools/PakTool/Source",
    "Tools/SceneMigrator/Source",
    "Tools/ShaderProtocolGen/Source",
    "Tools/TextureCook/Source",
    "Tools/UIMockup/Source",
    "Tools/WorldCook/Source",
    "Tools/WorldGen/Source",
]
# The inputs of a build step that are not C++ sources, with the extensions the step reads: the custom command
# that reads them names every listed file as a dependency, so it re-runs exactly when one of them changes.
INPUT_DIRS = {
    "Editor/Resources/Shaders": (".shader", ".glslh"),
    "Tools/DesertHeaderTool/Templates": (".tpl",),
}
HEADER = (
    "# Written by scripts/Dev/sources_sync.py from git; never edited by hand. A file added or removed in this\n"
    "# directory: run `python scripts/Dev/sources_sync.py` and commit this list with the change (land_dev.sh\n"
    "# refuses a list that differs from the tree). Read by desert_sources() (cmake/DesertWorkspace.cmake).\n"
)


def module_folders() -> list[str]:
    """Every module's FOLDER (desert_module rows), the default Desert/<Name>/Source/<Name> when none is named."""
    folders = []
    for line in TABLE.read_text(encoding="utf-8").splitlines():
        m = re.match(r"\s*desert_module\((\w+)\b(.*)", line)
        if not m:
            continue
        named = re.search(r'FOLDER\s+"([^"]+)"', m.group(2))
        folders.append(named.group(1) if named else f"Desert/{m.group(1)}/Source/{m.group(1)}")
    if not folders:
        sys.exit(f"sources_sync: no desert_module row in {TABLE}")
    return folders


def list_dirs() -> list[str]:
    dirs = [d for d in module_folders() if (ROOT / d).is_dir()]
    dirs.append(LEGACY_ROOT)
    dirs.extend(d for d in SOURCE_DIRS if (ROOT / d).is_dir())
    dirs.extend(INPUT_DIRS)
    missing = [d for d in [*SOURCE_DIRS, *INPUT_DIRS] if not (ROOT / d).is_dir()]
    if missing:
        sys.exit(f"sources_sync: SOURCE_DIRS names directories that do not exist: {missing}")
    return sorted(set(dirs))


def tree_files() -> list[str]:
    out = subprocess.run(["git", "-C", str(ROOT), "ls-files", "-c"],
                         capture_output=True, text=True, check=True).stdout
    return [f for f in out.splitlines() if (ROOT / f).is_file()]


def render(files: list[str]) -> str:
    rows = "".join(f'    "{f}"\n' for f in files)
    return f"{HEADER}set(DESERT_SOURCES\n{rows})\n"


def main() -> int:
    check = "--check" in sys.argv[1:]
    dirs = list_dirs()
    files = tree_files()
    stale = []
    for d in dirs:
        prefix = d + "/"
        nested = [o + "/" for o in dirs if o != d and o.startswith(prefix)]
        extensions = INPUT_DIRS.get(d, EXTENSIONS)
        own = sorted(f[len(prefix):] for f in files if f.startswith(prefix) and f.endswith(extensions)
                     and not any(f.startswith(n) for n in nested))
        target = ROOT / d / "Sources.cmake"
        text = render(own)
        # A checkout with core.autocrlf holds the list with CRLF: the same list, not a stale one.
        old = target.read_bytes().decode("utf-8").replace("\r\n", "\n") if target.exists() else None
        if old == text:
            continue
        stale.append(str(target.relative_to(ROOT)).replace("\\", "/"))
        if not check:
            target.write_bytes(text.encode("utf-8"))
    if check and stale:
        print("sources_sync: these source lists differ from the tree (run python scripts/Dev/sources_sync.py):")
        for s in stale:
            print(f"  {s}")
        return 1
    if not check:
        print(f"sources_sync: {len(dirs)} lists, {len(stale)} rewritten")
    return 0


if __name__ == "__main__":
    sys.exit(main())
