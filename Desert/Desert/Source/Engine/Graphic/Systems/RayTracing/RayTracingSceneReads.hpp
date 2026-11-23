#pragma once

#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/RDG/RDGShaderParameters.hpp>

namespace Desert::Graphic::RayTracing
{
    /**
     * @brief The view's scene reads a hit group makes beyond the camera block (Common/RayTracingSceneReads.glslh):
     * the world's parameter table and the view's rain occlusion, which the hits' weather reads
     * (Common/WeatherRain.glslh through Mesh/Surface/Pass_RayHit.glslh and Programs/Terrain/TerrainRayHit.shader).
     * A ray tracing pipeline's layout is its ray generation's, so every pass that traces the scene's hit group
     * table (PathTracer "PathTracing: Trace", SSRRenderer "SSR: Hit-lit trace") declares and binds these three
     * with the raster's own resources this frame: a reflection is never wetter or drier than the surface it shows.
     */
    struct RayTracingSceneReadsInputs
    {
        /// The view's WorldParameters table (SceneRenderer::UploadWorldParameters), imported into the graph.
        RDG::BufferRef WorldParameters;
        /// The view's rain occlusion map (SceneViewInputs::RainOcclusionMap: System.White without one).
        RDG::TextureRef RainOcclusionMap;
        /// Its RainOcclusionUB (RainOcclusionUniforms; Params.y = 0: no map), uploaded this frame.
        RDG::BufferRef RainOcclusion;

        [[nodiscard]] bool IsValid() const noexcept
        {
            return WorldParameters.IsValid() && RainOcclusionMap.IsValid() && RainOcclusion.IsValid();
        }
    };

// The slot names, spelled once for every tracing pass's parameter struct (the shader's names:
// Common/WorldParameters.glslh, Common/RainOcclusion.glslh).
#define DESERT_RAY_TRACING_SCENE_READ_PARAMETERS                                                                  \
    DESERT_SHADER_PARAMETER( StorageBuffer, WorldParameters );                                                    \
    DESERT_SHADER_PARAMETER( Texture, RainOcclusionMapTexture );                                                  \
    DESERT_SHADER_PARAMETER( Sampler, RainOcclusionSampler );                                                     \
    DESERT_SHADER_PARAMETER( UniformBuffer, RainOcclusionUB )

    /// Binds @p reads into @p block through @p Parameters (a struct that expanded
    /// DESERT_RAY_TRACING_SCENE_READ_PARAMETERS). The map is gathered 2x2 and compared by hand: nearest, clamped,
    /// as the raster binds it (FrameGraphRefs BindSceneViewInputs).
    template <typename Parameters, typename Block>
    Block& BindRayTracingSceneReads( Block& block, const RayTracingSceneReadsInputs& reads )
    {
        return block.Storage( Parameters::WorldParameters, reads.WorldParameters, RDG::Access::StorageRead )
             .Texture( Parameters::RainOcclusionMapTexture, reads.RainOcclusionMap, RDG::Access::SampledCompute,
                       RDG::SubresourceRange::All() )
             .Sampler( Parameters::RainOcclusionSampler, RDG::SamplerDesc::PointClamp() )
             .Uniform( Parameters::RainOcclusionUB, reads.RainOcclusion );
    }
} // namespace Desert::Graphic::RayTracing
