#pragma once

#include <cstdint>
#include <string>

namespace Desert::Assets::Serialization
{
    /// Where an IMPORTED asset written as a file of its own (a .skeleton, a .anim) came from - UE's
    /// UAssetImportData on a USkeleton / UAnimSequence: the source's file name beside the asset and the hash of
    /// the bytes it was imported from (Assets::HashMeshSourceFile). The importer compares that hash, not file
    /// times, to decide whether a skinned source is current (AF8b) - so a fresh checkout, whose mtimes are the
    /// checkout's order, does not re-import committed assets - and Reimport of the asset re-imports THIS source,
    /// which rewrites every asset made from it (THM-FIXJ). Absent on a hand-authored asset (no source).
    struct ImportSourceInfo
    {
        std::string Source;
        uint64_t    SourceHash = 0;
    };
} // namespace Desert::Assets::Serialization
