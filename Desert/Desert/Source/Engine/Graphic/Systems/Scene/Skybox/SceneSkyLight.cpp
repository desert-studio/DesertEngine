#include "SceneSkyLight.hpp"

#include <Common/Core/Logger.hpp>
#include <Engine/Core/FrameManager.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <algorithm>

namespace Desert::Graphic
{
    bool SceneSkyLight::ClaimFrame( const void* view, const glm::vec3& sunDir, float deltaSeconds )
    {
        const uint64_t frame = Engine::FrameManager::GetInstance().GetAbsoluteFrameCount();
        if ( frame == m_ClaimFrame )
            return m_Claimant == view;
        m_ClaimFrame = frame;
        m_Claimant   = view;

        // How long the sun has held still, once per frame however many views ask. Compared by direction rather
        // than by "did anything write it", because the time-of-day driver rewrites the same value every frame
        // when it is paused.
        const glm::vec3 moved = m_LastSeenSunDir - sunDir;
        if ( glm::dot( moved, moved ) > 1e-12f )
        {
            m_LastSeenSunDir       = sunDir;
            m_SecondsSinceSunMoved = 0.0f;
        }
        else
        {
            m_SecondsSinceSunMoved += std::max( deltaSeconds, 0.0f );
        }
        return true;
    }

    bool SceneSkyLight::Claims( const void* view ) const
    {
        return m_Claimant == view && m_ClaimFrame == Engine::FrameManager::GetInstance().GetAbsoluteFrameCount();
    }

    bool SceneSkyLight::EnsurePrograms()
    {
        if ( m_ProgramsMade )
            return true;
        if ( m_ProgramsFailed )
            return false;
        if ( const Common::BoolResultStr made = m_Capture.Initialize(); !made )
        {
            LOG_ERROR( "[SkyAtmosphere] this scene's procedural sky gives no sky light: {}", made.GetError() );
            m_ProgramsFailed = true;
            return false;
        }
        m_ProgramsMade = true;
        return true;
    }

    bool SceneSkyLight::Begin( const SkyEnvironmentSize& size, const System::SkyCaptureSource& source,
                               const SkyLightStamps& from, const char* what )
    {
        const System::SkyCapturePace pace =
             m_Environment ? System::SkyCapturePace::TimeSliced : System::SkyCapturePace::Whole;
        if ( !m_Capture.Begin( size, pace, source ) )
            return false;
        m_Pending        = from;
        m_PendingSize    = size;
        m_PendingWhat    = what;
        m_PendingStarted = std::chrono::steady_clock::now();
        m_PendingFrames  = 0;
        return true;
    }

    void SceneSkyLight::AddPasses( const void* view, RDG::Builder& graph, FrameTextures& textures,
                                   const System::SkyCaptureFrameReads& reads )
    {
        if ( !m_Capture.IsCapturing() || !Claims( view ) )
            return;

        ++m_PendingFrames;
        std::optional<Environment> captured = m_Capture.Settle( m_Capture.AddPasses( graph, textures, reads ) );
        if ( !captured )
            return;

        // The previous environment's cubes are released by the replacement (the image service owns them until
        // unregistered; their destruction is deferred past the frames in flight). From here on every view of the
        // scene reads the new cubes -- this view's ImportSceneViewTextures, which comes after this, included.
        m_Environment.Replace( *Runtime::ResourceRegistry::GetImageService(), std::move( *captured ) );
        m_Captured = m_Pending;

        const double spanMs =
             std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - m_PendingStarted )
                  .count();
        LOG_INFO( "[SkyAtmosphere] Sky light captured at {}x{} in the frame graph over {} frame(s) ({:.1f} ms of "
                  "wall time from its begin), once for every view of the scene -- {}. The trigger is the sun, the "
                  "clouds and the sky's own parameters.",
                  m_PendingSize.Width, m_PendingSize.Height, m_PendingFrames, spanMs, m_PendingWhat );
    }
} // namespace Desert::Graphic
