#pragma once

#include <Common/Core/Events/Event.hpp>
#include <Common/Core/KeyCodes.hpp>

namespace Common
{
    class KeyEvent : public Event
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

        virtual EventType GetEventType() const
        {
            return GetStaticType();
        }
        static EventType GetStaticType()
        {
            return EventType::KeyPressed;
        }
        inline int GetRepeatCount() const
        {
            return RepeatCount;
        }

        KeyPressedEvent( KeyCode keycode, int repeatCount ) : KeyEvent( keycode ), RepeatCount( repeatCount )
        {
        }
        int RepeatCount;
    };

    // A text-input event: the Unicode codepoint produced by a key press, already resolved for the keyboard
    // layout / modifiers (GLFW char callback). Use this for text fields, NOT KeyPressed (which is raw keys).
    class KeyTypedEvent : public Event
    {
    public:
        DESERT_ROUTED_EVENT( KeyTypedEvent, KeyTyped, Focus )

        virtual EventType GetEventType() const
        {
            return GetStaticType();
        }
        static EventType GetStaticType()
        {
            return EventType::KeyTyped;
        }
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