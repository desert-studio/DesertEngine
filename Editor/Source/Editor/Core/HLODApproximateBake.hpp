#pragma once

// THE MESH APPROXIMATE HLOD'S TEXTURES, CAPTURED (UE FApproximateActorsImpl: FSceneCapturePhotoSet +
// FRenderCaptureMapBaker): an offscreen scene holding ONLY the approximated cell's own parts (UE photographs the HLOD's own
// actors: a neighbouring cell's wall inside the frame would be captured onto this shell) is photographed around
// each approximated cell from the directions
// ::Desert::Core::HLODCaptureViews gives - orthographic, framing the cell's merged source - and each photo's
// G-buffer (BaseColor + Metallic, world Normal + Roughness, material AO; Mesh/Surface/GBufferOutputs.glslh) is read
// back. ::Desert::Core::BakeApproximationTextures projects the shell's texels into those photos; the maps are saved
// beside the built HLODs as `T_A<key>_BaseColor.detex`, `T_A<key>_Normal.detex`, `T_A<key>_ORM.detex` and bound by
// `M_A<key>.demat` (StandardSurface), the one material of the cell's shell. Recorded into the editor's own frames
// like LandscapeHLODBake: Tick() once per editor frame until it answers true.

#include <Engine/Core/HLODApproximate.hpp>
#include <Engine/Core/HLODMeshBuilder.hpp>
#include <Engine/Core/Serialize/SceneFormat.hpp>
#include <Engine/Geometry/DynamicMeshSerialization.hpp>
#include <Engine/Geometry/SavedMeshForm.hpp>

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
    // One approximated cell to capture: its key (::Desert::Core::HLODApproximationKey), the layer's settings, the
    // parts its shell was wrapped from (re-merged here as the surface the photos must see) and the shell itself.
    struct HLODApproximateBakeCell
    {
        std::string                                            Key;
        ::Desert::Core::HLODApproximationSettings              Settings;
        std::vector<::Desert::Assets::InstancedStaticMeshComponentSer> Parts;
        ::Desert::Geometry::MeshSavedForm                      Shell;
    };

    // The material one cell's shell binds, as a StaticMesh slot states it.
    struct HLODApproximateBaked
    {
        std::string Key;
        std::string MaterialGuid;
        std::string MaterialPath; // as a scene record stores a material slot (assets-relative)
    };

    class HLODApproximateBake
    {
    public:
        HLODApproximateBake( std::filesystem::path scenePath, ::Desert::Core::LoadableScene saved,
                             std::vector<HLODApproximateBakeCell> cells, const Assets::AssetManager* assets );
        ~HLODApproximateBake();

        HLODApproximateBake( const HLODApproximateBake& )            = delete;
        HLODApproximateBake& operator=( const HLODApproximateBake& ) = delete;

        // One editor frame of the bake. True once every cell is written, or the bake was refused (Error()).
        [[nodiscard]] bool Tick();

        [[nodiscard]] const std::vector<HLODApproximateBaked>& Baked() const
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
            Frame,    // the current view is framed; warm-up frames are recorded
            Readback, // the G-buffer copies are in flight
            Done,
        };

        [[nodiscard]] Common::BoolResultStr StartScene();
        [[nodiscard]] Common::BoolResultStr StartCell();
        void                                FrameView();
        [[nodiscard]] Common::BoolResultStr SaveCell();
        void                                Fail( std::string error );

        std::filesystem::path                           m_ScenePath;
        std::optional<::Desert::Core::LoadableScene>    m_Saved;
        std::vector<HLODApproximateBakeCell>            m_Cells;
        const Assets::AssetManager*                     m_Assets = nullptr;

        std::unique_ptr<Graphic::SceneRenderer>         m_Renderer;
        std::shared_ptr<::Desert::Core::Scene>          m_Scene;
        std::shared_ptr<::Desert::Core::GameplayCamera> m_Camera;
        std::shared_ptr<Graphic::ImageReadback>         m_Albedo;   // GBufferA, RGBA8 (BaseColor.rgb, Metallic.a)
        std::shared_ptr<Graphic::ImageReadback>         m_GBufferB; // RGBA16F (world Normal.xyz, Roughness.a)
        std::shared_ptr<Graphic::ImageReadback>         m_Emissive; // RGBA16F (Emissive.rgb, AO.a)
        uint32_t                                        m_ReadbackWidth  = 0;
        uint32_t                                        m_ReadbackHeight = 0;
        Phase                                           m_Phase          = Phase::Build;
        std::size_t                                     m_Next           = 0; // the cell being captured
        std::size_t                                     m_View           = 0; // its view being captured
        int                                             m_WarmFrames     = 0;
        bool                                            m_FirstFrame     = true;
        ::Desert::Geometry::DynamicMesh3                m_Source; // the current cell's merged parts
        std::vector<Common::UUID>                       m_CellRecords; // the current cell's part records in m_Scene
        std::vector<::Desert::Core::HLODCaptureView>    m_Views;
        std::vector<::Desert::Core::HLODCapturePhoto>   m_Photos;
        std::vector<HLODApproximateBaked>               m_Baked;
        std::optional<std::string>                      m_Error;
    };
} // namespace Desert::Editor
