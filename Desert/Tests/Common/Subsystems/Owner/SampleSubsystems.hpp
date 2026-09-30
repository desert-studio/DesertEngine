#pragma once

#include <SampleOwner.hpp>

#include <Common/Core/Events/KeyEvents.hpp>
#include <Common/Core/Events/WindowEvents.hpp>
#include <Common/Core/Subsystems/SubsystemCollection.hpp>

namespace SubsystemSamples
{
    class InputJournalSubsystem
    {
        DESERT_SUBSYSTEM( Sample )

    public:
        explicit InputJournalSubsystem( SampleOwner& owner ) : m_Owner( &owner )
        {
            m_Owner->Record( "input journal created" );
        }
        ~InputJournalSubsystem()
        {
            m_Owner->Record( "input journal destroyed" );
        }
        InputJournalSubsystem( const InputJournalSubsystem& )            = delete;
        InputJournalSubsystem& operator=( const InputJournalSubsystem& ) = delete;
        InputJournalSubsystem( InputJournalSubsystem&& )                 = delete;
        InputJournalSubsystem& operator=( InputJournalSubsystem&& )      = delete;

        bool OnKeyPressed( Common::KeyPressedEvent& )
        {
            m_Owner->Record( "key pressed" );
            return true;
        }

        bool OnWindowResized( Common::EventWindowResize& )
        {
            m_Owner->Record( "window resized" );
            return false;
        }

        [[nodiscard]] SampleOwner& Owner() const
        {
            return *m_Owner;
        }

    private:
        SampleOwner* m_Owner = nullptr;
    };

    class ClockSubsystem
    {
        DESERT_SUBSYSTEM( Sample )

    public:
        explicit ClockSubsystem( SampleOwner& owner ) : m_Owner( &owner )
        {
            m_Owner->Record( "clock created" );
        }
        ~ClockSubsystem()
        {
            m_Owner->Record( "clock destroyed" );
        }
        ClockSubsystem( const ClockSubsystem& )            = delete;
        ClockSubsystem& operator=( const ClockSubsystem& ) = delete;
        ClockSubsystem( ClockSubsystem&& )                 = delete;
        ClockSubsystem& operator=( ClockSubsystem&& )      = delete;

    private:
        SampleOwner* m_Owner = nullptr;
    };

    class ForeignOwnerSubsystem
    {
        DESERT_SUBSYSTEM( Other )
    };

    class WeatherSubsystem
    {
        DESERT_SUBSYSTEM( SampleWorld )

    public:
        explicit WeatherSubsystem( SampleWorld& world ) : m_World( &world )
        {
            m_World->Journal->Record( "weather created" );
        }
        ~WeatherSubsystem()
        {
            m_World->Journal->Record( "weather destroyed" );
        }
        WeatherSubsystem( const WeatherSubsystem& )            = delete;
        WeatherSubsystem& operator=( const WeatherSubsystem& ) = delete;
        WeatherSubsystem( WeatherSubsystem&& )                 = delete;
        WeatherSubsystem& operator=( WeatherSubsystem&& )      = delete;

    private:
        SampleWorld* m_World = nullptr;
    };
} // namespace SubsystemSamples
