#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingDynamicGeometry.hpp>

#include <cstdint>
#include <memory>

// THE DEVICE-FACING SIDE OF ONE SKIN-CACHE ENTRY (RenderCore): the interface the API implements
// (VulkanSkinnedRayTracingGeometry) and the skin cache (RayTracingSkinCache.hpp, Renderer) drives. Kept apart from
// the cache so the API layer never includes the renderer.
namespace Desert::Graphic::RayTracing
{
    // Floats per vertex in a GPU skin cache entry's streams buffer (layout: RayTracingSkinCache.hpp;
    // Vertex_Skinned.glslh and SkinCache.shader read it). Here, in the RenderCore interface, because the Vulkan
    // side sizes the buffer and RenderCore does not include the renderer's skin cache.
    inline constexpr uint32_t kSkinCacheFloatsPerVertex = 15;

    // What one primitive's build costs this frame: the scratch it needs (alignment slack included) and whether it
    // refits the BLAS already built (true) or builds it anew (false).
    struct SkinnedBlasPrepared
    {
        uint64_t ScratchBytes = 0;
        bool     Refit        = false;
    };

    // The API side of one entry's cached streams (RayTracingSkinCache.hpp layout: current positions, previous
    // positions, tangent frame), alive across frames. The skin pass writes them, the raster view passes fetch them
    // by device address, and the entry's BLAS is built from their current positions (the buffer's start).
    class SkinCacheStreams
    {
    public:
        virtual ~SkinCacheStreams() = default;
        // Makes the buffer fit @p vertexCount vertices. Answers true when the buffer was (re)created this call:
        // its previous positions hold nothing yet. Errors: no device, the device has no buffer device addresses,
        // the allocation failed.
        [[nodiscard]] virtual Common::ResultStr<bool> Prepare( uint32_t vertexCount ) = 0;
        // Graph external carrying its own state across graphs (StorageWrite by the skin pass, StorageRead by the
        // raster view passes, AccelStructBuildInput by the dynamic-geometry pass).
        [[nodiscard]] virtual RDG::ExternalBuffer& GetBuffer() = 0;
        // The buffer's device address (its start = the current positions); 0 before a successful Prepare.
        [[nodiscard]] virtual uint64_t GetDeviceAddress() const = 0;
        // Bytes of the buffer (its ledger row; stats overlay).
        [[nodiscard]] virtual uint64_t GetBytes() const = 0;
    };

    // The API's streams (VulkanSkinnedRayTracingGeometry.cpp).
    [[nodiscard]] std::unique_ptr<SkinCacheStreams> CreateSkinCacheStreams();

    // The API side of one skinned primitive's BLAS, alive across frames: a dynamic-geometry target whose vertex
    // input is the entry's streams (current positions at the buffer's start, kDeformedPositionLayout) - it owns no
    // positions of its own, so the BLAS and the raster read the one skinning of the frame.
    class SkinnedRayTracingGeometry : public RayTracingDynamicGeometryTarget
    {
    public:
        // Makes the BLAS fit @p input (DescribeDeformedMeshBlas of the skinned mesh and the instance's hidden and
        // non-opaque masks), recreating it when the vertex/index buffers, the vertex count or the segments (a
        // section's opacity included) changed. Errors: the device cannot build acceleration structures, the
        // buffers are not resident, the input has no segment or no vertex, an allocation failed.
        [[nodiscard]] virtual Common::ResultStr<SkinnedBlasPrepared>
        Prepare( const RayTracingMeshInput& input ) = 0;
    };

    // The API's skin-cache entry (VulkanSkinnedRayTracingGeometry.cpp).
    [[nodiscard]] std::unique_ptr<SkinnedRayTracingGeometry> CreateSkinnedRayTracingGeometry();
} // namespace Desert::Graphic::RayTracing
