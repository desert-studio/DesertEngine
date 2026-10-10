#include <Engine/Input/InputKey.hpp>

#include <algorithm>
#include <array>
#include <string>
#include <utility>

namespace Desert::Input
{
    namespace
    {
        using K = Common::KeyCode;

        InputKey Keyboard( K code )
        {
            return { InputKey::Device::Keyboard, static_cast<uint16_t>( code ) };
        }

        InputKey Button( Common::MouseButton button )
        {
            return { InputKey::Device::MouseButton, static_cast<uint16_t>( button ) };
        }

        // The keys with a word for a name, in the order the editor's key picker lists them.
        const std::array<std::pair<std::string_view, InputKey>, 21>& NamedKeys()
        {
            static const std::array<std::pair<std::string_view, InputKey>, 21> kNamed = { {
                 { "Space", Keyboard( K::Space ) },
                 { "Enter", Keyboard( K::Enter ) },
                 { "Escape", Keyboard( K::Escape ) },
                 { "Tab", Keyboard( K::Tab ) },
                 { "Backspace", Keyboard( K::Backspace ) },
                 { "Up", Keyboard( K::Up ) },
                 { "Down", Keyboard( K::Down ) },
                 { "Left", Keyboard( K::Left ) },
                 { "Right", Keyboard( K::Right ) },
                 { "LeftShift", Keyboard( K::LeftShift ) },
                 { "RightShift", Keyboard( K::RightShift ) },
                 { "LeftControl", Keyboard( K::LeftControl ) },
                 { "RightControl", Keyboard( K::RightControl ) },
                 { "LeftAlt", Keyboard( K::LeftAlt ) },
                 { "RightAlt", Keyboard( K::RightAlt ) },
                 { "LeftMouseButton", Button( Common::MouseButton::Left ) },
                 { "RightMouseButton", Button( Common::MouseButton::Right ) },
                 { "MiddleMouseButton", Button( Common::MouseButton::Middle ) },
                 { "MouseX", InputKey{ InputKey::Device::MouseX, 0 } },
                 { "MouseY", InputKey{ InputKey::Device::MouseY, 0 } },
                 { "Mouse2D", InputKey{ InputKey::Device::Mouse2D, 0 } },
            } };
            return kNamed;
        }
    } // namespace

    std::optional<InputKey> InputKeyFromName( const std::string_view name )
    {
        // A single letter or digit: KeyCode::A..Z and D0..D9 are their ASCII codes (glfw3.h).
        if ( name.size() == 1 )
        {
            const char c = name[0];
            if ( ( c >= 'A' && c <= 'Z' ) || ( c >= '0' && c <= '9' ) )
                return Keyboard( static_cast<K>( c ) );
            return std::nullopt;
        }
        // F1..F12 are consecutive codes.
        if ( name.size() >= 2 && name.size() <= 3 && name[0] == 'F' )
        {
            int n = 0;
            for ( const char c : name.substr( 1 ) )
            {
                if ( c < '0' || c > '9' )
                    return std::nullopt;
                n = n * 10 + ( c - '0' );
            }
            if ( n < 1 || n > 12 || ( name.size() == 3 && name[1] == '0' ) )
                return std::nullopt;
            return Keyboard( static_cast<K>( static_cast<int>( K::F1 ) + n - 1 ) );
        }

        for ( const auto& [spelled, key] : NamedKeys() )
            if ( spelled == name )
                return key;
        return std::nullopt;
    }

    std::vector<std::string> InputKeyNames()
    {
        std::vector<std::string> names;
        for ( char c = 'A'; c <= 'Z'; ++c )
            names.emplace_back( 1, c );
        for ( char c = '0'; c <= '9'; ++c )
            names.emplace_back( 1, c );
        for ( int n = 1; n <= 12; ++n )
            names.push_back( "F" + std::to_string( n ) );
        for ( const auto& named : NamedKeys() )
            names.emplace_back( named.first );
        return names;
    }

    glm::vec3 RawKeyValue( const InputKey& key, const RawInputFrame& frame )
    {
        switch ( key.Kind )
        {
            case InputKey::Device::Keyboard:
            {
                const auto code = static_cast<Common::KeyCode>( key.Code );
                const bool down =
                     std::find( frame.KeysDown.begin(), frame.KeysDown.end(), code ) != frame.KeysDown.end();
                return { down ? 1.0f : 0.0f, 0.0f, 0.0f };
            }
            case InputKey::Device::MouseButton:
            {
                const auto button = static_cast<Common::MouseButton>( key.Code );
                const bool down   = std::find( frame.MouseButtonsDown.begin(), frame.MouseButtonsDown.end(),
                                               button ) != frame.MouseButtonsDown.end();
                return { down ? 1.0f : 0.0f, 0.0f, 0.0f };
            }
            case InputKey::Device::MouseX:
                return { frame.MouseDelta.x, 0.0f, 0.0f };
            case InputKey::Device::MouseY:
                return { frame.MouseDelta.y, 0.0f, 0.0f };
            case InputKey::Device::Mouse2D:
                return { frame.MouseDelta.x, frame.MouseDelta.y, 0.0f };
        }
        return glm::vec3( 0.0f );
    }
} // namespace Desert::Input
