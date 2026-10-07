#pragma once

#include <Common/Core/Events/Event.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace Common
{
    // OS file drop (drag files from Explorer/desktop onto the window). Paths are absolute.
    class EventWindowFileDrop
    {
    public:
        DESERT_ROUTED_EVENT( EventWindowFileDrop, WindowFileDropped, Pointer )

        explicit EventWindowFileDrop( std::vector<std::string> paths ) : Paths( std::move( paths ) )
        {
        }

        std::vector<std::string> Paths;
    };

    class EventWindowClose
    {
    public:
        DESERT_ROUTED_EVENT( EventWindowClose, WindowClosed, Broadcast )
    };

    class EventWindowResize
    {
    public:
        DESERT_ROUTED_EVENT( EventWindowResize, WindowResized, Broadcast )

        EventWindowResize( uint32_t width, uint32_t height ) : width( width ), height( height )
        {
        }
        EventWindowResize() = delete;

        uint32_t width;
        uint32_t height;
    };
} // namespace Common