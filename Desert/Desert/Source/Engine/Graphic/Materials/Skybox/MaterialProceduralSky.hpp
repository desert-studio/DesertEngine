#pragma once

#include <Engine/Graphic/Materials/Material.hpp>
#include <Engine/Graphic/Materials/Properties/StorageBufferProperty.hpp>
#include <Engine/Graphic/Materials/Properties/Texture2DProperty.hpp>
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

        // @p transmittanceLut / @p skyViewLut back the PhysicalAtmosphere branch and are owned by
        // SkyboxRenderer; null (the gradient model, or a frame before the graph has written them) binds the
        // shader's default textures — the gradient branch never samples them.
        void Update( const Core::Camera* camera, const std::shared_ptr<ShaderResources::StorageBuffer>& skyParams,
                     const Image2D* transmittanceLut, const Image2D* skyViewLut )
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

            // Both LUTs or neither: the sky pass passes them only in a frame whose graph node declared them. A
            // frame that does not goes back to the shader's own defaults, so a LUT is never left bound in a
            // frame whose graph has not put it in a sampled layout.
            if ( transmittanceLut && skyViewLut )
            {
                if ( auto* tex = Get<Texture2DProperty>( "u_TransmittanceLut" ) )
                    tex->SetImage( transmittanceLut, RDG::Access::SampledGraphics );
                if ( auto* tex = Get<Texture2DProperty>( "u_SkyViewLut" ) )
                    tex->SetImage( skyViewLut, RDG::Access::SampledGraphics );
                m_LutsBound = true;
            }
            else if ( m_LutsBound )
            {
                BindSchemaDefaultTexture( "u_TransmittanceLut" );
                BindSchemaDefaultTexture( "u_SkyViewLut" );
                m_LutsBound = false;
            }
        }

    private:
        bool m_LutsBound = false;
    };
} // namespace Desert::Graphic
