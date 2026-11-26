#pragma once

#include <Engine/Graphic/LocalShadows.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace Desert::Graphic
{
    // A VIEW'S LIGHT ROWS ACROSS FRAMES (System::MegaLights' PreviousLightRowsUB): the row of a light in the light
    // index space (points, then spots) changes when another light appears, leaves or crosses a cull; its source
    // does not.
    class LightRowHistory
    {
    public:
        // Per row of this frame, the row the same light had in the previous call; -1 = it had none (a new light,
        // or one with no source id). Never empty (a storage buffer is not): no light is one -1. Valid until the
        // next call. @p points / @p spots carry the sources; a row past them has none.
        [[nodiscard]] std::span<const int32_t> Advance( const std::span<const LocalLightShadowSettings> points,
                                                        const std::size_t                               pointCount,
                                                        const std::span<const LocalLightShadowSettings> spots,
                                                        const std::size_t                               spotCount )
        {
            m_Previous.assign( std::max<std::size_t>( pointCount + spotCount, 1u ), -1 );
            m_NextRows.clear();
            const auto add = [this]( const std::span<const LocalLightShadowSettings> settings,
                                     const std::size_t count, const std::size_t firstRow, const uint64_t kind )
            {
                for ( std::size_t i = 0; i < count && i < settings.size(); ++i )
                {
                    if ( settings[i].SourceId == 0 )
                        continue;
                    const uint64_t key   = static_cast<uint64_t>( settings[i].SourceId ) * 2u + kind;
                    const auto     found = std::lower_bound( m_Rows.begin(), m_Rows.end(), Row{ key, -1 } );
                    if ( found != m_Rows.end() && found->first == key )
                        m_Previous[firstRow + i] = found->second;
                    m_NextRows.emplace_back( key, static_cast<int32_t>( firstRow + i ) );
                }
            };
            add( points, pointCount, 0, 0u );
            add( spots, spotCount, pointCount, 1u );
            std::sort( m_NextRows.begin(), m_NextRows.end() );
            std::swap( m_Rows, m_NextRows );
            return m_Previous;
        }

    private:
        using Row = std::pair<uint64_t, int32_t>; // (source id, kind) -> its row
        std::vector<Row>     m_Rows;              // of the last Advance, sorted by key
        std::vector<Row>     m_NextRows;          // the same storage, filled by the next one
        std::vector<int32_t> m_Previous;          // what Advance returned
    };
} // namespace Desert::Graphic
