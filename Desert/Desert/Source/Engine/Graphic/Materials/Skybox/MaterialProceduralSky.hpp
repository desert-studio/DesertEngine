#pragma once

#include <Engine/Graphic/Materials/Material.hpp>
#include <Engine/Graphic/Materials/Properties/StorageBufferProperty.hpp>
#include <Engine/Graphic/Materials/Properties/UniformBufferProperty.hpp>
#include <Engine/Core/Camera.hpp>
#include <Engine/Graphic/ShaderProtocols/Camera.hpp>
#include <Engine/ShaderResources/StorageBuffer.hpp>

#include <glm/glm.hpp>

#include <memory>

namespace Desert::Graphic
{
    // The engine-generated sky (no HDR asset): the "ProceduralSky" shader evaluates the shared atmosphere
    // model from the view ray and the sun direction. This material feeds only the shared CameraUB (for the
    // view-ray reconstruction) and rebinds the sky parameter SSBO.
    //
    // The buffer is EXTERNALLY owned — SkyboxRenderer creates and fills it, because the same buffer also
    // feeds the IBL bake's compute dispatch. The material only points
    // the descriptor at it, the way MaterialParticleBillboard does with the particle buffer.
    class MaterialProceduralSky final : public Material
    {
    public:
        MaterialProceduralSky() : Material( "MaterialProceduralSky", "ProceduralSky" )
        {
        }

        // The transmittance / sky-view LUTs are pass parameters (SkyboxRenderer::Render binds them through
        // RDG::PassBindings); the material owns only the camera block and the sky buffer.
        void Update( const Core::Camera* camera, const std::shared_ptr<ShaderResources::StorageBuffer>& skyParams )
        {
            if ( camera )
            {
                ShaderProtocols::Camera cameraUB;
                cameraUB.View       = camera->GetViewMatrix();
                cameraUB.Projection = camera->GetProjectionMatrix();
                cameraUB.CameraPos  = camera->GetPosition();
                if ( auto* ub = Get<UniformBufferProperty>( ShaderProtocols::Camera::Name ) )
                    ub->SetRawData( reinterpret_cast<const std::byte*>( &cameraUB ), sizeof( cameraUB ) );
            }

            if ( auto* sb = Get<StorageBufferProperty>( "SkyBuffer" ) )
                sb->SetBuffer( skyParams );
        }
    };
} // namespace Desert::Graphic
