#pragma once

// THE LANDSCAPE HLOD'S BASECOLOR, BAKED (UE ULandscapeHLODBuilder / FMaterialUtilities::ExportLandscapeMaterial):
// an offscreen scene holding only the saved world's landscape is looked at straight down through an orthographic
// camera that frames exactly the X/Z rectangle a landscape HLOD's UVs span
// (::Desert::Core::LandscapeHLODTileAreaOf), and the G-buffer's albedo — the material's BaseColor before any
// light, not the lit and tonemapped picture — is read back and saved beside the built HLODs as
// `T_<tile>_BaseColor.detex`; the G-buffer's roughness (B.a) and material AO (Emissive.a) are read back with it
// and saved as `T_<tile>_ORM.detex` (UE's landscape HLOD bakes roughness too); the G-buffer's world normal (B.xyz)
// is brought into the tangent frame of the tile's own HLOD render mesh — the mesh the runtime draws, rasterised in
// its UV space — and saved as `T_<tile>_Normal.detex` (UE bakes the landscape normal into the HLOD the same way,
// FMaterialUtilities::ExportLandscapeMaterial + the proxy's tangent frame), and the material that binds all three,
// `M_<tile>.demat`. The capture is recorded into the
// editor's own frames (as AssetThumbnailRenderer's is), so a bake runs over several frames: Tick() once per editor
// frame until it answers true.

#include <Engine/Core/HLODMeshBuilder.hpp>
#include <Engine/Core/Serialize/SceneFormat.hpp>
#include <Engine/Geometry/SavedMeshForm.hpp>

#include <Common/Core/ResultStr.hpp>
#include <Common/Core/UUID.hpp>

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
    // The material one landscape HLOD tile binds, as a StaticMesh slot states it.
    struct LandscapeHLODBaked
    {
        Common::UUID Tile; // the tile's record id
        std::string  MaterialGuid;
        std::string  MaterialPath; // as a scene record stores a material slot (assets-relative)
    };

    // One tile to bake: the X/Z rectangle its UVs span and the HLOD mesh the build made for it (its EditMesh, the
    // source the runtime's render mesh is derived from), whose tangent frame the baked normal is stated in.
    struct LandscapeHLODBakeTile
    {
        ::Desert::Core::LandscapeHLODTileArea Area;
        ::Desert::Geometry::MeshSavedForm     Mesh;
    };

    class LandscapeHLODBake
    {
    public:
        // @p scenePath: the saved scene (its BuiltHLODDirectory receives the assets). @p saved: that file, parsed;
        // only its landscape records are loaded into the bake's scene. @p tiles: the areas to bake, each with its
        // HLOD mesh.
        LandscapeHLODBake( std::filesystem::path scenePath, ::Desert::Core::LoadableScene saved,
                           std::vector<LandscapeHLODBakeTile> tiles, const Assets::AssetManager* assets );
        ~LandscapeHLODBake();

        LandscapeHLODBake( const LandscapeHLODBake& )            = delete;
        LandscapeHLODBake& operator=( const LandscapeHLODBake& ) = delete;

        // One editor frame of the bake. True once every tile is written, or the bake was refused (Error()).
        [[nodiscard]] bool Tick();

        [[nodiscard]] const std::vector<LandscapeHLODBaked>& Baked() const
        {
            return m_Baked;
        }
        [[nodiscard]] const std::optional<std::string>& Error() const
        {
            return m_Error;
        }

    private:
        enum class Phase
        {
            Build,    // the renderer is built a stage per frame
            Frame,    // the current tile is framed; warm-up frames are recorded
            Readback, // the albedo, G-buffer B and emissive copies are in flight
            Done,
        };

        [[nodiscard]] Common::BoolResultStr StartScene();
        void                                FrameTile();
        // @p rgba: the albedo, RGBA8. @p gbufferB / @p emissive: the RGBA16F targets' own texels (8 bytes each).
        [[nodiscard]] Common::BoolResultStr SaveTile( const std::vector<uint8_t>& rgba,
                                                      const std::vector<uint8_t>& gbufferB,
                                                      const std::vector<uint8_t>& emissive, uint32_t width,
                                                      uint32_t height );
        void                                Fail( std::string error );

        std::filesystem::path                    m_ScenePath;
        std::optional<::Desert::Core::LoadableScene>       m_Saved;
        std::vector<LandscapeHLODBakeTile>                 m_Tiles;
        const Assets::AssetManager*              m_Assets = nullptr;

        std::unique_ptr<Graphic::SceneRenderer> m_Renderer;
        std::shared_ptr<::Desert::Core::Scene>          m_Scene;
        std::shared_ptr<::Desert::Core::GameplayCamera> m_Camera;
        std::shared_ptr<Graphic::ImageReadback>         m_Albedo; // G-buffer attachment 0, RGBA8 (Albedo.rgb)
        std::shared_ptr<Graphic::ImageReadback>
             m_GBufferB; // attachment 1, RGBA16F (world Normal.xyz, Roughness.a)
        std::shared_ptr<Graphic::ImageReadback> m_Emissive; // attachment 3, RGBA16F (Emissive.rgb, AO.a)
        uint32_t                                m_ReadbackWidth  = 0;
        uint32_t                                m_ReadbackHeight = 0;
        Phase                                   m_Phase          = Phase::Build;
        std::size_t                             m_Next           = 0; // the tile being baked
        int                                     m_WarmFrames     = 0;
        std::vector<LandscapeHLODBaked>         m_Baked;
        std::optional<std::string>              m_Error;
    };
} // namespace Desert::Editor
