#include "IncludeLists.hpp"

#include <Engine/Graphic/DistanceField/DistanceFieldLayout.hpp>
#include <Engine/Graphic/GlobalIllumination/GILayout.hpp>
#include <Engine/Graphic/GlobalIllumination/SurfaceCacheLayout.hpp>
#include <Engine/Graphic/Materials/Debug/DebugMaterialLayout.hpp>
#include <Engine/Graphic/Neural/NeuralRadianceCacheLayout.hpp>
#include <Engine/Graphic/ReflectionCaptures/ReflectionCaptureLayout.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingLayout.hpp>
#include <Engine/Graphic/WorldParameterLayout.hpp>

namespace Desert::ShaderProtocolGen
{
    // Run C: the global distance field, GI (frame, tracer, radiance cache, ReSTIR, screen probes), the surface
    // cache, the ray-tracing records and emissive lights, the neural radiance cache, reflection captures, the
    // world parameter table, the debug materials. A required include comes before its requirer.
    std::span<const ShaderIncludeDecl* const> LightingIncludes()
    {
        static constexpr const ShaderIncludeDecl* kIncludes[] = {
             &Graphic::DistanceField::kDistanceFieldShaderInclude,
             &Graphic::GlobalIllumination::kGIFrameShaderInclude,
             &Graphic::GlobalIllumination::kGITracerShaderInclude,
             &Graphic::GlobalIllumination::kRadianceCacheShaderInclude,
             &Graphic::GlobalIllumination::kReSTIRShaderInclude,
             &Graphic::GlobalIllumination::kScreenProbesShaderInclude,
             &Graphic::SurfaceCache::kSurfaceCacheShaderInclude,
             &Graphic::RayTracing::kRayTracingRecordsShaderInclude,
             &Graphic::RayTracing::kEmissiveLightsShaderInclude,
             &Engine::Neural::kNeuralRadianceCacheShaderInclude,
             &Graphic::kReflectionCaptureShaderInclude,
             &Graphic::kWorldParameterShaderInclude,
             &Graphic::kDebugMaterialShaderInclude,
        };
        return kIncludes;
    }
} // namespace Desert::ShaderProtocolGen
