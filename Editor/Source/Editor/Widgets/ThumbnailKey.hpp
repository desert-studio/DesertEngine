#pragma once

#include <Common/Content/DerivedDataCache.hpp>
#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/Constants.hpp>

#include <cctype>
#include <cstdint>
#include <filesystem>
#include <string>

namespace Desert::Editor::ThumbnailKey
{
    /**
     * @brief Which cached thumbnail belongs to which asset.
     *
     * WHY THE ANSWER IS NOT "THE PATH THE CALLER HAPPENED TO HOLD". The key used to be the caller's own
     * spelling with every non-alphanumeric byte turned into '_', which made it a property of the MACHINE
     * rather than of the asset: the file this sentence was written next to was really called
     * `_Users_daniilsavcenko_Desktop_..._Editor_Resources_Assets_Materials_Starter_Prop_demat.png`.
     * Measured consequence, in the editor, in one `Cooked/Thumbnails` directory: opening one project
     * through two equivalent spellings of its own path (`<proj>/Editor` and a symlink to it) captured the
     * SAME three materials twice and left SIX files behind — three pictures nobody can invalidate,
     * because the panel that would invalidate them only knows one of the two names. Renaming the project
     * folder does the same thing to every thumbnail at once.
     *
     * So the key is the asset's identity, and this engine already has exactly one answer to "which asset
     * is this, whatever the spelling": `Common::AssetHandle::StableKeyForPath`, the project-relative,
     * root-tagged key the AssetManager deduplicates its registry on. Deriving the thumbnail key from the
     * same function is the point — a thumbnail is a picture OF an asset, so the two must not be able to
     * disagree about what an asset is. That is also why nothing here folds case: the identity key does
     * not, and a thumbnail key that case-folded while the asset handle did not would be a second, subtly
     * different notion of sameness — the exact two-keys-that-must-agree shape this replaces.
     *
     * A path under no content root keeps its normalized absolute spelling, because that is what
     * StableKeyForPath returns for a file genuinely outside the project: such a file has no
     * project-relative identity to give it, and inventing one here would make two projects' strays
     * collide.
     */
    inline std::string Identity( const std::string& assetPath )
    {
        return Common::AssetHandle::StableKeyForPath( assetPath );
    }

    /**
     * @brief The cache file name for an asset: a readable flattening of its identity, then the identity's
     *        own hash.
     *
     * The hash is not decoration. Flattening `assets:Materials/Wood/Oak.demat` for the filesystem loses
     * the difference between '/' and '_', so `Materials/Wood_Oak.demat` and `Materials/Wood/Oak.demat`
     * flatten to ONE name — two different assets sharing a cache entry, i.e. one of them showing the
     * other's picture with nothing able to tell them apart. Appending
     * `Common::AssetHandle::FromKey(identity)` restores injectivity. It hashes the identity string only,
     * so it is a property of the path, not of the asset: meshes and materials now carry a handle derived
     * from their header GUID, and the cache name deliberately does not follow it -- a thumbnail is a
     * picture of the file at this path.
     *
     * The readable half stays because it is what makes the cache inspectable from a shell — that is how
     * the absolute-path defect above was found.
     */
    inline std::string FileName( const std::string& assetPath )
    {
        const std::string identity = Identity( assetPath );

        std::string readable = identity;
        for ( char& c : readable )
        {
            if ( !std::isalnum( static_cast<unsigned char>( c ) ) )
                c = '_';
        }

        const std::uint64_t id = static_cast<std::uint64_t>( Common::AssetHandle::FromKey( identity ) );
        return readable + '_' + std::to_string( id ) + ".png";
    }

    // Bump whenever the thumbnail render path changes so all old thumbnails regenerate. v2: sky-IBL ambient
    // (old pre-IBL renders produced chrome/glass blobs that the source-modtime check never invalidated).
    // v3: output bumped 128 -> 256 px (128 looked low-res / "240p" when shown larger than 128 in the grid).
    // v4: PNG bumped to 1024 px ("hi-res on disk, box-averaged down for the small grid display" — a
    // decoupling that turned out to be pure waste; see v9 below).
    // v5: studio-gradient backdrop in the preview scene (was the dull default sky).
    // v7: existed for a WRONG PICTURE, not for a nicer one, which is why it was worth a forced re-render
    // of everybody's cache. FitTarget framed subjects against a hardcoded camera pose and an assumed
    // one-unit size; the centimetre migration made the preview sphere 100 units and moved EditorCamera to
    // eye height, so every thumbnail regenerated since then captured the flank of a 400-unit ball the
    // camera was resting on — mesh previews as well as materials (Д30).
    inline int CacheVersion()
    {
        // v8 IS NOT A PICTURE CHANGE. Every version before it says "the renderer improved, so the old
        // images are wrong"; this one says "the NAME the images are filed under changed" — DiskPath now
        // asks ThumbnailKey for the asset's project-relative identity instead of flattening whatever
        // spelling the caller held. The pixels a v8 capture produces are byte-for-byte the pixels v7
        // produced.
        //
        // It is still a bump, for the one reason a key change forces: every v7 file is now UNREACHABLE —
        // no path can hash to its name any more. Left at 7 they would sit in the current version's folder
        // forever, because PurgeOldVersions only deletes OTHER versions, so the cache would keep a
        // permanent layer of orphans that nothing reads and nothing removes. Bumping is what lets that
        // sweep collect them. Renaming them instead is not available: the old flattening is lossy, so the
        // path a v7 name came from cannot be recovered from the name.
        //
        // The cost is one re-render pass over the content tree, once, per developer — the same cost the
        // absolute-path key already charged every time anyone moved or symlinked their project.
        //
        // v9 IS a picture change, and the smallest kind: the same render at a different SIZE. The PNG is
        // written at 512 px instead of 1024 and rendered at 1024 instead of 2048, because 512 is
        // ThumbnailPixels::kMaxDim — the size this class uploads at and therefore the only size anything has ever
        // seen. A v8 file holds four times the pixels its own and only reader keeps, so they are not
        // "good enough to leave": each one costs 31 ms of PNG decode plus a box-average filter, on the
        // main thread inside the ImGui pass, once per session, to arrive at a picture a 512 px file hands
        // over directly. The capture that produced it cost 2823 ms against 410.
        //
        // The visible result is SHARPER, not softer, because ThumbnailPixels::kMaxDim went 256 -> 512 in the same
        // change: what a v8 grid drew was a 256 px texture stretched across a card up to 528 physical
        // pixels wide. See AssetThumbnailRenderer::kSize and ThumbnailPixels::kMaxDim for the
        // measurements and the arithmetic this rests on.
        return 9; // v9: the PNG is written at the size it is displayed at (512 px)
    }

    inline std::string DiskPath( const std::string& assetPath )
    {
        // THE NAME AND THE LOCATION NOW SIT TOGETHER, and the move is M11's. The location used to live
        // in ThumbnailCache.cpp — a translation unit that includes Engine/Graphic/Image.hpp and therefore
        // cannot be linked without a renderer. That was fine while the only callers were panels, and it
        // stopped being fine the moment the background sweep had to compute this path: the sweep's
        // decision — which files have no picture — is exactly the kind of thing this header exists to
        // keep reachable by a test rather than only by launching the editor. ThumbnailCache keeps what
        // genuinely needs the device: decoding a PNG into an Image2D.
        return ( Common::DDC::BucketDir( "Thumbnails" ) / ( "v" + std::to_string( CacheVersion() ) ) /
                 FileName( assetPath ) )
             .string();
    }
} // namespace Desert::Editor::ThumbnailKey
