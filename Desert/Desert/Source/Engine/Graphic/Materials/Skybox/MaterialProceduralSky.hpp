#pragma once

#include <Engine/Graphic/Materials/Material.hpp>
#include <Engine/Graphic/Materials/Properties/StorageBufferProperty.hpp>
#include <Engine/Graphic/Materials/Properties/UniformBufferProperty.hpp>
#include <Engine/Graphic/Materials/SceneLightingBinding.hpp>
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
        // `view` is the view the sky is drawn for (SceneRenderer::GetViewFrame); without one the camera block is
        // left as it is and nothing is drawn for it this frame.
        void Update( const ViewFrame* view, const std::shared_ptr<ShaderResources::StorageBuffer>& skyParams )
        {
            if ( view != nullptr )
                SceneCameraBind( this, *view );

            if ( auto* sb = Get<StorageBufferProperty>( "SkyBuffer" ) )
                sb->SetBuffer( skyParams );
        }
    };
} // namespace Desert::Graphic
