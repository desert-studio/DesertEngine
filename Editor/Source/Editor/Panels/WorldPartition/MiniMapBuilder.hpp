#pragma once

// BUILD MINIMAP (WP-MAP-VIEW; UE UWorldPartitionMiniMapBuilder +
// FWorldPartitionMiniMapHelper::CaptureBoundsMiniMapToTexture): the saved world is walked one BuilderCellSize
// square at a time - each square's region is read from the world's files into an offscreen scene of its own
// (EditorRegions::ReadForOpen, every data layer loaded, as UE's builder loads its iterative cells and unloads them
// again), so the world is never in memory whole - and each square is looked at straight down through an
// orthographic camera. The G-buffer's albedo (the material's BaseColor before any light: UE's SCS_BaseColor
// capture) is read back and stitched into one image of the world rectangle at WorldUnitsPerPixel, saved beside the
// scene as `<stem>_MiniMap.detex` (sRGB colour). The capture is recorded into the editor's own frames (as
// LandscapeHLODBake's is), so a build runs over many frames: Tick() once per editor frame until it answers true.

#include <Engine/Core/Serialize/WorldPartitionFormat.hpp>

#include <Common/Core/ResultStr.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Desert::Graphic
{
    class SceneRenderer;
    class ImageReadback;
} // namespace Desert::Graphic

namespace Desert::Core
{
    class Scene;
    class GameplayCamera;
} // namespace Desert::Core

namespace Desert::Assets
{
    class AssetManager;
}

namespace Desert::Editor
{
    // The largest side of the minimap texture, in texels. A world wider than this at its WorldUnitsPerPixel is
    // refused by name, naming the WorldUnitsPerPixel it needs (UE clamps MiniMapSize the same way).
    inline constexpr uint32_t kMiniMapMaxSide = 8192;

    class MiniMapBuilder
    {
    public:
        // @p scenePath: the saved world. @p settings: the world rectangle to capture (MinX/MinZ/MaxX/MaxZ, cm,
        // already snapped to whole texels), WorldUnitsPerPixel and BuilderCellSize; its Texture is filled by the
        // build. @p dataLayers: the names of the world's data layers - every one is loaded for the capture.
        // Refused, by name, for a rectangle that is empty or wider than kMiniMapMaxSide texels, or settings that
        // are not positive and finite.
        [[nodiscard]] static Common::ResultStr<std::unique_ptr<MiniMapBuilder>>
        Start( std::filesystem::path scenePath, ::Desert::Core::WorldPartitionMiniMapSerialized settings,
               std::vector<std::string> dataLayers, const Assets::AssetManager* assets );

        ~MiniMapBuilder();
        MiniMapBuilder( const MiniMapBuilder& )            = delete;
        MiniMapBuilder& operator=( const MiniMapBuilder& ) = delete;

        // One editor frame of the build. True once the texture is written (Result()) or the build was refused
        // (Error()).
        [[nodiscard]] bool Tick();

        // The WorldPartition block's MiniMap the build made: the settings with the texture written.
        [[nodiscard]] const std::optional<::Desert::Core::WorldPartitionMiniMapSerialized>& Result() const
        {
            return m_Result;
        }
        [[nodiscard]] const std::optional<std::string>& Error() const
        {
            return m_Error;
        }
        [[nodiscard]] std::size_t TileCount() const
        {
            return m_Tiles.size();
        }
        [[nodiscard]] std::size_t TilesDone() const
        {
            return m_Next;
        }

        // `<dir>/<stem>_MiniMap.detex` of the scene file @p scenePath.
        [[nodiscard]] static std::filesystem::path TexturePath( const std::filesystem::path& scenePath );

    private:
        // One square of the world, in texels of the whole image: [X0, X1) x [Y0, Y1).
        struct Tile
        {
            uint32_t X0 = 0;
            uint32_t Y0 = 0;
            uint32_t X1 = 0;
            uint32_t Y1 = 0;
        };

        enum class Phase
        {
            Build,    // the renderer is built a stage per frame
            Load,     // the current tile's region is read into a fresh scene and framed
            Frame,    // warm-up frames are recorded until the tile's content has streamed in
            Readback, // the albedo copy is in flight
            Done,
        };

        MiniMapBuilder( std::filesystem::path scenePath, ::Desert::Core::WorldPartitionMiniMapSerialized settings,
                        std::vector<std::string> dataLayers, const Assets::AssetManager* assets, uint32_t width,
                        uint32_t height );

        [[nodiscard]] Common::BoolResultStr LoadTile();
        void                                ReleaseScene();
        void                                Blit( const std::vector<uint8_t>& rgba );
        [[nodiscard]] Common::BoolResultStr Save();
        void                                Fail( std::string error );

        std::filesystem::path                           m_ScenePath;
        ::Desert::Core::WorldPartitionMiniMapSerialized m_Settings;
        std::vector<std::string>                        m_DataLayers;
        const Assets::AssetManager*                     m_Assets = nullptr;
        uint32_t                                        m_Width  = 0; // the whole image, texels
        uint32_t                                        m_Height = 0;
        std::vector<uint8_t>                            m_Image; // sRGB RGBA8, row 0 along MinZ
        std::vector<Tile>                               m_Tiles;

        std::unique_ptr<Graphic::SceneRenderer>         m_Renderer;
        std::shared_ptr<::Desert::Core::Scene>          m_Scene;
        std::shared_ptr<::Desert::Core::GameplayCamera> m_Camera;
        std::shared_ptr<Graphic::ImageReadback>         m_Albedo; // G-buffer attachment 0, RGBA8 (Albedo.rgb)
        Phase                                           m_Phase      = Phase::Build;
        std::size_t                                     m_Next       = 0; // the tile being captured
        int                                             m_WarmFrames = 0;
        int                                             m_Waited     = 0; // frames spent on the current tile

        std::optional<::Desert::Core::WorldPartitionMiniMapSerialized> m_Result;
        std::optional<std::string>                                     m_Error;
    };
} // namespace Desert::Editor
