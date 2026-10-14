#include <Engine/Libraries/InputLibrary.hpp>

#include <Common/Core/KeyCodes.hpp>
#include <Engine/Core/WorldContext.hpp>
#include <Engine/Input/EnhancedInputSubsystem.hpp>
#include <Engine/Input/LocalPlayerInput.hpp>
#include <Engine/Reflection/FunctionThunk.hpp>

#include <format>

#include <unordered_map>

namespace Desert::Libraries
{
    namespace
    {
        // Script-friendly key names -> engine key codes: single characters cover the alphabet + digits, named
        // keys the common gameplay set.
        std::optional<Common::KeyCode> KeyFromName( const std::string& name )
        {
            using K = Common::KeyCode;
            if ( name.size() == 1 )
            {
                const char c = name[0];
                if ( c >= 'A' && c <= 'Z' )
                    return static_cast<K>( c );
                if ( c >= 'a' && c <= 'z' )
                    return static_cast<K>( c - 'a' + 'A' );
                if ( c >= '0' && c <= '9' )
                    return static_cast<K>( c ); // KeyCode::D0..D9 == ASCII '0'..'9'
            }
            static const std::unordered_map<std::string, K> kNamed = { { "Space", K::Space },
                                                                       { "Shift", K::LeftShift },
                                                                       { "LeftShift", K::LeftShift },
                                                                       { "Ctrl", K::LeftControl },
                                                                       { "LeftControl", K::LeftControl },
                                                                       { "Alt", K::LeftAlt },
                                                                       { "LeftAlt", K::LeftAlt },
                                                                       { "Tab", K::Tab },
                                                                       { "Enter", K::Enter },
                                                                       { "Escape", K::Escape },
                                                                       { "Left", K::Left },
                                                                       { "Right", K::Right },
                                                                       { "Up", K::Up },
                                                                       { "Down", K::Down } };
            const auto                                      it     = kNamed.find( name );
            return it == kNamed.end() ? std::nullopt : std::optional<K>( it->second );
        }

        struct FrameState
        {
            glm::vec2           MouseDelta{ 0.0f };
            std::optional<bool> CursorLockRequest;
        };

        FrameState& State()
        {
            static FrameState state;
            return state;
        }

        // The playing world's local player input, or nullptr with the call failed naming @p function.
        Input::LocalPlayerInput* PlayerInput( const char* function )
        {
            const Core::WorldContext* context = Core::WorldContext::Current();
            if ( context == nullptr || context->PlayerInput == nullptr )
            {
                Reflection::RaiseCallError( std::format( "Input.{}: no world is being played", function ) );
                return nullptr;
            }
            return context->PlayerInput;
        }

        // The action a script names, when a context the player added maps it; otherwise the call fails naming it.
        std::optional<Common::Content::AssetGuid> Action( const char* function, const std::string& name,
                                                          Input::LocalPlayerInput*& input )
        {
            input = PlayerInput( function );
            if ( input == nullptr )
                return std::nullopt;
            auto guid = input->ActionNamed( name );
            if ( !guid )
                Reflection::RaiseCallError( std::format(
                     "Input.{}: no mapping context the player added maps an action named '{}'", function, name ) );
            return guid;
        }

        template <bool Input::TriggerEvents::*Event>
        bool ActionEvent( const char* function, const std::string& name )
        {
            Input::LocalPlayerInput* input = nullptr;
            const auto               guid  = Action( function, name, input );
            return guid.has_value() && input->Subsystem().GetTriggerEvents( *guid ).*Event;
        }
    } // namespace

    // The keys are the local player's (LocalPlayerInput's key state, fed by the viewport in focus), never the
    // window's: a key typed into a text field elsewhere is not down for gameplay.
    bool InputLibrary::IsKeyDown( const std::string& key )
    {
        const auto               code  = KeyFromName( key );
        Input::LocalPlayerInput* input = code.has_value() ? PlayerInput( "isKeyDown" ) : nullptr;
        return input != nullptr && input->IsKeyDown( Input::InputKey::Of( *code ) );
    }

