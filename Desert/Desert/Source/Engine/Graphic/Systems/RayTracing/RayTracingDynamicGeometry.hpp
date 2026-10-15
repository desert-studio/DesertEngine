#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingGeometry.hpp>

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

// THE RT SCENE'S DYNAMIC GEOMETRY (UE FRayTracingDynamicGeometryCollection): every BLAS built INSIDE the frame
// graph, from vertices the graph knows - the skin cache's skinned positions, a procedural mesh's vertex buffer,
// landscape tiles - is recorded by ONE pass, "RayTracingScene.DynamicGeometry", owned by the RT scene and ordered
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
        // Records the build (full or refit, per the producer's last Prepare) reading @p vertices, its scratch at
        // @p scratchOffset bytes into @p scratch.
        [[nodiscard]] virtual Common::BoolResultStr RecordBuild( const RDG::PassContext&   context,
                                                                 const RDG::BufferBinding& vertices,
                                                                 const RDG::BufferBinding& scratch,
                                                                 uint64_t                  scratchOffset ) = 0;
    };

    // One BLAS to build this frame. @p Vertices is read as AccelStructBuildInput, @p Storage written; @p
    // ScratchBytes already includes the scratch-address alignment slack.
    struct RayTracingDynamicGeometryBuild
    {
        RayTracingDynamicGeometryTarget* Target = nullptr;
        RDG::BufferRef                   Vertices;
        RDG::BufferRef                   Storage;
        uint64_t                         ScratchBytes = 0;
    };

    // Declares "RayTracingScene.DynamicGeometry" (Compute) building every request in order, its scratch one
    // transient buffer sliced per build. No request: no pass, no scratch. Every Storage is appended to @p blasOut
    // for the TLAS pass to read. Error: a request without a target or storage (a producer bug, named with its
    // index).
    [[nodiscard]] Common::BoolResultStr
    AddRayTracingDynamicGeometryPass( RDG::Builder& graph, std::span<const RayTracingDynamicGeometryBuild> builds,
                                      std::vector<RDG::BufferRef>& blasOut );
} // namespace Desert::Graphic::RayTracing
