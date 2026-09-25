#pragma once

#include <Engine/Graphic/Materials/Material.hpp>
#include <Engine/Graphic/Materials/Properties/UniformBufferProperty.hpp>
#include <Engine/Graphic/Materials/Properties/TextureCubeProperty.hpp>
#include <Engine/Graphic/Materials/SceneLightingBinding.hpp>
#include <Engine/Core/Camera.hpp>

#include <glm/glm.hpp>

namespace Desert::Graphic
{
    // Feeds the "CubemapSphere" shader: the Material Editor's cubemap-on-a-sphere presenter (the pane's
    // answer for a Skybox-domain material — see the shader for why the ball is ray-traced from a quad).
    // One UB with the camera matrices + inverses for the per-pixel world ray, the sphere radius, and the
    // cube to wrap. Beside MaterialGrid because it is the same kind of object: an editor pass's material,
    // engine-owned so the pass code stays draw logic only.
    class MaterialCubemapSphere final : public Material
    {
    public:
        MaterialCubemapSphere() : Material( "MaterialCubemapSphere", "CubemapSphere" )
        {
        }

        // @p cube may not be null — the pass skips its draw entirely when the material resolves no cube,
        // so "no cubemap" is a named refusal in the panel rather than a black ball here.
        void Update( const Core::Camera* camera, const ImageCube* cube, const SkyLook& look,
                     float radiusWorldUnits, bool cubeIsBackdrop )
        {
            if ( !camera || !cube )
                return;

            CubemapSphereUB data{};
            data.Projection    = camera->GetProjectionMatrix();
            data.View          = camera->GetViewMatrix();
            data.InvProjection = glm::inverse( data.Projection );
            data.InvView       = glm::inverse( data.View );
            data.CameraPos     = glm::vec4( camera->GetPosition(), 1.0f );
            data.Params        = glm::vec4( radiusWorldUnits, cubeIsBackdrop ? 1.0f : 0.0f, 0.0f, 0.0f );

            if ( auto* ub = Get<UniformBufferProperty>( "CubemapSphereUB" ) )
                ub->SetRawData( reinterpret_cast<const std::byte*>( &data ), sizeof( data ) );
            if ( auto* tex = Get<TextureCubeProperty>( "u_CubeMap" ) )
                tex->SetTexture( cube );
            // The ball shows the sky the scene shows: a Details-panel preview beside a Rotation slider
            // that did not turn would read as a slider that does nothing.
            SceneSkyLookBind( this, look );
        }

    private:
        struct CubemapSphereUB
        {
            glm::mat4 Projection;
            glm::mat4 View;
            glm::mat4 InvProjection;
            glm::mat4 InvView;
            glm::vec4 CameraPos;
            glm::vec4 Params; // x = sphere radius (world units), y = cube is the backdrop (0/1)
        };
    };
} // namespace Desert::Graphic
