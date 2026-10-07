#pragma once

#include <Common/Core/Events/Event.hpp>
#include <Common/Core/MouseButton.hpp>

#include <sstream>

namespace Common
{

    class MouseMovedEvent
    {
    public:
        DESERT_ROUTED_EVENT( MouseMovedEvent, MouseMoved, Pointer )

        MouseMovedEvent( float x, float y ) : m_MouseX( x ), m_MouseY( y )
        {
        }

        inline float GetX() const
        {
            return m_MouseX;
        }
        inline float GetY() const
        {
            return m_MouseY;
        }

    private:
        float m_MouseX, m_MouseY;
    };

    class MouseScrolledEvent
    {
    public:
        DESERT_ROUTED_EVENT( MouseScrolledEvent, MouseScrolled, Pointer )

        MouseScrolledEvent( float xOffset, float yOffset ) : m_XOffset( xOffset ), m_YOffset( yOffset )
        {
        }

        inline float GetXOffset() const
        {
            return m_XOffset;
        }
        inline float GetYOffset() const
        {
            return m_YOffset;
        }

    private:
        float m_XOffset, m_YOffset;
    };

    class MouseButtonEvent
    {
    public:
        inline MouseButton GetMouseButton() const
        {
            return m_Button;
        }

    protected:
        explicit MouseButtonEvent( MouseButton button ) : m_Button( button )
        {
        }

        MouseButton m_Button;
    };

    class MouseButtonPressedEvent : public MouseButtonEvent
    {
    public:
        DESERT_ROUTED_EVENT( MouseButtonPressedEvent, MouseButtonPressed, Pointer )

        explicit MouseButtonPressedEvent( MouseButton button ) : MouseButtonEvent( button )
        {
        }
    };

    // class MouseButtonReleasedEvent : public MouseButtonEvent
    //{
    // public:
    //	MouseButtonReleasedEvent(int button)
    //		: MouseButtonEvent(button) {}

    //};

} // namespace Common