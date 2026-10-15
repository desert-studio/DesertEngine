#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingGeometry.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

// THE RT SCENE'S DYNAMIC GEOMETRY (UE FRayTracingDynamicGeometryCollection): every BLAS built INSIDE the frame
// graph, from vertices the graph knows - the skin cache's skinned positions, a procedural mesh's vertex buffer,
// landscape tiles (multi-geometry: opaque + alpha-tested; refit when only the vertices moved) - is recorded by
// ONE pass, "RayTracingScene.DynamicGeometry", owned by the RT scene and ordered
// before "RayTracingScene.TLAS" by the TLAS reading every storage it wrote. Producers never declare the pass: they
// append a build request each, and RayTracingSceneRenderer declares the pass once with all of them.
namespace Desert::Graphic::RayTracing
{
    inline constexpr std::string_view kDynamicGeometryPassName    = "RayTracingScene.DynamicGeometry";
    inline constexpr std::string_view kDynamicGeometryScratchName = "RayTracingScene.DynamicGeometryScratch";

    // The API side of one BLAS the dynamic-geometry pass builds: its storage is a graph external that carries its
    // own state across graphs, and its build is recorded into the pass's command buffer.
    class RayTracingDynamicGeometryTarget : public RayTracingGeometryResource
    {
    public:
        [[nodiscard]] virtual RDG::ExternalBuffer& GetStorage() = 0;
        // Records the build (full or refit, per the producer's last Prepare) reading @p vertices and, when the
        // request named them, @p indices (nullptr: the target reads an index buffer it holds by address), its
        // scratch at @p scratchOffset bytes into @p scratch.
        [[nodiscard]] virtual Common::BoolResultStr RecordBuild( const RDG::PassContext&   context,
                                                                 const RDG::BufferBinding& vertices,
                                                                 const RDG::BufferBinding* indices,
                                                                 const RDG::BufferBinding& scratch,
                                                                 uint64_t                  scratchOffset ) = 0;
    };

    // What a target's Prepare decided for this frame's build.
    struct RayTracingDynamicGeometryPlan
    {
        uint64_t ScratchBytes = 0;     // includes the scratch-address alignment slack
        bool     Update       = false; // refit in place (else a full build)
    };

    // The shape of a BLAS over a vertex + index buffer pair the graph writes (a landscape tile): Desert::Vertex
    // vertices, uint32 indices, and one triangle geometry per segment (Opaque or alpha-tested) over them.
    struct RayTracingDynamicMeshLayout
    {
        uint64_t                               VertexBytes = 0;
        uint64_t                               IndexBytes  = 0;
        std::vector<RayTracingGeometrySegment> Segments;
        bool                                   operator==( const RayTracingDynamicMeshLayout& ) const = default;
    };

    // A dynamic BLAS that owns its build inputs: a persistent vertex buffer and index buffer (written by a
    // producer's compute in the same graph) and the BLAS over them, built with ALLOW_UPDATE so a change of the
    // vertices alone refits in place. Every object has its own ledger row and is freed on the frame ring.
    class RayTracingDynamicMeshGeometryTarget : public RayTracingDynamicGeometryTarget
    {
    public:
        // Makes the buffers and the BLAS fit @p layout (recreating all three when it changed) and plans this
        // frame's build. @p update asks for a refit; it is granted only when nothing was recreated.
        [[nodiscard]] virtual Common::ResultStr<RayTracingDynamicGeometryPlan>
        Prepare( const RayTracingDynamicMeshLayout& layout, bool update ) = 0;
        [[nodiscard]] virtual RDG::ExternalBuffer& GetVertices()          = 0;
        [[nodiscard]] virtual RDG::ExternalBuffer& GetIndices()           = 0;
        // Bytes of the vertex and index buffers (GetMemoryBytes is the BLAS storage alone).
        [[nodiscard]] virtual uint64_t GetBuildInputBytes() const = 0;
    };

    // The API's object (VulkanRayTracingDynamicMeshGeometry); @p debugName prefixes its buffers' names.
    [[nodiscard]] std::unique_ptr<RayTracingDynamicMeshGeometryTarget>
    CreateRayTracingDynamicMeshGeometry( std::string_view debugName );

    // One BLAS to build this frame. @p Vertices (and @p Indices, when valid) are read as AccelStructBuildInput,
    // @p Storage written; @p ScratchBytes already includes the scratch-address alignment slack.
    struct RayTracingDynamicGeometryBuild
    {
        RayTracingDynamicGeometryTarget* Target = nullptr;
        RDG::BufferRef                   Vertices;
        RDG::BufferRef                   Indices; // invalid: the target's indices are not a graph resource
        RDG::BufferRef                   Storage;
        uint64_t                         ScratchBytes = 0;
    };

    // Declares "RayTracingScene.DynamicGeometry" (Compute) building every request in order, its scratch one
    // transient buffer sliced per build. No request: no pass, no scratch. Every Storage is appended to @p blasOut
    // for the TLAS pass to read. Error: a request without a target or storage (a producer bug, named with its
    // index). Value: the scratch bytes declared (0 without a request) — the RT scene stats read it.
    [[nodiscard]] Common::ResultStr<uint64_t>
    AddRayTracingDynamicGeometryPass( RDG::Builder& graph, std::span<const RayTracingDynamicGeometryBuild> builds,
                                      std::vector<RDG::BufferRef>& blasOut );
} // namespace Desert::Graphic::RayTracing
