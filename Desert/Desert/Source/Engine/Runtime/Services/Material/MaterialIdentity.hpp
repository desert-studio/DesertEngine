#pragma once

// The rule MaterialService applies when a second material claims a handle it already holds.
//
// It lives in a header of its own, with no include beyond <filesystem>, so a test can reach the
// DECISION: MaterialService itself cannot be linked into one, because MaterialService pulls in the
// renderer and the renderer pulls in Vulkan. Extracting the rule is what makes it reachable — the file
// around it stays where it was.

#include <filesystem>

namespace Desert::Runtime
{
    // Is `incoming` a SECOND file claiming an identity `existing` already holds?
    //
    // A material's asset handle IS the `MaterialId` written inside its `.demat` (SurfaceMaterialAsset::
    // AdoptStableHandle), so two files carrying the same number are two materials wearing one identity,
    // and every map keyed by that identity can hold only one of them. `MP_LitConst.demat` and
    // `MP_HandUnlit.demat` both carried 6666666666666666666. The symptom was not a missing material: it
    // was the Material Editor opening SOMEBODY ELSE'S asset, because a document is addressed by handle and
    // AssetManager::m_HandleLookup is a last-write-wins map.
    //
    // Compared LEXICALLY, and that is exact rather than approximate: AssetManager::CreateAsset
    // deduplicates on Common::AssetHandle::StableKeyForPath, which reduces every spelling of one file to
    // one project-relative key, so one file has exactly one asset record and can only ever present ONE
    // filepath here. Two different strings therefore mean two different files, and
    // std::filesystem::equivalent would stat a path on every registration to answer a question that
    // cannot arise.
    //
    // A material with no file behind it (a runtime-built shell) is never called a collision: there would
    // be nothing to name in the message, and the caller would be refused over a duplicate it cannot go and
    // look at.
    inline bool IsMaterialIdentityCollision( const std::filesystem::path& existing,
                                             const std::filesystem::path& incoming )
    {
        if ( existing.empty() || incoming.empty() )
            return false;

        // generic_string() rather than native(): native() is a WIDE string on Windows and a narrow one
        // here, so comparing the two forms only compiles on this platform. lexically_normal() first, so
        // `./Materials/X.demat` and `Materials/X.demat` are the one file they are.
        return existing.lexically_normal().generic_string() != incoming.lexically_normal().generic_string();
    }
} // namespace Desert::Runtime
