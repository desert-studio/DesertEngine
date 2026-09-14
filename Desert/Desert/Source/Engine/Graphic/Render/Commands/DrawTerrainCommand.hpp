#pragma once

#include "../RenderCommand.hpp"
// The renderer itself: every Execute below calls a method on it, and RenderCommand.hpp only
// forward-declares the type (see the note there on the include cycle that cost).
#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Graphic/Materials/MaterialOverrides.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <utility>

namespace Desert::Graphic
{
    class Image2D;
}

namespace Desert::Graphic::Render
{
    // Carries one terrain entity's transform + params from the ECS (TerrainComponent) to the
    // TerrainRenderer (via SceneRenderer::SubmitTerrain) for this frame. ParamOverrides come from an
    // optional MaterialComponent on the same entity (name -> value), applied generically by name.
    struct DrawTerrainCommand : RenderCommand
    {
        glm::mat4 Transform;
        float     Size;
        int       Resolution;
        float     HeightScale;
        float     NoiseFrequency;
        int       Seed;

        glm::vec3 LayerModes = glm::vec3( 0.0f ); // grass/rock/snow: 0=Auto, 1=Manual, 2=Off
        Image2D*  SplatMap   = nullptr;           // per-terrain painted splat map (non-owning)

        Graphic::MaterialOverrides Overrides;

        DrawTerrainCommand( const glm::mat4& transform, float size, int resolution, float heightScale,
                            float noiseFrequency, int seed, const glm::vec3& layerModes = glm::vec3( 0.0f ),
                            Image2D* splatMap = nullptr, Graphic::MaterialOverrides overrides = {} )
             : Transform( transform ), Size( size ), Resolution( resolution ), HeightScale( heightScale ),
               NoiseFrequency( noiseFrequency ), Seed( seed ), LayerModes( layerModes ), SplatMap( splatMap ),
               Overrides( std::move( overrides ) )
        {
        }

        void Execute( SceneRenderer& renderer ) override
        {
            renderer.SubmitTerrain( Transform, Size, Resolution, HeightScale, NoiseFrequency, Seed, LayerModes,
                                    SplatMap, Overrides );
        }
    };
} // namespace Desert::Graphic::Render
