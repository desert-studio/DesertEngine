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
                                    const std::span<const CullPhaseSource> sources )
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
        for ( const CullPhaseSource& source : sources )
        {
            if ( source.Carry.ViewCount != source.ViewNames.size() || !source.Carry.Visibility.IsValid() )
            {
                std::string message = std::format(
                     "{} view names, the carry holds {} views (Visibility {}); no counts", source.ViewNames.size(),
                     source.Carry.ViewCount, source.Carry.Visibility.IsValid() ? "valid" : "invalid" );
                if ( message != m_LastError )
                    LOG_ERROR( "[GPUDriven] DrawBins phase counts: {}", message );
                m_LastError = std::move( message );
                return;
            }
        }
        slot.Parts.resize( sources.size() );
        for ( std::size_t s = 0; s < sources.size(); ++s )
        {
            SlotPart& part = slot.Parts[s];
            graph.Extract( sources[s].Carry.Visibility, part.Visibility, RDG::Access::HostRead );
            part.Names.assign( sources[s].ViewNames.begin(), sources[s].ViewNames.end() );
            part.InstanceCount = sources[s].Carry.InstanceCount;
        }
    }

    void CullPhaseCounts::Forget()
    {
        m_Slots.clear();
        m_Last.clear();
    }

    void CullPhaseCounts::Count( const Slot& slot )
    {
        if ( slot.Parts.empty() ||
             std::ranges::any_of( slot.Parts, []( const SlotPart& part ) { return !part.Visibility.Physical; } ) )
            return; // the slot has not been handed a frame yet
        std::vector<CullPhaseViewCount> counts;
        for ( const SlotPart& part : slot.Parts )
        {
            const std::span<const std::byte> bytes = part.Visibility.Physical->ReadForHost();
            const uint64_t needed = static_cast<uint64_t>( part.Names.size() ) * part.InstanceCount;
            if ( bytes.size() < needed * sizeof( uint32_t ) )
            {
                std::string message = std::format(
                     "the Visibility readback holds {} bytes, {} views x {} instances need {}", bytes.size(),
                     part.Names.size(), part.InstanceCount, needed * sizeof( uint32_t ) );
                if ( message != m_LastError )
                    LOG_ERROR( "[GPUDriven] DrawBins phase counts: {}", message );
                m_LastError = std::move( message );
                return;
            }
            for ( std::size_t v = 0; v < part.Names.size(); ++v )
            {
                CullPhaseViewCount& count = counts.emplace_back();
                count.Name                = part.Names[v];
                for ( uint32_t row = 0; row < part.InstanceCount; ++row )
                {
                    uint32_t word = 0;
                    std::memcpy( &word, bytes.data() + ( v * part.InstanceCount + row ) * sizeof( uint32_t ),
                                 sizeof( uint32_t ) );
                    if ( ( word & CullVisibility::kPostVisible ) != 0u )
                        ++count.Phase2;
                    else if ( ( word & CullVisibility::kInView ) != 0u &&
                              ( word & CullVisibility::kOccluded ) == 0u )
                        ++count.Phase1;
                    else if ( ( word & CullVisibility::kInView ) != 0u )
                        ++count.Occluded;
                }
            }
        }
        m_Last = std::move( counts );
        m_LastError.clear();
    }
} // namespace Desert::Graphic::GpuDriven
