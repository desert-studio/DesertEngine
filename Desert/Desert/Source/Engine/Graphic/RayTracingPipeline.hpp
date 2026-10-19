#pragma once

#include <Engine/Graphic/Shader.hpp>

#include <Common/Core/Core.hpp>
#include <Common/Core/ResultStr.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// THE RAY TRACING PIPELINE (VK_KHR_ray_tracing_pipeline; UE: FRayTracingPipelineState built from a ray generation
// shader, miss shaders and the scene's material hit groups, RayTracingMaterialHitShaders.cpp). One per (ray
// generation program x set of hit groups):
//   * Program   a pass whose stages are exactly one RayGen and one Miss (e.g. "SSR/HitLighting"). Its reflection
//               IS the pipeline's descriptor layout and push block: a pass binds it through RDG::PassBindings as a
//               compute dispatch binds its program (Renderer::TraceRays);
//   * HitGroups surface templates' `RayTracing.Hit` cells (DShaderParser kSurfaceRayHitPath), one triangle hit
//               group each: closest hit, and any hit for a Masked template. Hit group i of the pipeline is
//               HitGroups[i]; a frame's hit records name it by that index (RayTracingHitGroupTable.hpp).
// A hit cell reads its geometry and its material row from its shader record, so it binds nothing of set 0 but
// what the program binds as well (the view block): Create refuses a hit cell that declares a set-0 binding the
// program does not, or the same binding as another descriptor kind, naming both - the hit stages share the
// program's sets. Refused, too, on a device without the ray tracing pipeline (the reason names the device).
namespace Desert::Graphic
{
    struct RayTracingPipelineSpecification
    {
        std::shared_ptr<Shader>              Program;
        std::vector<std::shared_ptr<Shader>> HitGroups;
        std::string                          DebugName;
    };

    class RayTracingPipeline
    {
    public:
        virtual ~RayTracingPipeline() = default;

        [[nodiscard]] virtual const RayTracingPipelineSpecification& GetSpecification() const = 0;
        [[nodiscard]] uint32_t                                       GetHitGroupCount() const
        {
            return static_cast<uint32_t>( GetSpecification().HitGroups.size() );
        }

        NO_DISCARD static Common::ResultStr<std::shared_ptr<RayTracingPipeline>>
        Create( const RayTracingPipelineSpecification& spec );
    };
} // namespace Desert::Graphic
