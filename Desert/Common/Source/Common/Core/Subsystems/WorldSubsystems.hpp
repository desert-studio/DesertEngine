#pragma once

#include <Common/Core/Subsystems/SubsystemCollection.hpp>

#include <optional>

namespace Common
{
    template <typename World>
    class WorldSubsystems
    {
    public:
        explicit WorldSubsystems( World& world ) : m_World( &world )
        {
        }

        WorldSubsystems( const WorldSubsystems& )            = delete;
        WorldSubsystems& operator=( const WorldSubsystems& ) = delete;
        WorldSubsystems( WorldSubsystems&& )                 = delete;
        WorldSubsystems& operator=( WorldSubsystems&& )      = delete;

        void Begin()
        {
            m_Collection.reset();
            m_Collection.emplace( *m_World );
        }

        void End()
        {
            m_Collection.reset();
        }

        void SetPlaying( bool playing )
        {
            if ( playing == m_Playing )
                return;
            m_Playing = playing;
            if ( m_Collection )
                Begin();
        }

        [[nodiscard]] bool IsPlaying() const
        {
            return m_Playing;
        }

        [[nodiscard]] bool IsRunning() const
        {
            return m_Collection.has_value();
        }

        template <typename T>
        [[nodiscard]] T* Get() const
        {
            return m_Collection ? m_Collection->template Get<T>() : nullptr;
        }

    private:
        World*                                    m_World   = nullptr;
        bool                                      m_Playing = false;
        std::optional<SubsystemCollection<World>> m_Collection;
    };
} // namespace Common
