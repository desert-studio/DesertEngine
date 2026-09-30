#pragma once

#include <Common/Content/ContentScan.hpp>

#include <Common/Content/ContentKinds.hpp>
#include <Common/Core/AssetHandle.hpp>

#include <Editor/Panels/FileExplorer/FileType.hpp>
#include <Editor/Widgets/ThumbnailProducers.hpp>

#include <algorithm>
#include <cctype>
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
//     set eviction keeps resident), materials and meshes, by path; ProjectWarmList is EVERY picture of the
//     project, read from the content registry (THM1n-13, owner 09-29 "all assets on the splash"), and
//     SplashWarmList orders the two into the captures the splash runs.
//   * MayDispatch — when the capture queue may start the next capture: at most one in flight (the renderer
//     has one slot), only when the main-thread budget has been repaid (ThumbnailEncode::CaptureBudget), and
//     before the hand-over only for a scene-warm request.
namespace Desert::Editor::ThumbnailWarmup
{
    /// How one picture of the project comes to exist — ThumbnailProducers' producers that make a picture.
    /// Every kind but Decoded is a capture when the picture on disk is missing or stale.
    enum class WarmKind
    {
        Material, // a surface material, photographed on its preview (ThumbnailSubject::ResolveMaterial)
        Mesh,     // a static mesh, photographed from its cooked form (ThumbnailSubject::ResolveMesh)
        Pose,     // a skinned mesh in its bind pose (ThumbnailPose::ResolveSkinnedMesh)
        Painted,  // painted on a worker from the asset's own bytes (ThumbnailService::WarmPainted)
        Sky,      // a skybox, its sky drawn under the dome camera (ThumbnailService::WarmSkybox)
        Decoded,  // the file IS the picture (a texture): decoded, never captured
    };

    /// One picture to warm: the file the browser tile or scene root names, and how it is photographed.
    struct WarmItem
    {
        std::string Path;
        WarmKind    Kind = WarmKind::Material;

        friend bool operator==( const WarmItem&, const WarmItem& ) = default;
    };

    /// The warm kind of a browser file type, through ThumbnailProducers' table — the one place that says how
    /// a kind gets its picture; nullopt for a kind with no picture (type icon, not yet produced).
    [[nodiscard]] constexpr std::optional<WarmKind> WarmKindOfType( FileType type )
    {
        using ThumbnailProducers::Producer;
        switch ( ThumbnailProducers::ProducerOf( type ).value_or( Producer::TypeIcon ) )
        {
            case Producer::RenderedMaterial:
                return WarmKind::Material;
            case Producer::RenderedMesh:
                return WarmKind::Mesh;
            case Producer::RenderedPose:
                return WarmKind::Pose;
            case Producer::Painted:
                return WarmKind::Painted;
            case Producer::Decoded:
                return WarmKind::Decoded;
            case Producer::RenderedSky:
                return WarmKind::Sky;
            case Producer::NotYetProduced:
            case Producer::TypeIcon:
                return std::nullopt;
        }
        return std::nullopt;
    }

    /// The file type of a file of content kind @p kind, as the browser types it (FileTypeOfContent: the
    /// extension without the dot, and the kind — a skybox `.detex` is a Skybox, not a texture).
    [[nodiscard]] inline FileType FileTypeOfRow( const std::filesystem::path&                path,
                                                 std::optional<Common::Content::ContentKind> kind )
    {
        std::string extension = path.extension().string();
        if ( !extension.empty() )
            extension.erase( 0, 1 );
        std::transform( extension.begin(), extension.end(), extension.begin(),
                        []( unsigned char c ) { return static_cast<char>( std::tolower( c ) ); } );
        return FileTypeOfContent( extension, kind );
    }

    /// The file type of a file the caller knows only by path: its kind is the census's (KindOfContentFile).
    [[nodiscard]] inline FileType FileTypeOfPath( const std::filesystem::path& path )
    {
        return FileTypeOfRow( path, Common::Content::KindOfContentFile( path ) );
    }

