#pragma once

#include <Common/Core/Core.hpp> // BOOLSUCCESS (the .cpp and VulkanSkinnedRayTracingGeometry.cpp)
#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingGeometry.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingScene.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Desert
{
    class SkinnedMesh;
}

namespace Desert::Graphic
{
    class ComputePipeline;
}

namespace Desert::Graphic::System
{
    class MeshRenderer;
}

// THE GPU SKIN CACHE FOR RAY TRACING (UE FGPUSkinCache + FRayTracingDynamicGeometryCollection). The TLAS instances
// a skinned primitive through a BLAS of its POSED triangles, so every frame a compute pass skins the primitive's
// LOD-0 vertices into a cached positions buffer (kDeformedPositionLayout: one float3 per vertex, mesh space, the
// mesh's own vertex order, so its index buffer and submesh ranges address it unchanged) and the primitive's BLAS
// is built from it - fully on the first frame and whenever the mesh or the hidden mask changes, refit in place
// every other frame. Both run inside the view's graph, before "RayTracingScene.TLAS" (the TLAS pass reads every
// built BLAS).
//
// ONE SOURCE OF THE POSE. The skin pass reads the same bytes the raster skins from: the view's ObjectBones palette
// at the primitive's row BoneOffset (MeshRenderer::BuildObjectMotions), and the vertices of the mesh the queue
// record names (a morphed component's twin included). No second palette upload exists.
//
// Raster still skins in its vertex stage (Vertex_Skinned.glslh). UE's raster reads the cache when it is on; that
// needs the cache to also keep the previous positions (velocity) and the tangent frame - a separate step.
namespace Desert::Graphic::RayTracing
{
    inline constexpr std::string_view kSkinCacheShader         = "SkinCache";
    inline constexpr std::string_view kSkinCachePassName       = "SkinCache: Positions";
    inline constexpr std::string_view kDynamicGeometryPassName = "RayTracingScene.DynamicGeometry";
    inline constexpr uint32_t         kSkinCacheGroupSize = 64; // LocalSize of Programs/Compute/SkinCache.shader

    // What one primitive's build costs this frame: the scratch it needs (alignment slack included) and whether it
    // refits the BLAS already built (true) or builds it anew (false).
    struct SkinnedBlasPrepared
    {
        uint64_t ScratchBytes = 0;
        bool     Refit        = false;
    };

    // The API side of one skinned primitive's cache entry: its cached positions and its BLAS, alive across frames.
    class SkinnedRayTracingGeometry : public RayTracingGeometryResource
    {
    public:
        // Makes the positions buffer and the BLAS fit @p mesh with @p hiddenSubmeshes left out, recreating both
        // when the mesh's vertex/index buffers, its vertex count or the mask changed. Errors: the device cannot
        // build acceleration structures, the mesh is not resident or has no visible triangle, an allocation
        // failed.
        [[nodiscard]] virtual Common::ResultStr<SkinnedBlasPrepared> Prepare( const SkinnedMesh& mesh,
                                                                              uint64_t hiddenSubmeshes ) = 0;
        // Graph externals carrying their own state across graphs: the positions the skin pass writes and the BLAS
        // storage the build writes.
        [[nodiscard]] virtual RDG::ExternalBuffer& GetPositions() = 0;
        [[nodiscard]] virtual RDG::ExternalBuffer& GetStorage()   = 0;
        // Records the build (or refit, per the last Prepare) into the pass's command buffer, its scratch at
        // @p scratchOffset bytes into @p scratch.
        [[nodiscard]] virtual Common::BoolResultStr RecordBuild( const RDG::PassContext&   context,
                                                                 const RDG::BufferBinding& positions,
                                                                 const RDG::BufferBinding& scratch,
                                                                 uint64_t                  scratchOffset ) = 0;
    };

    // The API's skin-cache entry (VulkanSkinnedRayTracingGeometry).
    [[nodiscard]] std::unique_ptr<SkinnedRayTracingGeometry> CreateSkinnedRayTracingGeometry();

    // The scene's skin cache (RayTracingSceneCache owns one, shared by every view of the scene, UE FGPUSkinCache
    // per FScene): each skinned primitive is skinned and its BLAS built / refit ONCE per frame, by the first view
    // that asks, from that view's ObjectBones (the pose is the entity's, not the view's); the next views of the
    // frame instance the same BLAS.
    class RayTracingSkinCache
    {
    public:
        // For every primitive SelectDynamicGeometry picks: prepares its entry (keyed {Entity, n-th skinned record
        // of the entity this frame}, MotionKey's slot rule), declares its "SkinCache: Positions" dispatch, then
        // ONE "RayTracingScene.DynamicGeometry" pass building/refitting every prepared BLAS, and points the
        // primitive's Geometry at its BLAS. @p primitives[firstSkinned + i] is MeshRenderer's skinned record i.
        // Every BLAS storage the pass writes is appended to @p blasOut for the TLAS pass to read. A primitive
        // whose entry cannot be prepared keeps Geometry null (counted RayTracingSkip::Skinned) and is logged once
        // per entry, never silently. An entry another view already skinned in @p frame is not skinned again: its
        // primitive points at the same BLAS. @p viewBones is the calling view's import slot for its ObjectBones.
        [[nodiscard]] Common::BoolResultStr AddPasses( RDG::Builder& graph, const System::MeshRenderer& meshes,
                                                       RDG::ExternalBuffer&           viewBones,
                                                       std::span<RayTracingPrimitive> primitives,
                                                       uint32_t firstSkinned, uint64_t frame,
                                                       std::vector<RDG::BufferRef>& blasOut );

        // Releases the entries no view asked for in @p frame (RayTracingSceneCache::BeginViewFrame).
        void EndFrame( const uint64_t frame )
        {
            std::erase_if( m_Entries, [frame]( const auto& entry ) { return entry.second.LastFrame != frame; } );
        }

        // Live entries (one per skinned primitive the TLAS instanced last frame).
        [[nodiscard]] std::size_t GetEntryCount() const
        {
            return m_Entries.size();
        }

    private:
        struct Entry
        {
            std::unique_ptr<SkinnedRayTracingGeometry> Geometry;
            RDG::ExternalBuffer VerticesImport; // the mesh's vertex buffer as a graph external
            uint64_t            LastFrame     = 0;
            bool                Ready         = false; // prepared and skinned in LastFrame: views may trace it
            bool                RefusalLogged = false;
        };

        std::unordered_map<uint64_t, Entry>    m_Entries;
        std::unordered_map<uint32_t, uint32_t> m_RecordsPerEntity; // CPU scratch: skinned records seen per entity
        std::shared_ptr<ComputePipeline>       m_Pipeline;
        ShaderBindingLayoutCache               m_Layout;
        bool                                   m_PipelineRefusalLogged = false;
    };
} // namespace Desert::Graphic::RayTracing
