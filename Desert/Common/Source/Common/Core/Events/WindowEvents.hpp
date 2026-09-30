#pragma once

#include <Common/Core/Events/Event.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace Common
{
    // OS file drop (drag files from Explorer/desktop onto the window). Paths are absolute.
    class EventWindowFileDrop : public Event
    {
    public:
        DESERT_ROUTED_EVENT( EventWindowFileDrop, WindowFileDropped, Pointer )

        virtual EventType GetEventType() const
        {
            return GetStaticType();
        }
        static EventType GetStaticType()
        {
            return EventType::WindowFileDrop;
        }
        explicit EventWindowFileDrop( std::vector<std::string> paths ) : Paths( std::move( paths ) )
        {
        }

        std::vector<std::string> Paths;
    };

    class EventWindowClose : public Event
    {
    public:
        DESERT_ROUTED_EVENT( EventWindowClose, WindowClosed, Broadcast )

        virtual EventType GetEventType() const
        {
            return GetStaticType();
        }
        static EventType GetStaticType()
        {
            return EventType::WindowClose;
        }
    };

    class EventWindowResize : public Event
    {
    public:
        DESERT_ROUTED_EVENT( EventWindowResize, WindowResized, Broadcast )

        virtual EventType GetEventType() const
        {
            return GetStaticType();
        }
        static EventType GetStaticType()
        {
            return EventType::WindowResize;
        }
        EventWindowResize( uint32_t width, uint32_t height ) : width( width ), height( height )
        {
        }
        EventWindowResize() = delete;

        uint32_t width;
        uint32_t height;
    };
} // namespace Common