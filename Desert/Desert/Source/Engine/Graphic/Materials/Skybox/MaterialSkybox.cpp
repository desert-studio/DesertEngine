#include "MaterialSkybox.hpp"

#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Graphic/ShaderProtocols/Camera.hpp>
#include <Engine/Graphic/Materials/SceneLightingBinding.hpp>

#include <Common/Core/Logger.hpp>

namespace Desert::Graphic
{
    MaterialSkybox::MaterialSkybox( const std::shared_ptr<Assets::SkyboxAsset>& baseAsset )
         : Material( "MaterialSkybox", "Skybox" ), m_BaseMaterial( baseAsset )
    {
        m_CubeMapTexture = m_MaterialExecutor->GetTextureCubeProperty( "samplerCubeMap" ).get();
        // The FILE as authored — identity rotation, white tint, unit intensity — and that is ALL it will
        // ever be: the scene's look is applied where the cubes are sampled (BindInputs below, and
        // SceneEnvironmentBind for the lit materials), so there is no second bake for it to trigger.
        m_Environment = Graphic::EnvironmentManager::Create( baseAsset );
    }

    void MaterialSkybox::BindInputs( const UpdateMaterialSkyboxInfo& data )
    {
        if ( !m_Environment.RadianceMap.IsValid() ||
             m_Environment.RadianceMap.ImageType != Runtime::ImageHandle::Type::ImageCube )
        {
            return;
        }

        auto* image = static_cast<ImageCube*>(
             Runtime::ResourceRegistry::GetImageService()->Resolve( m_Environment.RadianceMap ) );
        if ( !image )
            return;

        if ( m_CubeMapTexture )
            m_CubeMapTexture->SetTexture( image );

        static ShaderProtocols::Camera CameraUB;
        CameraUB.View       = data.Camera->GetViewMatrix();
        CameraUB.Projection = data.Camera->GetProjectionMatrix();
        CameraUB.CameraPos  = data.Camera->GetPosition();

        Get<UniformBufferProperty>( CameraUB.Name )
             ->SetRawData( reinterpret_cast<std::byte*>( &CameraUB ), sizeof( ShaderProtocols::Camera ) );

        // THE SCENE'S LOOK, per draw — the same value, through the same struct, that the lit materials
        // receive (SceneEnvironmentBind), so the backdrop and the light it casts are one sky. It used to be
        // baked into the cube, which made every slider value a device-idling rebake.
        SceneSkyLookBind( this, data.Look );
    }

} // namespace Desert::Graphic
