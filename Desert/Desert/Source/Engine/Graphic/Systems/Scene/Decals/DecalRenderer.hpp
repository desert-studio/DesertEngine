#pragma once

#include <Engine/Assets/Common.hpp>
#include <Engine/Graphic/Materials/DataDrivenMaterial.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace Desert::Graphic
{
    class PipelineCache;
}

namespace Desert::Graphic::System
{
    // One decal as the ECS hands it to the renderer (UE FDeferredDecalProxy): the box, the material, the order and
    // the fade numbers. Per frame, copied (a command replayed into several views hands each the same value).
    struct DecalProxy
    {
        // The unit box [-1, 1]^3 in world: the entity's world transform x scale( Extent ). x / y the texture
        // plane, z the projection axis (ECS/DecalComponent.hpp).
        glm::mat4           DecalToWorld{ 1.0f };
        Assets::AssetHandle Material  = Assets::AssetHandle::Null(); // null = Programs/Decals/DefaultDecal.shader
        uint64_t            EntityId  = 0;                           // the stack's tie-break: never flickers
        int32_t             SortOrder = 0;
        uint8_t             ChannelMask    = 7u;   // Core::Formats::DecalChannel bits
        float               FadeStart      = 0.0f; // cm
        float               FadeLength     = 0.0f; // cm; 0 = no distance fade
        float               FadeScreenSize = 0.0f; // fraction of the screen height below which it is not drawn
        float               EdgeFade       = 0.1f;
    };

    // One row of "Decals.Rows" (std430, Programs/Decals/DefaultDecal.shader DecalRow): what a decal's vertex and
    // fragment stages need, baked per view on the CPU so the push block stays one row index.
    struct DecalRowGpu
    {
        glm::mat4 DecalToClip; // view-projection x DecalToWorld: the box's corners to clip space
        glm::mat4 ClipToDecal; // inverse( DecalToWorld ) x inverse view-projection: a depth texel into the box
        glm::vec4 AxisX;       // the box's world x / y / z axes, normalised (the decal's tangent frame)
        glm::vec4 AxisY;
        glm::vec4 AxisZ;
        glm::vec4 Params; // x opacity (the CPU fade), y edge fade, z channel mask (as a float), w unused
    };
    static_assert( sizeof( DecalRowGpu ) == 208, "DecalRowGpu is the std430 DecalRow of the decal shaders" );

    // The pass's own inputs, the one spelling Declare binds and every decal program is validated against.
    struct DecalShaderParameters
    {
        DESERT_SHADER_PARAMETER( Texture, SceneDepthTexture );
        DESERT_SHADER_PARAMETER( Sampler, PointClampSampler );
        DESERT_SHADER_PARAMETER( StorageBuffer, DecalRowsBuffer );
        static constexpr std::array kAll{ RDG::Erase( SceneDepthTexture ), RDG::Erase( PointClampSampler ),
                                          RDG::Erase( DecalRowsBuffer ) };
    };

    /**
     * @brief "Deferred: Decals" (DECALS-DBUFFER; UE DBuffer decals, FDeferredDecalProxy + RenderDBuffer).
     *
     * Per frame the ECS adds every visible DecalComponent (Add); per view PrepareFrame fades them on the CPU
     * (distance, screen size), drops the ones that fade to nothing, resolves each one's program (a `Domain
     * DeferredDecal` shader, else the default decal) and sorts them by (SortOrder, entity id), so equal orders
     * stack the same way every frame. The pass (SceneRenderer::AddFrameGBuffer, after the full early-Z) draws one
     * unit box per decal - 36 procedural vertices, back faces, no depth attachment - into DBufferA/B/C with the
     * premultiplied blend of Common/DBuffer.glslh; the fragment reconstructs the receiver from the prepass depth.
     * Consecutive decals of one program are one instanced draw (Vulkan keeps primitive order across instances, so
     * the blend order is the sort order).
     *
     * Not a RenderSystem: it owns no target, its targets are the frame graph's DBuffer transients. Every
     * container is reserved once and cleared per frame; nothing is looked up by string per frame (programs are
     * keyed by the material handle).
     */
    class DecalRenderer final
    {
    public:
        DecalRenderer();

        void Add( const DecalProxy& proxy );
        void ClearQueue();
        // Any decal recorded this frame (before the per-view fade): the frame then runs the full early-Z.
        [[nodiscard]] bool HasDecals() const
        {
            return !m_Decals.empty();
        }

        // Per view: fade, refuse, resolve, sort, bake the rows. @p viewProjection is the camera's projection x
        // view (the matrix the G-buffer rasters with), @p projectionYScale its projection[1][1].
        void               PrepareFrame( const glm::mat4& viewProjection, const glm::vec3& cameraPosition,
                                         float projectionYScale, PipelineCache& pipelines );
        [[nodiscard]] bool HasDraws() const
        {
            return !m_Draws.empty();
        }

        // The rows of this frame's draws as a buffer of @p graph (uploaded before the graph runs).
        [[nodiscard]] RDG::BufferRef UploadRows( RDG::Builder& graph ) const;
        // One binding block per program this frame draws, in the order Render indexes them.
        void Declare( RDG::PassBuilder& pass, RDG::TextureRef sceneDepth, RDG::BufferRef rows ) const;
        [[nodiscard]] Common::BoolResultStr Render( const RDG::PassContext& context ) const;

    private:
        struct Program
        {
            std::shared_ptr<GraphicsPipeline>   Pipeline;
            std::unique_ptr<DataDrivenMaterial> Material;
            mutable ShaderBindingLayoutCache    Layout;
            uint8_t                             Channels   = 0u;    // the shader's `DecalChannels`
            uint64_t                            Generation = 0u;    // the shader's code generation it was built at
            bool                                Refused    = false; // logged once; the decal draws nothing
            mutable uint32_t                    Block      = 0u;    // this frame's binding block (Declare order)
        };
        struct FrameDecal
        {
            int32_t        SortOrder;
            uint64_t       EntityId;
            const Program* Program;
            DecalRowGpu    Row;
        };
        // A run of consecutive sorted decals of one program: one instanced draw.
        struct Draw
        {
            const Program* Program;
            uint32_t       FirstRow;
            uint32_t       Count;
        };

        const Program*        ProgramFor( const Assets::AssetHandle& material, PipelineCache& pipelines );
        Common::BoolResultStr Build( Program& program, const Assets::AssetHandle& material,
                                     PipelineCache& pipelines ) const;

        std::vector<DecalProxy>               m_Decals;   // this frame's, from the ECS
        std::vector<FrameDecal>               m_Frame;    // this view's, faded and sorted
        std::vector<DecalRowGpu>              m_Rows;     // m_Frame's rows in draw order
        std::vector<Draw>                     m_Draws;    // runs of m_Rows
        std::vector<const Program*>           m_Programs; // the distinct programs of m_Draws, block order
        std::unordered_map<uint64_t, Program> m_ProgramsByMaterial; // by material handle; 0 = the default decal
    };
} // namespace Desert::Graphic::System
