#pragma once

#include <Common/Core/ResultStr.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace Common
{
    // THE INVERSE OF ASSET IDENTITY: given the 64-bit number, say which file it names.
    //
    // WHY THIS HAS TO EXIST SOMEWHERE. `AssetHandle::FromCookedPath` is FNV-1a — one-way by
    // construction. Until now the only route back from a number to a path in this repository was
    // `Core::MakeAssetResolver`'s `ToPath`, and that route has two properties that make it the wrong
    // foundation for anything demand-driven:
    //
    //   * it asks the ASSET (`AssetManager::FindByHandle<T>`), so the answer needs the payload to be
    //     loaded and registered. A handle whose asset was evicted names nothing — the exact shape that
    //     cut the mesh->rig edge for a whole session when a skeleton's signature died with its load;
    //   * it is a hand-written table of twelve per-type branches that returns "" for a type nobody
    //     added a branch for. One missing branch is a reference silently saved as empty.
    //
    // This index has neither property. It is keyed on identity alone, it is filled at the moment the
    // identity is MINTED rather than by a walk somebody must remember to run, and it knows nothing
    // about asset types, so no type can be missing from it.
    //
    // WHAT IT STORES: the STABLE KEY (`assets:Textures/T.png`), not a path. Three reasons, and the
    // third is the load-bearing one:
    //   * the key is what the number was actually hashed from, so `KeyFor` is the literal inverse of
    //     the derivation rather than a reconstruction of it;
    //   * the key carries no machine's home directory, so an index dumped on one machine means the
    //     same thing on another (which is what T2.4's cooked registry will need);
    //   * `AssetHandle::PathForStableKey` is ALREADY the asserted exact inverse of `StableKeyForPath`
    //     (AssetHandleStability, `EveryRootsKeyExpandsBackToThePathItCameFrom`), so `PathFor` is that
    //     existing, tested edge composed with this one rather than a second answer to the same
    //     question that could drift from it.
    //
    // IT SURVIVES UNLOAD, AND THAT IS THE POINT. Nothing here ever touches a payload, so `Unload()`
    // cannot take a binding with it. `TextureAsset::Unload` already had to say in prose that "an
    // evicted asset keeps its identity"; this is the place where that sentence is true of the whole
    // engine instead of one class.
    //
    // WHAT IT IS NOT. It is not a cache of the filesystem and it does not make a handle resolvable
    // that nothing ever derived — a number read out of a `.desce` on a cold start still needs
    // SOMETHING to have hashed that path first. Inverting the hash is the precondition for the cooked
    // registry (GAP_ANALYSIS T2.4) that removes the directory walk; it is not that registry.
    namespace AssetPathIndex
    {
        // Binds `handle` to `stableKey`. Idempotent: re-recording the same pair is a success and does
        // not touch the table.
        //
        // COLLISION IS A REFUSAL, NOT A REPLACEMENT. Two different keys reaching one number means two
        // files share one identity, and whichever registered first is the only one any reference can
        // ever reach. Overwriting would make the winner depend on scan order — i.e. make the defect
        // move between runs. The first binding stands, both keys and the number are logged, and the
        // caller is told; `AssetReferenceCensus::NoTwoShippedContentFilesDeriveTheSameHandle` is the
        // same property asserted over the shipped corpus, and this is it enforced at run time for the
        // content that census cannot see (imported, generated, or from another project).
        //
        // An empty key or a null handle is refused: neither names anything, and recording one would
        // put a binding in the table that `PathFor` would have to special-case on the way out.
        [[nodiscard]] BoolResultStr Record( uint64_t handle, std::string_view stableKey );

        // The stable key `handle` was minted from, or empty if nothing ever minted it.
        std::string KeyFor( uint64_t handle );

        // The path `handle` names on THIS machine, or an empty path if nothing ever minted it.
        // Composed of `KeyFor` and `AssetHandle::PathForStableKey`, so a key that is an identity
        // rather than a location (`procedural://`, `memory://`) comes back verbatim, exactly as it
        // does through every other consumer of a stable key.
        std::filesystem::path PathFor( uint64_t handle );

        // How many distinct handles are bound. For tests and for the boot-cost measurement; there is
        // no consumer that branches on it.
        std::size_t Size();

        // Drops every binding. EXISTS FOR TESTS ONLY, and specifically for the ones that move the
        // project root underneath the index: a suite that re-roots the project mints the same relative
        // keys behind new absolute roots, and without a reset the second run's keys would be judged
        // against the first run's table. Production has no caller — an asset's identity outliving its
        // payload is the invariant this file is for, so clearing it at run time would reintroduce the
        // defect it removes.
        void Clear();
    } // namespace AssetPathIndex
} // namespace Common
