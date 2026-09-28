#pragma once

#include <Common/Core/AssetHandle.hpp>

#include <algorithm>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

// WHAT THE SPLASH WARMS, AND WHEN A CAPTURE MAY GO (THUMB3).
//
// The owner's complaint was two facts at once: entering a folder of materials stalled the editor, and the
// pictures of what the open scene USES were not there when the editor appeared. UE answers the second by
// rendering thumbnails of what is on screen first and never stalling a frame for one; this header holds the
// two rules that make that answer here, free of the device so a suite can hold them:
//
//   * SceneWarmList — which pictures the splash warms: the thumbnail subjects among the scene's asset roots
//     (Core::CollectAssetRoots, the same set eviction keeps resident), by path.
//   * MayDispatch — when the capture queue may start the next capture: at most one in flight (the renderer
//     has one slot), only when the main-thread budget has been repaid (ThumbnailEncode::CaptureBudget), and
//     before the hand-over only for a scene-warm request.
namespace Desert::Editor::ThumbnailWarmup
{
    /// A file whose thumbnail is a RENDERED capture the splash can warm: a surface material. Meshes are
    /// photographed from their cooked form, which a scene root already is, but resolving one builds it on
    /// the main thread (ThumbnailSubject::ResolveMesh) — the kind of work the splash must not add; their cached
    /// pictures still arrive through the browser's own prefetch. Textures are their own picture.
    [[nodiscard]] inline bool IsWarmSubject( const std::filesystem::path& path )
    {
        return path.extension() == ".demat";
    }

    /// The scene's thumbnail subjects, by path, sorted and unique. @p roots = the handles the scene references
    /// (Core::CollectAssetRoots(...).Handles()); @p pathFor = handle -> the file (Common::AssetPathIndex::
    /// PathFor), empty for a handle no file names — such a root has no picture to warm and is skipped.
    [[nodiscard]] inline std::vector<std::string>
    SceneWarmList( const std::vector<Common::AssetHandle>&                                   roots,
                   const std::function<std::filesystem::path( const Common::AssetHandle& )>& pathFor )
    {
        std::vector<std::string> out;
        for ( const Common::AssetHandle& handle : roots )
        {
            const std::filesystem::path path = pathFor( handle );
            if ( !path.empty() && IsWarmSubject( path ) )
                out.push_back( path.generic_string() );
        }
        std::sort( out.begin(), out.end() );
        out.erase( std::unique( out.begin(), out.end() ), out.end() );
        return out;
    }

    enum class CaptureScope
    {
        Everything,    // after the hand-over: the queue in order
        SceneWarmOnly, // on the splash: only what the open scene uses, and only while it is at the front
    };

    /// Whether the capture queue may dispatch its front request this frame. One capture at a time is the
    /// renderer's single slot; @p budgetAllows is CaptureBudget::MayDispatch, which spreads the captures'
    /// main-thread cost over frames. Called once per frame, so at most one capture starts per frame.
    [[nodiscard]] constexpr bool MayDispatch( bool budgetAllows, bool captureInFlight, CaptureScope scope,
                                              bool frontIsSceneWarm )
    {
        return budgetAllows && !captureInFlight && ( scope == CaptureScope::Everything || frontIsSceneWarm );
    }
} // namespace Desert::Editor::ThumbnailWarmup
