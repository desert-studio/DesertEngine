#include "MaterialSkybox.hpp"

#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Graphic/ShaderProtocols/Camera.hpp>
#include <Engine/Graphic/Renderer.hpp>

#include <Common/Core/Logger.hpp>

namespace Desert::Graphic
{
    MaterialSkybox::MaterialSkybox( const std::shared_ptr<Assets::SkyboxAsset>& baseAsset )
         : Material( "MaterialSkybox", "Skybox" ), m_BaseMaterial( baseAsset )
    {
        m_CubeMapTexture = m_MaterialExecutor->GetTextureCubeProperty( "samplerCubeMap" ).get();
        // The FILE as authored — identity rotation, white tint, unit intensity. The scene's own look
        // arrives through EnsureBaked on the first frame the component is seen, which is also the only
        // place that knows it: this constructor runs from the skybox service, which registers an ASSET
        // and has never heard of the component that points at it.
        m_Environment = Graphic::EnvironmentManager::Create( baseAsset, m_BakedLook );
    }

    void MaterialSkybox::ReleaseEnvironment( const Environment& previous )
    {
        if ( !previous )
            return;

        auto* imageService = Runtime::ResourceRegistry::GetImageService();
        imageService->Unregister( previous.RadianceMap );
        imageService->Unregister( previous.IrradianceMap );
        imageService->Unregister( previous.PreFilteredMap );
    }

    bool MaterialSkybox::EnsureBaked( const SkyLook& look )
    {
        if ( look == m_BakedLook && m_Environment )
            return false;

        const auto base = m_BaseMaterial.lock();
        if ( !base )
            return false;

        // WAIT FIRST. The cubes about to be released are bound into descriptor sets of frames that may
        // still be executing — the same order, and the same reason, as the skybox picker's own
        // WaitDeviceIdle before it registers a new asset.
        Renderer::GetInstance().WaitDeviceIdle();

        Environment rebaked = Graphic::EnvironmentManager::Create( base, look );
        if ( !rebaked )
        {
            // KEEP THE OLD SKY AND SAY SO. An empty Environment here would take the scene's ambient,
            // its reflections and its backdrop away at once, on an edit that was only meant to turn the
            // sun a few degrees — and EnvironmentManager::Create has already logged the cause.
            LOG_ERROR( "[Skybox] '{}' was not rebaked for rotation {:.1f} deg, intensity {:.2f}; the "
                       "previous sky is kept.",
                       base->GetMetadata().Filepath.string(), look.RotationDegrees, look.Intensity );
            return false;
        }

        const Environment previous = m_Environment;
        m_Environment              = rebaked;
        m_BakedLook                = look;
        ReleaseEnvironment( previous );

        // The binding is refreshed on the next BindInputs, but the CACHED pointer is not: the property
        // holds the old ImageCube until something writes it, and that old cube has just been
        // unregistered. Clear it here so a frame between the rebake and the next bind cannot resolve it.
        if ( m_CubeMapTexture )
            m_CubeMapTexture->SetTexture( nullptr );

        return true;
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

        // NO BRIGHTNESS UNIFORM ANY MORE, and its removal is the point rather than a tidy-up. The
        // multiplier used to be applied HERE, to the fullscreen sky pass alone, which is why a skybox
        // authored at intensity 5 lit nothing: the irradiance and prefiltered cubes every surface reads
        // never heard of it. Intensity, tint and rotation now live in the cubes themselves
        // (EnvironmentManager::Create), so the backdrop is drawn from the very image that lights the
        // world and the two cannot describe different skies.
    }

} // namespace Desert::Graphic
