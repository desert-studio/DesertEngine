#pragma once

#include <Engine/Graphic/GpuInstances.hpp>
#include <Engine/Graphic/InstanceWind.hpp>
#include <Engine/Graphic/Materials/MaterialInstance.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>
#include <Engine/Graphic/Systems/RenderSystem.hpp>
#include <Engine/ShaderResources/StorageBuffer.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace Desert
{
    class Mesh;
}

namespace Desert::Graphic
{
    struct ViewFrame;
}

namespace Desert::Graphic::System
{
    class MeshRenderer;

    // One landscape root's heights and grass-layer weights on ONE sample grid over all its tiles (tiles share
    // their edge samples), packed as the generator reads it (Programs/Grass/GrassGenerate.shader, u_Field):
    //   [0] samples X, [1] samples Z, [2] grass layer count, [3] 0, [4] origin X (float bits), [5] origin Z,
    //   [6] spacing cm, [7] 0, then one float world height per sample (kGrassNoHeight where no tile is loaded),
    //   then per grass layer one byte of weight per sample, four to a word.
    struct GrassFieldData
    {
        std::vector<uint32_t> Words;
        uint64_t              Revision = 0; // changes whenever Words do
    };
    inline constexpr float kGrassNoHeight = -2.0e30f;

    // UE FGrassVariety's placement, resolved by the ECS (LandscapeECSSystem) from the layer info's variety: units
    // are the asset's (Density per 1000 x 1000 cm, cm distances, Z up).
    struct GrassPlacement
    {
        float     Density           = 400.0f;
        float     PlacementJitter   = 1.0f;
        float     StartCullDistance = 0.0f;
        float     EndCullDistance   = 10000.0f;
        glm::vec2 AllowedDensityRange{ 0.0f, 1.0f };
        uint32_t  Scaling = 0; // 0 uniform, 1 free, 2 lock XY
        glm::vec2 ScaleX{ 1.0f }, ScaleY{ 1.0f }, ScaleZ{ 1.0f };
        bool      WeightAttenuatesMaxScale  = false;
        float     MaxScaleWeightAttenuation = 0.5f;
        bool      RandomRotation            = true;
        bool      AlignToSurface            = true;
        bool      CastDynamicShadow         = true;
    };

    struct GrassSectionDraw
    {
        MaterialInstancePtr Material;
        uint64_t            HiddenSubmeshes = 0;
    };

    struct GrassVarietyDraw
    {
        uint64_t                      Key   = 0; // stable: (root, layer, grass type, variety)
        uint32_t                      Layer = 0; // the grass layer's index in the field
        class Desert::Mesh*           Mesh  = nullptr;
        std::vector<GrassSectionDraw> Sections;
        GrassPlacement                Placement;
        InstanceWind                  Wind;
    };

    struct GrassFieldDraw
    {
        uint64_t                              Root = 0;
        std::shared_ptr<const GrassFieldData> Field;
        std::vector<GrassVarietyDraw>         Varieties;
    };

    // UE's landscape grass (ALandscapeProxy::UpdateGrass) on the GPU: every frame, for the main view, each variety
    // of each landscape grows its instances on a world grid around the camera (GrassGenerate, one thread per cell,
    // culled to the view and faded out to EndCullDistance) into a buffer of its own, and its sections are drawn
    // through the mesh renderer's instanced cells - G-buffer or forward, and the cascades - by indirect draws
    // (GpuInstances.hpp). Nothing is stored in the level and nothing is read back.
    class GrassRenderer final : public RenderSystem
    {
    public:
        using RenderSystem::RenderSystem;

        Common::BoolResultStr Initialize() override;

        void Submit( GrassFieldDraw draw )
        {
            m_Queue.push_back( std::move( draw ) );
        }
        void ClearQueue()
        {
            m_Queue.clear();
        }

        // Before the cascades and the geometry: the generation nodes of this frame's view, and the sections handed
        // to @p meshes. A view that is not the main one (a capture face) grows nothing.
        void AddFrame( RDG::Builder& graph, const ViewFrame& frame, MeshRenderer& meshes );

    private:
        struct FieldGpu
        {
            std::shared_ptr<ShaderResources::StorageBuffer> Buffer;
            uint64_t                                        Revision = 0;
            RDG::ExternalBuffer                             Import;
            RDG::BufferRef                                  Ref;
        };
        struct VarietyGpu
        {
            class Desert::Mesh*                             Mesh     = nullptr;
            uint32_t                                        Capacity = 0;
            std::shared_ptr<ShaderResources::StorageBuffer> Instances;
            std::shared_ptr<ShaderResources::StorageBuffer> Args;
            std::shared_ptr<ShaderResources::StorageBuffer> Params;
            RDG::ExternalBuffer                             InstancesImport, ArgsImport, ParamsImport;
            GpuInstanceRefs                                 Refs;
            RDG::BufferRef                                  ParamsRef;
            RDG::BufferRef                                  FieldRef;
            glm::ivec4                                      Cells{ 0 };
            uint32_t                                        Index = 0; // stable section key base
        };

        [[nodiscard]] VarietyGpu* PrepareVariety( const GrassVarietyDraw& variety, const ViewFrame& frame );

        std::vector<GrassFieldDraw>              m_Queue;
        std::unordered_map<uint64_t, FieldGpu>   m_Fields;
        std::unordered_map<uint64_t, VarietyGpu> m_Varieties;
        std::vector<VarietyGpu*>                 m_Frame; // this frame's, in dispatch order
        std::shared_ptr<ComputePipeline>         m_Generate;
        std::shared_ptr<ComputePipeline>         m_Finalize;
        mutable ShaderBindingLayoutCache         m_GenerateLayout;
        mutable ShaderBindingLayoutCache         m_FinalizeLayout;
        uint32_t                                 m_NextIndex = 0;
    };
} // namespace Desert::Graphic::System
