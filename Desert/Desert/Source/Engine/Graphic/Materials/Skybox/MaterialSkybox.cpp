#include "MaterialSkybox.hpp"

#include <Engine/Core/FrameManager.hpp>
#include <Engine/Graphic/Systems/Scene/Skybox/SkyCapture.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
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
        m_Environment = Graphic::EnvironmentManager::Create( baseAsset, m_Convolution );
    }

    MaterialSkybox::~MaterialSkybox() = default;

    void MaterialSkybox::AddConvolutionPasses( RDG::Builder& graph, FrameTextures& textures )
    {
        if ( !m_Convolution )
            return;
        const uint64_t frame = Engine::FrameManager::GetInstance().GetAbsoluteFrameCount();

        if ( m_Environment )
        {
            // Converted by an earlier frame's graph, which has been submitted: the writer's readbacks go after it.
            if ( frame != m_ConvertedFrame )
            {
                // The capture (and its programs) stays: a frame still in flight was recorded with its pipelines.
                EnvironmentManager::WriteCache( *m_Convolution, m_Environment );
                m_Convolution.reset();
            }
            return;
        }

        if ( !m_Capture )
        {
            m_Capture = std::make_unique<System::SkyCapture>();
            if ( const Common::BoolResultStr made = m_Capture->Initialize(); !made )
            {
                LOG_ERROR( "[Skybox] '{}' gets NO environment: {}", m_Environment.Filepath.string(), made.GetError() );
                m_Convolution.reset();
                m_Capture.reset();
                return;
            }
        }
        auto* images = Runtime::ResourceRegistry::GetImageService();
        if ( !m_Capture->BeginPanorama( System::SkyCapturePanorama{
                  .Panorama = images->Share( m_Convolution->Panorama->GetImageHandle() ),
                  .Source   = m_Environment.Filepath } ) )
        {
            LOG_ERROR( "[Skybox] '{}' gets NO environment: its conversion was not begun (see above).",
                       m_Environment.Filepath.string() );
            m_Convolution.reset();
            m_Capture.reset();
            return;
        }
        std::optional<Environment> converted =
             m_Capture->Settle( m_Capture->AddPasses( graph, textures, System::SkyCaptureFrameReads{} ) );
        if ( !converted )
        {
            LOG_ERROR( "[Skybox] '{}' gets NO environment: the frame graph refused its conversion (see above).",
                       m_Environment.Filepath.string() );
            m_Convolution.reset();
            m_Capture.reset();
            return;
        }
        m_Environment    = std::move( *converted );
        m_ConvertedFrame = frame;
        LOG_INFO( "[Skybox] '{}' was converted in this frame's graph (Sky Capture: Panorama to cube, radiance mips, "
                  "irradiance, prefilter); its cache is written next frame.",
                  m_Environment.Filepath.string() );
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

        if ( data.View != nullptr )
            SceneCameraBind( this, *data.View );

        // THE SCENE'S LOOK, per draw — the same value, through the same struct, that the lit materials
        // receive (SceneEnvironmentBind), so the backdrop and the light it casts are one sky. It used to be
        // baked into the cube, which made every slider value a device-idling rebake.
        SceneSkyLookBind( this, data.Look );
    }

} // namespace Desert::Graphic