    /// EVERY PICTURE OF THE PROJECT (THM1n-13): each row of the content registry, of every kind, whose file
    /// type has a picture producer — sorted by path, each once. Each row is typed by the kind the registry
    /// FILED it under (FileTypeOfRow), not re-derived from its path: a skybox row is a sky picture. @p filesOf =
    /// ContentRegistry::FilesOfKind in the editor, a table in a test. The registry and not a walk of the disk: it
    /// is the one census of what is content, and it is complete before the splash asks (ContentRegistry::Gather).
    /// A redirector is a forwarding stub at a moved asset's old path, spelled with the asset's own extension — the
    /// asset has its row and its picture under its new path, so the stub is not asked for one.
    [[nodiscard]] inline std::vector<WarmItem> ProjectWarmList(
         const std::function<std::vector<std::filesystem::path>( Common::Content::ContentKind )>& filesOf )
    {
        using Common::Content::ContentKind;
        std::vector<WarmItem> out;
        for ( std::size_t index = 0; index < Common::Content::CONTENT_KIND_COUNT; ++index )
        {
            const auto kind = static_cast<ContentKind>( index );
            if ( kind == ContentKind::Redirector )
                continue;
            for ( const std::filesystem::path& file : filesOf( kind ) )
            {
                if ( const std::optional<WarmKind> warm = WarmKindOfType( FileTypeOfRow( file, kind ) ) )
                    out.push_back( { file.generic_string(), *warm } );
            }
        }
        const auto byPath = []( const WarmItem& a, const WarmItem& b ) { return a.Path < b.Path; };
        std::sort( out.begin(), out.end(), byPath );
        out.erase( std::unique( out.begin(), out.end() ), out.end() );
        return out;
    }

    /// A CONTENT KIND WITH FILES AND NO PICTURE PRODUCER (THM-FIXB): what ProjectWarmList passed over, by kind,
    /// so the splash SAYS it (one line per kind, with the producer table's reason) instead of skipping silently.
    /// TypeIcon kinds are icons by design and not listed; NotYetProduced kinds are the debt the register names.
    struct Unproduced
    {
        Common::Content::ContentKind Kind  = Common::Content::ContentKind::Scene;
        std::size_t                  Files = 0;
        std::string_view             Why;
    };

    [[nodiscard]] inline std::vector<Unproduced> UnproducedKinds(
         const std::function<std::vector<std::filesystem::path>( Common::Content::ContentKind )>& filesOf )
    {
        using Common::Content::ContentKind;
        std::vector<Unproduced> out;
        for ( std::size_t index = 0; index < Common::Content::CONTENT_KIND_COUNT; ++index )
        {
            const auto kind = static_cast<ContentKind>( index );
            if ( kind == ContentKind::Redirector )
                continue;
            for ( const std::filesystem::path& file : filesOf( kind ) )
            {
                const FileType type = FileTypeOfRow( file, kind );
                if ( ThumbnailProducers::ProducerOf( type ) != ThumbnailProducers::Producer::NotYetProduced )
                    continue;
                if ( out.empty() || out.back().Kind != kind )
                {
                    std::string_view why;
                    for ( const ThumbnailProducers::Row& row : ThumbnailProducers::kTable )
                        if ( row.Type == type )
                            why = row.Why;
                    out.push_back( { kind, 0, why } );
                }
                ++out.back().Files;
            }
        }
        return out;
    }

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

    /// EVERY CAPTURE THE SPLASH RUNS, IN ORDER (THM1m, THM1n-13): the scene's subjects first (what the
    /// viewport shows), then the project's pictures (ProjectWarmList) that are missing or stale on disk.
    /// @p needsCapture answers "is this picture missing or out of date" — ThumbnailFreshness in the editor, a
    /// table in a test. A scene subject is kept whatever it answers (its own warm path skips a fresh one and
    /// still prefetches the PNG); a project picture the scene already names appears once, in the scene's
    /// place; a Decoded picture is its own file and never a capture. No time bound: the window waits for all
    /// of them (Splash::SceneCapturesHoldReveal).
    [[nodiscard]] inline std::vector<WarmItem>
    SplashWarmList( const std::vector<WarmItem>& scene, const std::vector<WarmItem>& project,
                    const std::function<bool( const WarmItem& )>& needsCapture )
    {
        std::vector<WarmItem> out = scene;
        for ( const WarmItem& tile : project )
        {
            if ( tile.Kind == WarmKind::Decoded || std::find( out.begin(), out.end(), tile ) != out.end() ||
                 !needsCapture( tile ) )
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
