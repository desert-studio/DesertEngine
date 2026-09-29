#pragma once

#include <Common/Core/AssetHandle.hpp>

#include <algorithm>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

// WHAT THE SPLASH WARMS, AND WHEN A CAPTURE MAY GO (THUMB3).
//
// The owner's complaint was two facts at once: entering a folder of materials stalled the editor, and the
// pictures of what the open scene USES were not there when the editor appeared. UE answers the second by
// rendering thumbnails of what is on screen first and never stalling a frame for one; this header holds the
// two rules that make that answer here, free of the device so a suite can hold them:
//
//   * SceneWarmList — the thumbnail subjects among the scene's asset roots (Core::CollectAssetRoots, the same
//     set eviction keeps resident), materials and meshes, by path; SplashWarmList adds the opening folder's
//     tiles that have no fresh picture on disk (THM1m) — together, which pictures the splash warms.
//   * MayDispatch — when the capture queue may start the next capture: at most one in flight (the renderer
//     has one slot), only when the main-thread budget has been repaid (ThumbnailEncode::CaptureBudget), and
//     before the hand-over only for a scene-warm request.
namespace Desert::Editor::ThumbnailWarmup
{
    /// The two kinds of RENDERED capture the splash can warm. Textures are their own picture and cloud
    /// formats are painted on the CPU, so neither is ever a capture to warm.
    enum class WarmKind
    {
        Material, // a surface material, photographed on its preview (ThumbnailSubject::ResolveMaterial)
        Mesh,     // a static mesh, photographed from its cooked form (ThumbnailSubject::ResolveMesh)
        Pose,     // a skinned mesh in its bind pose (ThumbnailPose::ResolveSkinnedMesh)
    };

    /// One picture to warm: the file the browser tile or scene root names, and how it is photographed.
    struct WarmItem
    {
        std::string Path;
        WarmKind    Kind = WarmKind::Material;

        friend bool operator==( const WarmItem&, const WarmItem& ) = default;
    };

    /// Whether a scene root is a capture the splash warms, and which kind. THM1m: meshes too — resolving
    /// one no longer builds it on the main thread (ThumbnailSubject::ResolveMesh answers PENDING and reads a
    /// cold mesh on a worker, AL1-5c/THM1f), so a mesh costs the splash one capture, as a material does.
    [[nodiscard]] inline std::optional<WarmKind> WarmKindOf( const std::filesystem::path& path )
    {
        if ( path.extension() == ".demat" )
            return WarmKind::Material;
        if ( path.extension() == ".stmesh" )
            return WarmKind::Mesh;
        if ( path.extension() == ".skmesh" )
            return WarmKind::Pose;
        return std::nullopt;
    }

    /// The scene's thumbnail subjects, by path, sorted and unique. @p roots = the handles the scene references
    /// (Core::CollectAssetRoots(...).Handles()); @p pathFor = handle -> the file (Common::AssetPathIndex::
    /// PathFor), empty for a handle no file names — such a root has no picture to warm and is skipped.
    [[nodiscard]] inline std::vector<WarmItem>
    SceneWarmList( const std::vector<Common::AssetHandle>&                                   roots,
                   const std::function<std::filesystem::path( const Common::AssetHandle& )>& pathFor )
    {
        std::vector<WarmItem> out;
        for ( const Common::AssetHandle& handle : roots )
        {
            const std::filesystem::path path = pathFor( handle );
            if ( path.empty() )
                continue;
            if ( const std::optional<WarmKind> kind = WarmKindOf( path ) )
                out.push_back( { path.generic_string(), *kind } );
        }
        const auto byPath = []( const WarmItem& a, const WarmItem& b ) { return a.Path < b.Path; };
        std::sort( out.begin(), out.end(), byPath );
        out.erase( std::unique( out.begin(), out.end() ), out.end() );
        return out;
    }

    /// EVERYTHING THE SPLASH WARMS, IN ORDER (THM1m): the scene's subjects first (what the viewport shows),
    /// then the opening folder's tiles whose picture on disk is missing or stale (what the browser shows the
    /// moment the window appears). @p needsCapture answers "is this tile's picture missing or out of date" —
    /// ThumbnailFreshness in the editor, a table in a test. A scene subject is kept whatever it answers (its
    /// own warm path skips a fresh one and still prefetches the PNG); a folder tile the scene already
    /// names appears once, in the scene's place. The time this may add to the splash is not decided here:
    /// Splash::SceneCapturesHoldReveal bounds the wait, and what is not captured by then goes first after it.
    [[nodiscard]] inline std::vector<WarmItem>
    SplashWarmList( const std::vector<WarmItem>& scene, const std::vector<WarmItem>& folder,
                    const std::function<bool( const WarmItem& )>& needsCapture )
    {
        std::vector<WarmItem> out = scene;
        for ( const WarmItem& tile : folder )
        {
            if ( std::find( out.begin(), out.end(), tile ) != out.end() || !needsCapture( tile ) )
                continue;
            out.push_back( tile );
        }
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
