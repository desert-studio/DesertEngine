#include "Sandbox.hpp"
#include "EditorLayer.hpp"

namespace Desert
{

    Sandbox::Sandbox( const Engine::ApplicationInfo& appinfo, std::unique_ptr<Editor::Splash::SplashScreen> splash )
         : Engine::Application( appinfo ), m_Splash( std::move( splash ) )
    {
    }

    void Sandbox::OnCreate()
    {
        PushLayer( std::make_unique<Editor::EditorLayer>( this, "", std::move( m_Splash ) ) );
    }

    void Sandbox::OnDestroy()
    {
    }

} // namespace Desert