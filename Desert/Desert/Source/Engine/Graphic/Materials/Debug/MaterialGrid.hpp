#pragma once

#include <Engine/Graphic/Materials/Material.hpp>
#include <Engine/Graphic/Materials/Properties/UniformBufferProperty.hpp>
#include <Engine/Core/Camera.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_inverse.hpp>

namespace Desert::Graphic
{
    // Infinite ground-plane grid material. Feeds the "Grid" shader a single UB with the camera matrices
    // (+ inverses, for the per-pixel world ray) and grid appearance params. Drawn as a fullscreen triangle.
    class MaterialGrid final : public Material
    {
    public:
        MaterialGrid() : Material( "MaterialGrid", "Grid" )
        {
        }

        void Update( const Core::Camera* camera )
        {
            if ( !camera )
                return;

            GridUB data{};
            data.Projection    = camera->GetProjectionMatrix();
            data.View          = camera->GetViewMatrix();
            data.InvProjection = glm::inverse( data.Projection );
            data.InvView       = glm::inverse( data.View );
            data.CameraPos     = glm::vec4( camera->GetPosition(), 1.0f );
            data.ThinColor     = glm::vec4( 0.40f, 0.40f, 0.46f, 0.45f );
            data.ThickColor    = glm::vec4( 0.55f, 0.55f, 0.62f, 0.75f );
            data.Params        = glm::vec4( 100.0f, 4000.0f, 40000.0f, 0.0f ); // baseCell, fadeStart, fadeEnd (cm)

            if ( auto* ub = Get<UniformBufferProperty>( "GridUB" ) )
                ub->SetRawData( reinterpret_cast<const std::byte*>( &data ), sizeof( data ) );
        }

    private:
        struct GridUB
        {
            glm::mat4 Projection;
            glm::mat4 View;
            glm::mat4 InvProjection;
            glm::mat4 InvView;
            glm::vec4 CameraPos;
            glm::vec4 ThinColor;
            glm::vec4 ThickColor;
            glm::vec4 Params;
        };
    };
} // namespace Desert::Graphic
