#include <Engine/GameplayDebugger/GameplayDebuggerCategoryScript.hpp>

#if DESERT_DEV_INSTRUMENTS
#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/System/ScriptSystem.hpp>
#include <Engine/Scripting/ScriptEngine.hpp>

#include <algorithm>
#include <format>
#include <string>

namespace Desert::Debug
{
    namespace
    {
        const glm::vec4 kScriptLabel( 0.65f, 0.65f, 0.65f, 1.0f );
        const glm::vec4 kScriptValue( 1.0f, 1.0f, 1.0f, 1.0f );
        const glm::vec4 kScriptRunning( 0.35f, 1.0f, 0.35f, 1.0f );
        const glm::vec4 kScriptStopped( 0.55f, 0.55f, 0.55f, 1.0f );
        const glm::vec4 kScriptError( 1.0f, 0.35f, 0.25f, 1.0f );
        const glm::vec4 kScriptOverride( 1.0f, 0.85f, 0.2f, 1.0f );

        std::string ScriptValueText( const Scripting::ScriptValue& value )
        {
            using Scripting::PropertyType;
            switch ( value.Type )
            {
                case PropertyType::Number:
                    return std::format( "{:.3f}", value.Number );
                case PropertyType::Int:
                    return std::format( "{}", static_cast<long long>( value.Number ) );
                case PropertyType::Bool:
                    return value.Bool ? "true" : "false";
                case PropertyType::String:
                    return std::format( "\"{}\"", value.Str );
                case PropertyType::Enum:
                    return value.Str;
                case PropertyType::Color:
                case PropertyType::Vector:
                    return std::format( "{:.2f} {:.2f} {:.2f}", value.Vec[0], value.Vec[1], value.Vec[2] );
                case PropertyType::Asset:
                case PropertyType::Entity:
                case PropertyType::Array:
                    break; // never read live (LuauProperties::ReadScalar): shown by kind
            }
            return std::format( "<{}>", Scripting::PropertyTypeName( value.Type ) );
        }
    } // namespace

    void GameplayDebuggerCategoryScript::CollectData( const Core::Scene& scene, const ECS::Entity* targetEntity,
                                                      GameplayDebuggerCategoryData& out )
    {
        const entt::registry& registry = scene.GetRegistry();
        if ( targetEntity == nullptr || !registry.valid( targetEntity->GetHandle() ) )
        {
            out.AddTextLine( { { "no target", kScriptLabel } } );
            return;
        }
        const entt::entity target    = targetEntity->GetHandle();
        const auto*        component = registry.try_get<ECS::ScriptComponent>( target );
        if ( component == nullptr || component->Scripts.empty() )
        {
            out.AddTextLine( { { "no scripts", kScriptLabel } } );
            return;
        }

        // The scripts run while Play runs; in Edit the slots are shown as authored, with no live values.
        const auto*                     system = scene.FindSystem<ECS::ScriptSystem>();
        const Scripting::ScriptEngine* engine = system != nullptr ? &system->GetEngine() : nullptr;

        for ( uint32_t slot = 0; slot < component->Scripts.size(); ++slot )
        {
            const ECS::ScriptSlot& script = component->Scripts[slot];
            const std::string      file   = script.ScriptKey.empty()
                                                 ? std::string( "<no script>" )
                                                 : script.ResolvedPath().filename().string();
            const bool running = script.Started && script.RuntimeError.empty();
            out.AddTextLine( { { std::format( "Slot {} ", slot ), kScriptLabel },
                               { file, kScriptValue },
                               { running ? "  running" : script.Started ? "  failed" : "  not started",
                                 running ? kScriptRunning : script.Started ? kScriptError : kScriptStopped } } );
            if ( !script.RuntimeError.empty() )
                out.AddTextLine( { { "  Last error ", kScriptLabel }, { script.RuntimeError, kScriptError } } );

            if ( engine == nullptr || !script.Started )
                continue;
            const std::vector<Scripting::ScriptEngine::LiveProperty> live =
                 engine->ReadLiveProperties( static_cast<uint32_t>( target ), slot );
            for ( const Scripting::ScriptEngine::LiveProperty& property : live )
            {
                const bool overridden =
                     std::any_of( script.Properties.begin(), script.Properties.end(),
                                  [&]( const Scripting::ScriptProperty& p ) { return p.Name == property.Name; } );
                const std::string value =
                     property.Value.has_value() ? ScriptValueText( *property.Value )
                                                : std::format( "<{}>", Scripting::PropertyTypeName( property.Type ) );
                out.AddTextLine( { { std::format( "  {} = ", property.Name ), kScriptLabel },
                                   { value, overridden ? kScriptOverride : kScriptValue } } );
            }
        }
    }
} // namespace Desert::Debug
#endif // DESERT_DEV_INSTRUMENTS
