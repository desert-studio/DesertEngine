#pragma once

#include <Common/Core/Events/Event.hpp>
#include <Common/Core/KeyCodes.hpp>

namespace Common
{
    class KeyEvent
    {
    public:
        inline KeyCode GetKeyCode() const
        {
            return Key;
        }

        KeyEvent( KeyCode c ) : Key( c )
        {
        }
        KeyCode Key;
    };

    class KeyPressedEvent : public KeyEvent
    {
    public:
        DESERT_ROUTED_EVENT( KeyPressedEvent, KeyPressed, Focus )

        inline int GetRepeatCount() const
        {
            return RepeatCount;
        }

        KeyPressedEvent( KeyCode keycode, int repeatCount ) : KeyEvent( keycode ), RepeatCount( repeatCount )
        {
        }
        int RepeatCount;
    };

    // The key went up (GLFW_RELEASE). Routed to the same focused node its press was, so whoever keeps a key
    // state (the player, UE UPlayerInput::KeyStateMap) sees both halves of the key from one source.
    class KeyReleasedEvent : public KeyEvent
    {
    public:
        DESERT_ROUTED_EVENT( KeyReleasedEvent, KeyReleased, Focus )

        explicit KeyReleasedEvent( KeyCode keycode ) : KeyEvent( keycode )
        {
        }
    };

    // A text-input event: the Unicode codepoint produced by a key press, already resolved for the keyboard
    // layout / modifiers (GLFW char callback). Use this for text fields, NOT KeyPressed (which is raw keys).
    class KeyTypedEvent
    {
    public:
        DESERT_ROUTED_EVENT( KeyTypedEvent, KeyTyped, Focus )

        inline unsigned int GetCodepoint() const
        {
            return Codepoint;
        }

        explicit KeyTypedEvent( unsigned int codepoint ) : Codepoint( codepoint )
        {
        }
        unsigned int Codepoint;
    };
} // namespace Common