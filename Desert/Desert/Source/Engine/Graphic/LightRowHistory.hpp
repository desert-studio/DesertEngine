#pragma once

#include <Engine/Graphic/LocalShadows.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Desert::Graphic
{
    // A VIEW'S LIGHT ROWS ACROSS FRAMES (ViewLightBuffers::PreviousRow): the row of a light in the light index
    // space (points, then spots) changes when another light appears, leaves or crosses a cull; its source does
    // not.
    class LightRowHistory
    {
    public:
        // Per row of this frame, the row the same light had in the previous call; -1 = it had none (a new light,
        // or one with no source id). Never empty (a storage buffer is not): no light is one -1. @p points / @p
        // spots carry the sources; a row past them has none.
        [[nodiscard]] std::vector<int32_t> Advance( const std::span<const LocalLightShadowSettings> points,
                                                    const std::size_t                               pointCount,
                                                    const std::span<const LocalLightShadowSettings> spots,
                                                    const std::size_t                               spotCount )
        {
            std::vector<int32_t>                  previous( pointCount + spotCount, -1 );
            std::unordered_map<uint64_t, int32_t> rows;
            rows.reserve( previous.size() );
            const auto add = [&]( const std::span<const LocalLightShadowSettings> settings,
                                  const std::size_t count, const std::size_t firstRow, const uint64_t kind )
            {
                for ( std::size_t i = 0; i < count && i < settings.size(); ++i )
                {
                    if ( settings[i].SourceId == 0 )
                        continue;
                    const uint64_t key = static_cast<uint64_t>( settings[i].SourceId ) * 2u + kind;
                    if ( const auto found = m_Rows.find( key ); found != m_Rows.end() )
                        previous[firstRow + i] = found->second;
                    rows.insert_or_assign( key, static_cast<int32_t>( firstRow + i ) );
                }
            };
            add( points, pointCount, 0, 0u );
            add( spots, spotCount, pointCount, 1u );
            m_Rows = std::move( rows );
            if ( previous.empty() )
                previous.push_back( -1 );
            return previous;
        }

    private:
        std::unordered_map<uint64_t, int32_t> m_Rows; // (source id, kind) -> its row in the last Advance
    };
} // namespace Desert::Graphic
