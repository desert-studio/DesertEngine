#pragma once

#include <functional>
#include <unordered_map>
#include <vector>

namespace Desert::Assets
{
    class AssetsEventSystem
    {
    public:
        enum class EventType
        {
            OnLoaded, // Data loaded into RAM
            OnReady,  // Data is ready for rendering (GPU)
        };

        virtual ~AssetsEventSystem() = default;

        void Subscribe( EventType type, std::function<void()> callback )
        {
            m_EventCallbacks[type].push_back( callback );
        }

        void Notify( EventType type )
        {
            auto it = m_EventCallbacks.find( type );
            if ( it != m_EventCallbacks.end() )
            {
                for ( auto& callback : it->second )
                {
                    callback();
                }
            }
        }

    private:
        std::unordered_map<EventType, std::vector<std::function<void()>>> m_EventCallbacks;
    };
} // namespace Desert::Assets