    bool InputLibrary::WasPressed( const std::string& key )
    {
        const auto               code  = KeyFromName( key );
        Input::LocalPlayerInput* input = code.has_value() ? PlayerInput( "wasPressed" ) : nullptr;
        return input != nullptr && input->WasKeyPressed( Input::InputKey::Of( *code ) );
    }

    glm::vec2 InputLibrary::MouseDelta()
    {
        return State().MouseDelta;
    }

    void InputLibrary::LockCursor()
    {
        State().CursorLockRequest = true;
    }

    void InputLibrary::ShowCursor()
    {
        State().CursorLockRequest = false;
    }

    bool InputLibrary::IsMouseDown( const std::string& button )
    {
        Common::MouseButton which = Common::MouseButton::Left;
        if ( button == "right" )
            which = Common::MouseButton::Right;
        else if ( button == "middle" )
            which = Common::MouseButton::Middle;
        Input::LocalPlayerInput* input = PlayerInput( "isMouseDown" );
        return input != nullptr && input->IsKeyDown( Input::InputKey::Of( which ) );
    }

    glm::vec3 InputLibrary::ActionValue( const std::string& action )
    {
        Input::LocalPlayerInput* input = nullptr;
        const auto               guid  = Action( "actionValue", action, input );
        return guid ? input->Subsystem().GetActionValue( *guid ) : glm::vec3( 0.0f );
    }

    bool InputLibrary::ActionTriggered( const std::string& action )
    {
        return ActionEvent<&Input::TriggerEvents::Triggered>( "actionTriggered", action );
    }

    bool InputLibrary::ActionStarted( const std::string& action )
    {
        return ActionEvent<&Input::TriggerEvents::Started>( "actionStarted", action );
    }

    bool InputLibrary::ActionOngoing( const std::string& action )
    {
        return ActionEvent<&Input::TriggerEvents::Ongoing>( "actionOngoing", action );
    }

    bool InputLibrary::ActionCompleted( const std::string& action )
    {
        return ActionEvent<&Input::TriggerEvents::Completed>( "actionCompleted", action );
    }

    bool InputLibrary::ActionCanceled( const std::string& action )
    {
        return ActionEvent<&Input::TriggerEvents::Canceled>( "actionCanceled", action );
    }

    float InputLibrary::ActionSeconds( const std::string& action )
    {
        Input::LocalPlayerInput* input = nullptr;
        const auto               guid  = Action( "actionSeconds", action, input );
        return guid ? input->Subsystem().GetTriggeredSeconds( *guid ) : 0.0f;
    }

    void InputLibrary::AddContext( const std::string& context, int priority )
    {
        Input::LocalPlayerInput* input = PlayerInput( "addContext" );
        if ( input == nullptr )
            return;
        const Core::WorldContext* world = Core::WorldContext::Current();
        if ( world->Assets == nullptr )
        {
            Reflection::RaiseCallError( "Input.addContext: this world has no asset manager" );
            return;
        }
        if ( Common::BoolResultStr const added = input->AddContext( *world->Assets, context, priority ); !added )
            Reflection::RaiseCallError( std::format( "Input.addContext('{}'): {}", context, added.GetError() ) );
    }

    bool InputLibrary::RemoveContext( const std::string& context )
    {
        Input::LocalPlayerInput* input = PlayerInput( "removeContext" );
        return input != nullptr && input->RemoveContext( context );
    }

    void InputLibrary::RebindKey( const std::string& context, const std::string& action,
                                  const std::string& defaultKey, const std::string& key )
    {
        Input::LocalPlayerInput* input = PlayerInput( "rebindKey" );
        if ( input == nullptr )
            return;
        if ( Common::BoolResultStr const rebound = input->RebindKey( context, action, defaultKey, key ); !rebound )
            Reflection::RaiseCallError( std::format( "Input.rebindKey: {}", rebound.GetError() ) );
    }

    void InputLibrary::NewFrame( glm::vec2 mouseDelta )
    {
        State().MouseDelta = mouseDelta;
    }

    std::optional<bool> InputLibrary::ConsumeCursorLockRequest()
    {
        return std::exchange( State().CursorLockRequest, std::nullopt );
    }
} // namespace Desert::Libraries
