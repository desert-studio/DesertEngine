#include <Engine/Graphic/Systems/Scene/Skybox/SkyboxConvolution.hpp>

#include <Engine/Core/FrameManager.hpp>
#include <Engine/Graphic/Materials/Skybox/MaterialSkybox.hpp>
#include <Engine/Graphic/Systems/Scene/Skybox/SkyCapture.hpp>
#include <Engine/Runtime/Services/Image/ImageService.hpp>

#include <Common/Core/Logger.hpp>

#include <memory>
#include <optional>

namespace Desert::Graphic
{
    void AddSkyboxConvolutionPasses( MaterialSkybox& material, RDG::Builder& graph, FrameTextures& textures )
    {
        if ( !material.m_Convolution )
            return;
        const uint64_t frame = Engine::FrameManager::GetInstance().GetAbsoluteFrameCount();

        if ( material.m_Environment )
        {
            // Converted by an earlier frame's graph, which has been submitted: the writer's readbacks go after it.
            if ( frame != material.m_ConvertedFrame )
            {
                // The capture (and its programs) stays: a frame still in flight was recorded with its pipelines.
                EnvironmentManager::WriteCache( *material.m_Convolution, material.m_Environment );
                material.m_Convolution.reset();
            }
            return;
        }

        if ( !material.m_Capture )
        {
            // No instance: a specified cubemap's cubes are the skybox's (SkyboxService shares one material
            // between every scene that names the asset), counted under the Environment category only.
            material.m_Capture = std::make_shared<System::SkyCapture>( ResourceOwnerInstance{} );
            if ( const Common::BoolResultStr made = material.m_Capture->Initialize(); !made )
            {
                LOG_ERROR( "[Skybox] '{}' gets NO environment: {}", material.m_Environment.Filepath.string(),
                           made.GetError() );
                material.m_Convolution.reset();
                material.m_Capture.reset();
                return;
            }
        }
        auto* images = Runtime::GetImageService();
        if ( !material.m_Capture->BeginPanorama( System::SkyCapturePanorama{
                  .Panorama = images->Share( material.m_Convolution->Panorama->GetImageHandle() ),
                  .Source   = material.m_Environment.Filepath } ) )
        {
            LOG_ERROR( "[Skybox] '{}' gets NO environment: its conversion was not begun (see above).",
                       material.m_Environment.Filepath.string() );
            material.m_Convolution.reset();
            material.m_Capture.reset();
            return;
        }
        std::optional<Environment> converted =
             material.m_Capture->Settle( material.m_Capture->AddPasses( graph, textures, System::SkyCaptureFrameReads{} ) );
        if ( !converted )
        {
            LOG_ERROR( "[Skybox] '{}' gets NO environment: the frame graph refused its conversion (see above).",
                       material.m_Environment.Filepath.string() );
            material.m_Convolution.reset();
            material.m_Capture.reset();
            return;
        }
        material.m_Environment    = std::move( *converted );
        material.m_ConvertedFrame = frame;
        LOG_INFO(
             "[Skybox] '{}' was converted in this frame's graph (Sky Capture: Panorama to cube, radiance mips, "
             "irradiance, prefilter); its cache is written next frame.",
             material.m_Environment.Filepath.string() );
    }
} // namespace Desert::Graphic
