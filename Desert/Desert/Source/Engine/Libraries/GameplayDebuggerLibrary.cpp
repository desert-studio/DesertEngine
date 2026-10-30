#include <Engine/Libraries/GameplayDebuggerLibrary.hpp>

#include <Common/Core/DevInstruments.hpp>
#include <Common/Core/Logger.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/Core/WorldContext.hpp>
#include <Engine/Libraries/ScriptWorld.hpp>

namespace Desert::Libraries
{
#if DESERT_DEV_INSTRUMENTS
    void GameplayDebuggerLibrary::SetActive( const bool active )
    {
        if ( Core::Scene* scene = ScriptScene( "GameplayDebugger", "setActive" ) )
            scene->GetGameplayDebugger().SetActive( active );
    }

    bool GameplayDebuggerLibrary::IsActive()
    {
        const Core::Scene* scene = ScriptScene( "GameplayDebugger", "isActive" );
        return scene != nullptr && scene->GetGameplayDebugger().IsActive();
    }

    bool GameplayDebuggerLibrary::SetCategory( const std::string& name, const bool enabled )
    {
        Core::Scene* scene = ScriptScene( "GameplayDebugger", "setCategory" );
        if ( scene == nullptr )
            return false;
        if ( scene->GetGameplayDebugger().SetCategoryEnabled( name, enabled ) )
            return true;
        LOG_WARN( "[Script] GameplayDebugger.setCategory: no category named '{}'", name );
        return false;
    }
#else
    void GameplayDebuggerLibrary::SetActive( bool )
    {
    }

    bool GameplayDebuggerLibrary::IsActive()
    {
        return false;
    }

    bool GameplayDebuggerLibrary::SetCategory( const std::string&, bool )
    {
        return false;
    }
#endif
} // namespace Desert::Libraries
