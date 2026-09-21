#pragma once

#include <cstdint>
#include <string>

namespace Desert::Assets
{
    class AssetManager;
}

namespace Desert::Core::Serialize
{
    // THE ROUND TRIP OF A TEXTURE REFERENCE, as its own translation unit.
    //
    // These two functions are the halves of one relation — `SlotFromPath( SlotToPath( h ) ) == h`, for the
    // same file, on a machine that shares nothing with the one that wrote it — and they live here rather
    // than inside ComponentRegistry's resolver so that the relation can be ASSERTED. ComponentRegistry.cpp
    // reaches the ResourceRegistry and through it the whole renderer, so nothing in it is reachable by any
    // suite in the repository; the two branches this replaces were therefore never once executed by a
    // test, and all three defects they carried (an absolute machine-local path, a silent zero on a miss,
    // and — through the serializer above them — a handle rounded by a double) shipped that way.
    //
    // Deliberately dependency-light: AssetManager and TextureAsset only. Registering the resolved shell
    // with the TextureService stays at the call site, because that is the one step that needs the GPU
    // layer and it is not part of the round trip.

    // The string a `.desce` stores for `handle`, or "" when the handle is unset (0).
    //
    // The form is `Common::AssetHandle::StableKeyForPath` — the asset's place in the project behind its
    // root's tag, e.g. `cooked:Textures/T_Checker.tex`. Not the raw filepath, which is ABSOLUTE with a
    // project open and so writes a developer's home directory into a committed file; and not simply
    // relative to the assets root the way a material's path is, because a cooked texture lives under
    // COOKED_PATH, a SIBLING of the assets root, where that reduction yields `../Cooked/...` and falls
    // back to absolute anyway. The tag is the bit no plain path can carry.
    //
    // A handle the cooked asset registry has no row for returns "" and LOGS why (DC §1.4): the slot is
    // about to be saved as empty, which is indistinguishable from an empty slot, so the next save loses
    // it.
    //
    // IT TAKES NO AssetManager SINCE T2.4, and the removal is the point rather than tidiness. The old
    // signature said, in the type system, that a texture reference could only be written down if the
    // registry happened to hold the asset — and the only thing that held assets wholesale was the boot's
    // directory walk. `ContentRegistry::KeyForHandle` answers from a file instead, so the round trip no
    // longer depends on what a session has loaded.
    std::string TextureSlotToPath( uint64_t handle );

    // The handle for a stored string, or 0 when it is empty or nothing resolves.
    //
    // Accepts every spelling: the tagged key above, a plain relative path, and an absolute path to a file
    // outside the project (`PathForStableKey` returns an untagged string unchanged). Creates the asset
    // when the manager does not already have it. A miss is logged with the name, its expansion and the
    // roots searched, never returned as a bare 0.
    //
    // THE CREATE-ON-MISS IS CREATE-ON-MISS AGAIN, and that sentence is a correction. It used to double as
    // the real lookup — `AssetManager::CreateAsset` deduplicated on the spelling-independent stable key
    // while `FindByPath` compared filepaths verbatim, so routing through the create was how this function
    // resolved a texture registered under another spelling. That was one of three hand-written detours
    // around one defect, and the registry now answers both questions the same way
    // (Desert/Tests/Engine/AssetPathIdentity), so the lookup below is the lookup and the create below is
    // a create. The behaviour that changed here: a stored reference whose expansion does not exist ON
    // DISK but whose texture IS registered under a different spelling used to return 0 and log a miss;
    // it now resolves.
    uint64_t TextureSlotFromPath( Assets::AssetManager& manager, const std::string& stored );
} // namespace Desert::Core::Serialize
