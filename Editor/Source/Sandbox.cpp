#include "Sandbox.hpp"
#include "EditorLayer.hpp"

namespace Desert
{

    Sandbox::Sandbox( const Engine::ApplicationInfo&                appinfo,
                      std::unique_ptr<Editor::Splash::SplashScreen> splash,
                      std::shared_ptr<Core::StartupProgress>        startup )
         : Engine::Application( appinfo ), m_Splash( std::move( splash ) ), m_StartupProgress( std::move( startup ) )
    {
    }

    void Sandbox::OnCreate()
    {
        PushLayer( std::make_unique<Editor::EditorLayer>( this, "", std::move( m_Splash ), m_StartupProgress ) );
    }

    void Sandbox::OnDestroy()
    {
    }

} // namespace Desert