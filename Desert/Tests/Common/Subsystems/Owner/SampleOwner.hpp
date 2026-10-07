#pragma once

#include <Common/Core/Subsystems/SubsystemCollection.hpp>
#include <Common/Core/Subsystems/WorldSubsystems.hpp>

#include <array>
#include <cstddef>
#include <string_view>

namespace SubsystemSamples
{
    class SampleOwner
    {
    public:
        void Record( std::string_view entry ) noexcept
        {
            if ( m_Count < m_Journal.size() )
                m_Journal[m_Count++] = entry;
        }

        [[nodiscard]] std::string_view At( std::size_t index ) const noexcept
        {
            return index < m_Count ? m_Journal[index] : std::string_view{};
        }

        [[nodiscard]] std::size_t Recorded() const noexcept
        {
            return m_Count;
        }

    private:
        std::array<std::string_view, 16> m_Journal{};
        std::size_t                      m_Count = 0;
    };

    void CreateSubsystems( Common::SubsystemCollection<SampleOwner>& collection );

    class SampleWorld;
} // namespace SubsystemSamples

template <>
inline constexpr bool Common::SubsystemsJoinEventTree<SubsystemSamples::SampleWorld> = false;

namespace SubsystemSamples
{
    class SampleWorld
    {
    public:
        explicit SampleWorld( SampleOwner& journal ) : Journal( &journal )
        {
            Subsystems.Begin();
        }
        ~SampleWorld()
        {
            Subsystems.End();
        }
        SampleWorld( const SampleWorld& )            = delete;
        SampleWorld& operator=( const SampleWorld& ) = delete;
        SampleWorld( SampleWorld&& )                 = delete;
        SampleWorld& operator=( SampleWorld&& )      = delete;

        SampleOwner*                         Journal = nullptr;
        Common::WorldSubsystems<SampleWorld> Subsystems{ *this };
    };
} // namespace SubsystemSamples

namespace SubsystemSamples
{
    void CreateSubsystems( Common::SubsystemCollection<SampleWorld>& collection );
} // namespace SubsystemSamples
