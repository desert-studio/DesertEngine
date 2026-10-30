#pragma once

#include <Common/Core/Logger.hpp>
#include <Common/Core/ResultStr.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/Core/WorldContext.hpp>

namespace Desert::Libraries
{
    /// The world context of the running script; nullptr outside a script call, logged as
    /// "[Script] <library>.<function>: no world". The one copy every script library reads its world through.
    inline const Core::WorldContext* ScriptWorldContext( const char* library, const char* function )
    {
        const Core::WorldContext* context = Core::WorldContext::Current();
        if ( context == nullptr || context->World == nullptr )
        {
            LOG_ERROR( "[Script] {}.{}: no world — called outside a script call", library, function );
            return nullptr;
        }
        return context;
    }

    /// The running script's world; nullptr (logged) outside a script call.
    inline Core::Scene* ScriptScene( const char* library, const char* function )
    {
        const Core::WorldContext* context = ScriptWorldContext( library, function );
        return context != nullptr ? context->World : nullptr;
    }

    /// The running script's world, or the error "<library>.<call>: no world runs this call" for a library whose
    /// calls return a result instead of logging.
    inline Common::ResultStr<Core::Scene*> ScriptWorld( const char* library, const char* call )
    {
        const Core::WorldContext* context = Core::WorldContext::Current();
        if ( context == nullptr || context->World == nullptr )
            return Common::MakeFormattedError<Core::Scene*>( "{}.{}: no world runs this call", library, call );
        return Common::MakeSuccess( context->World );
    }
} // namespace Desert::Libraries
