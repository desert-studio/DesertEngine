#include <Engine/Graphic/Environment/SceneSkyEnvironment.hpp>

#include <Common/Core/Logger.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

namespace Desert::Graphic
{
    bool SceneSkyEnvironment::BeginViewFrame( const uint64_t frame )
    {
        if ( m_HasFrame && frame == m_Frame )
            return false;
        m_Frame    = frame;
        m_HasFrame = true;
        return true;
    }

    bool SceneSkyEnvironment::TakeBakeRequest()
    {
        const bool requested = m_BakeRequested;
        m_BakeRequested      = false;
        return requested;
    }

    void SceneSkyEnvironment::ObserveSun( const glm::vec3& sunDir, const float deltaSeconds )
    {
        const float dt = glm::max( deltaSeconds, 0.0f );
        if ( glm::dot( m_LastSeenSunDir - sunDir, m_LastSeenSunDir - sunDir ) > 1e-12f )
        {
            m_LastSeenSunDir       = sunDir;
            m_SecondsSinceSunMoved = 0.0f;
        }
        else
        {
            m_SecondsSinceSunMoved += dt;
        }
    }

    bool SceneSkyEnvironment::TakeFirstHighResReport()
    {
        if ( m_HighResCostLogged )
            return false;
        m_HighResCostLogged = true;
        return true;
    }

    void SceneSkyEnvironment::Submit( std::unique_ptr<ProceduralEnvironmentBake> bake, const SkyBakeInputs& inputs,
                                      const SkyEnvironmentSize size, const char* what )
    {
        m_PendingBake    = std::move( bake );
        m_Pending        = inputs;
        m_PendingSize    = size;
        m_PendingWhat    = what;
        m_PendingStarted = std::chrono::steady_clock::now();

        // A sky that has no environment yet lands this one in the same call when the GPU is already done —
        // never by waiting for it.
        LandPendingBake();
    }

    void SceneSkyEnvironment::SettlePendingBake()
    {
        if ( !m_PendingBake )
            return;
        m_PendingBake->Wait();
        LandPendingBake();
    }

    bool SceneSkyEnvironment::LandPendingBake()
    {
        if ( !m_PendingBake )
            return true;
        if ( !m_PendingBake->IsComplete() )
            return false;

        Environment baked = m_PendingBake->Finish();
        m_PendingBake.reset();
        if ( !baked )
        {
            LOG_ERROR( "[SkyAtmosphere] Environment bake at {}x{} came back empty — the previous environment "
                       "is kept.",
                       m_PendingSize.Width, m_PendingSize.Height );
            return true;
        }

        // The previous environment's cubes are released by the replacement (the image service owns them
        // until unregistered); the last one is released when the scene dies.
        m_ProceduralEnv.Replace( *Runtime::ResourceRegistry::GetImageService(), std::move( baked ) );
        m_Baked = m_Pending;

        const double bakeMs =
             std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - m_PendingStarted )
                  .count();
        LOG_INFO( "[SkyAtmosphere] Environment of {} baked at {}x{}, landed {:.1f} ms after submit — {}. The "
                  "main thread did not wait for it; the trigger is still the sun, the clouds and the sky's own "
                  "parameters and not the frame.",
                  ResourceOwnerInstanceName( m_Scene ), m_PendingSize.Width, m_PendingSize.Height, bakeMs,
                  m_PendingWhat );
        return true;
    }
} // namespace Desert::Graphic
