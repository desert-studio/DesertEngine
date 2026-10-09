#pragma once

#include <Editor/Panels/FileExplorer/FileType.hpp>
#include <Editor/Widgets/ThumbnailPrefetch.hpp>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Desert::Assets
{
    class AssetManager;
}

namespace Desert::Editor
{
    class ThumbnailCache;
    struct DirectoryInformation;

    /// THE CONTENT BROWSER'S PICTURES, WITHOUT THE WINDOW (F5; UE FAssetThumbnailPool,
    /// Editor/UnrealEd/Private/AssetThumbnail.cpp:1949–2702 — the pool owns the resident textures and the
    /// queue of what to load, the tiles only ask it). Here: the decoded-picture cache, what the workers were
    /// handed (the open folder's pictures only: THUMB-LAZY, nothing is warmed for the splash), and the
    /// per-path answers a tile needs before it can be drawn (the PNG name, the photographed mesh, the
    /// refusals). No ImGui: drawing a tile is AssetTileThumbnail's.
    class AssetThumbnailPool
    {
    public:
        explicit AssetThumbnailPool( Assets::AssetManager* assetManager );
        ~AssetThumbnailPool();
        AssetThumbnailPool( const AssetThumbnailPool& )            = delete;
        AssetThumbnailPool& operator=( const AssetThumbnailPool& ) = delete;

        /// The decoded pictures (GPU-resident). Never null. Named for what it holds: the ThumbnailRequesters
        /// census finds a drawing site by a `Get(` on a receiver whose name says thumbnail.
        [[nodiscard]] ThumbnailCache& Thumbnails() const
        {
            return *m_Thumbnails;
        }

        // Hand the pictures of @p folder's tiles that are not resident to ThumbnailPrefetch so a worker decodes
        // them before they are drawn; the request replaces the previous folder's. Mirrors the per-type branches
        // of the tile.
        void PrefetchFolder( const DirectoryInformation& folder );

        /// How many pictures are resident (ThumbnailCache::ResidentCount).
        [[nodiscard]] std::size_t ResidentThumbnails() const;

        // PER-TILE WORK THAT USED TO BE REDONE EVERY FRAME FOR EVERY TILE (THUMB3): the cache file name costs
        // a StableKeyForPath (std::filesystem::absolute). The name never changes for a path.
        const std::string& ThumbnailPngFor( const std::string& assetPath );

        // THE FILE A RenderedMesh TILE PHOTOGRAPHS: a model's own path (CookPaths::MeshAsset maps it to its
        // .stmesh), or the cooked mesh a .defoliage names (ThumbnailFoliage — UE: a foliage type's picture is
        // its mesh's). The foliage read is cached per path and file time; a refusal is logged once and
        // blacklisted.
        std::optional<std::string> MeshSourceFor( const std::string& assetPath, FileType type );

        // WHAT A RenderedMesh TILE SHOWS, by the asset its import wrote (THM-FIXB2): a raw source is pictured
        // by what its import record says it imports as — a StaticMesh by its .stmesh, a SkinnedMesh by its
        // .skmesh in its bind pose, a Skeleton by its .skeleton on its preview mesh; clips only, or a source
        // not imported yet, keep the type icon. Any other mesh file is its own cooked form. `Cooked` is the
        // picture's key and freshness source; `Pose` routes it to RequestPose.
        struct MeshPicture
        {
            std::string Cooked;
            bool        Pose = false;
        };
        std::optional<MeshPicture> MeshPictureFor( const std::string& assetPath, FileType type );

        // Assets whose picture was refused (load failed, no subject): the tile shows its icon, no retry spam.
        [[nodiscard]] bool IsRefused( const std::string& assetPath ) const
        {
            return m_FailedThumbs.contains( assetPath );
        }
        void Refuse( const std::string& assetPath )
        {
            m_FailedThumbs.insert( assetPath );
        }

        // A capture (Capture from viewport, an edited orbit) wrote @p png for @p assetPath: drop the cached
        // decode so the tile reloads, and forget an earlier refusal so the tile shows the captured picture.
        void OnCaptured( const std::string& png, const std::string& assetPath );

    private:
        Assets::AssetManager*           m_AssetManager = nullptr;
        std::unique_ptr<ThumbnailCache> m_Thumbnails;

        std::unordered_map<std::string, std::string> m_ThumbnailPngOf;
        std::unordered_set<std::string>              m_FailedThumbs;

        struct MeshSourceRead
        {
            std::filesystem::file_time_type Written;
            std::string                     Source;
        };
        std::unordered_map<std::string, MeshSourceRead> m_MeshSourceOf;

        // The import record read, cached per source and the record's file time (a re-import rewrites it).
        struct SourcePictureRead
        {
            std::filesystem::file_time_type Written;
            std::optional<MeshPicture>      Picture;
        };
        std::unordered_map<std::string, SourcePictureRead> m_SourcePictureOf;
    };
} // namespace Desert::Editor
