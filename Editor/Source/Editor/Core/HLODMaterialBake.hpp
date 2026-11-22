#pragma once

// THE MERGED MATERIAL OF A MESH HLOD, BAKED (WP-HLOD-MATBAKE; UE IMeshMergeUtilities::MergeComponentsToStaticMesh
// / CreateProxyMesh with FMeshMergingSettings::bMergeMaterials, whose FMaterialBakingModule renders every source
// material over the merged mesh's UV layout). For each MeshMerge / MeshSimplify HLOD whose layer states
// MergeMaterials, the build hands over its bake source (::Desert::Core::HLODMaterialBakeSource): the source
// surface laid flat on its atlas, carrying its source UVs, world frames and source materials. An offscreen scene
// holding only that proxy is looked at straight down through an orthographic camera framing the atlas square, and
// the G-buffer - each source material as the renderer evaluates it, before any light - is read back:
//   MeshMerge    - the built mesh IS that atlas: BaseColor, ORM (AO, roughness, metallic) and Emissive are the
//                  readback texel for texel; the world normal is taken into the tangent frame of the built mesh's
//                  render form (the frame the runtime shades with), as the landscape bake does.
//   MeshSimplify - the readback is the flattened SOURCE; every texel of the simplified mesh's own atlas is
//                  projected along its normal onto the source surface (Geometry::BakeSurface + TriangleBvh, UE's
//                  proxy transfer and BakeMeshMaps' correspondence) and takes the source atlas texel there
//                  (bilinear), its world normal brought into the simplified mesh's tangent frame.
// Texels no surface covers take the nearest covered texel's value (UE's UV border smear) so mips never pull the
// empty atlas in. The maps the layer's MergeMaterials asks for are written by the shared writer
// (HLODBakeWriter.hpp) as T_HLOD_<key>_*.detex + M_HLOD_<key>.demat, and the built mesh comes back with its one
// slot bound to it. The capture is recorded into the editor's own frames: Tick() once per editor frame until it
// answers true.

#include <Engine/Core/HLODMeshBuilder.hpp>
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
    // One mesh HLOD to bake: what BuildHLODMeshForBake answered for its request.
    struct HLODMaterialBakeJob
    {
        ::Desert::Core::HLODMaterialBakeSource   Source;
        ::Desert::Assets::StaticMeshComponentSer Mesh; // the built HLOD, no slot yet
    };

    // The built HLOD with its one slot bound to the baked M_HLOD_<Key>.
    struct HLODMaterialBaked
    {
        std::string                              Key;
        ::Desert::Assets::StaticMeshComponentSer Mesh;
    };

    class HLODMaterialBake
    {
    public:
        // @p scenePath: the saved scene (its BuiltHLODDirectory receives the assets). @p sceneText: that file's
        // text, parsed once per job; only its scene-wide settings enter the job's scene (its records are replaced
        // by the job's proxy, and it is no partitioned world).
        HLODMaterialBake( std::filesystem::path scenePath, std::string sceneText,
                          std::vector<HLODMaterialBakeJob> jobs, const Assets::AssetManager* assets );
        ~HLODMaterialBake();

        HLODMaterialBake( const HLODMaterialBake& )            = delete;
        HLODMaterialBake& operator=( const HLODMaterialBake& ) = delete;

        // One editor frame of the bake. True once every job is written, or the bake was refused (Error()).
        [[nodiscard]] bool Tick();

        [[nodiscard]] const std::vector<HLODMaterialBaked>& Baked() const
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
            Frame,    // the current job's proxy is loaded and framed; warm-up frames are recorded
            Readback, // the G-buffer A, B and emissive copies are in flight
            Done,
        };

        // The current job's scene: the saved scene's settings, the job's proxy as its only record.
        [[nodiscard]] Common::BoolResultStr StartJob();
        [[nodiscard]] Common::BoolResultStr SaveJob( std::vector<uint8_t> a, std::vector<uint8_t> b,
                                                     std::vector<uint8_t> emissive, uint32_t width,
                                                     uint32_t height );
        void                                Fail( std::string error );

        std::filesystem::path                           m_ScenePath;
        std::string                                     m_SceneText;
        std::vector<HLODMaterialBakeJob>                m_Jobs;
        const Assets::AssetManager*                     m_Assets = nullptr;
        std::unique_ptr<Graphic::SceneRenderer>         m_Renderer;
        std::shared_ptr<::Desert::Core::Scene>          m_Scene;
        std::shared_ptr<::Desert::Core::GameplayCamera> m_Camera;
        std::shared_ptr<Graphic::ImageReadback>         m_A;        // attachment 0, RGBA8 (BaseColor, Metallic)
        std::shared_ptr<Graphic::ImageReadback>         m_B;        // attachment 1, RGBA16F (Normal, Roughness)
        std::shared_ptr<Graphic::ImageReadback>         m_Emissive; // attachment 3, RGBA16F (Emissive, AO)
        uint32_t                                        m_ReadbackWidth  = 0;
        uint32_t                                        m_ReadbackHeight = 0;
        Phase                                           m_Phase          = Phase::Build;
        std::size_t                                     m_Next           = 0; // the job being baked
        int                                             m_WarmFrames     = 0;
        std::vector<HLODMaterialBaked>                  m_Baked;
        std::optional<std::string>                      m_Error;
    };
} // namespace Desert::Editor
