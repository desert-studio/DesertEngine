#include <Engine/Graphic/GpuDriven/CullPhaseCounts.hpp>

#include <Common/Core/Logger.hpp>
#include <Engine/Core/EngineContext.hpp>
#include <Engine/Graphic/GpuDriven/GpuSceneLayout.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>

#include <algorithm>
#include <cstring>
#include <format>

namespace Desert::Graphic::GpuDriven
{
    void CullPhaseCounts::AddFrame( RDG::Builder& graph, const uint64_t frameIndex,
                                    const std::span<const std::string> viewNames, const OcclusionCarry& carry )
    {
        const uint32_t slots = std::max( 1u, EngineContext::GetInstance().GetMaxFramesInFlight() );
        if ( m_Slots.size() != slots )
        {
            m_Slots.clear();
            m_Slots.resize( slots );
        }
        Slot& slot = m_Slots[static_cast<std::size_t>( frameIndex % slots )];
        Count( slot );
        slot = Slot{};
        if ( carry.ViewCount != viewNames.size() || !carry.Visibility.IsValid() )
        {
            std::string message = std::format(
                 "{} view names, the carry holds {} views (Visibility {}); no counts", viewNames.size(),
                 carry.ViewCount, carry.Visibility.IsValid() ? "valid" : "invalid" );
            if ( message != m_LastError )
                LOG_ERROR( "[GPUDriven] DrawBins phase counts: {}", message );
            m_LastError = std::move( message );
            return;
        }
        graph.Extract( carry.Visibility, slot.Visibility, RDG::Access::HostRead );
        slot.Names.assign( viewNames.begin(), viewNames.end() );
        slot.InstanceCount = carry.InstanceCount;
    }

    void CullPhaseCounts::Forget()
    {
        m_Slots.clear();
        m_Last.clear();
    }

    void CullPhaseCounts::Count( const Slot& slot )
    {
        if ( !slot.Visibility.Physical )
            return; // the slot has not been handed a frame yet
        const std::span<const std::byte> bytes  = slot.Visibility.Physical->ReadForHost();
        const uint64_t                   needed = static_cast<uint64_t>( slot.Names.size() ) * slot.InstanceCount;
        if ( bytes.size() < needed * sizeof( uint32_t ) )
        {
            std::string message =
                 std::format( "the Visibility readback holds {} bytes, {} views x {} instances need {}",
                              bytes.size(), slot.Names.size(), slot.InstanceCount, needed * sizeof( uint32_t ) );
            if ( message != m_LastError )
                LOG_ERROR( "[GPUDriven] DrawBins phase counts: {}", message );
            m_LastError = std::move( message );
            return;
        }
        std::vector<CullPhaseViewCount> counts( slot.Names.size() );
        for ( std::size_t v = 0; v < slot.Names.size(); ++v )
        {
            CullPhaseViewCount& count = counts[v];
            count.Name                = slot.Names[v];
            for ( uint32_t row = 0; row < slot.InstanceCount; ++row )
            {
                uint32_t word = 0;
                std::memcpy( &word, bytes.data() + ( v * slot.InstanceCount + row ) * sizeof( uint32_t ),
                             sizeof( uint32_t ) );
                if ( ( word & CullVisibility::kPostVisible ) != 0u )
                    ++count.Phase2;
                else if ( ( word & CullVisibility::kInView ) != 0u && ( word & CullVisibility::kOccluded ) == 0u )
                    ++count.Phase1;
            }
        }
        m_Last = std::move( counts );
        m_LastError.clear();
    }
} // namespace Desert::Graphic::GpuDriven
