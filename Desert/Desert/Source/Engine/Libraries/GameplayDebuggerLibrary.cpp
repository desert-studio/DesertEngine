#include <Engine/Libraries/GameplayDebuggerLibrary.hpp>

#include <Common/Core/DevInstruments.hpp>
#include <Common/Core/Logger.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/Core/WorldContext.hpp>

namespace Desert::Libraries
{
#if DESERT_DEV_INSTRUMENTS
    namespace
    {
        // The world of the running script; nullptr (logged) outside a script call.
        Core::Scene* DebuggerWorld( const char* function )
        {
            const Core::WorldContext* context = Core::WorldContext::Current();
            if ( context == nullptr || context->World == nullptr )
            {
                LOG_ERROR( "[Script] GameplayDebugger.{}: no world — called outside a script call", function );
                return nullptr;
            }
            return context->World;
        }
    } // namespace

    void GameplayDebuggerLibrary::SetActive( const bool active )
    {
        if ( Core::Scene* scene = DebuggerWorld( "setActive" ) )
            scene->GetGameplayDebugger().SetActive( active );
    }

    bool GameplayDebuggerLibrary::IsActive()
    {
        const Core::Scene* scene = DebuggerWorld( "isActive" );
        return scene != nullptr && scene->GetGameplayDebugger().IsActive();
    }

    bool GameplayDebuggerLibrary::SetCategory( const std::string& name, const bool enabled )
    {
        Core::Scene* scene = DebuggerWorld( "setCategory" );
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
