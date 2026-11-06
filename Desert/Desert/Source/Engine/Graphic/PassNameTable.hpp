#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>

namespace Desert::Graphic
{
    // NAMES MADE ONCE (UE: FRDGBuilder event names are static, or formatted only while events are on). Row i's
    // name is formatted the first frame index i is asked for, and the same string is handed back every frame after
    // while the row's key stays (a local light's raster level given to another light changes the key and
    // reformats that row only). No per-frame formatting, no per-frame heap.
    // The rows are a deque: growing it never moves a row, so a view handed out earlier stays valid until that same
    // row is asked again with another key.
    class PassNameTable
    {
    public:
        // @p format( std::string& out ) appends the name into the (cleared) row; called only on a new row or key.
        template <typename Format>
        [[nodiscard]] std::string_view Get( const std::size_t index, const uint64_t key, Format&& format )
        {
            if ( index >= m_Rows.size() )
                m_Rows.resize( index + 1 );
            Row& row = m_Rows[index];
            if ( !row.Made || row.Key != key )
            {
                row.Name.clear();
                format( row.Name );
                row.Key  = key;
                row.Made = true;
            }
            return row.Name;
        }

    private:
        struct Row
        {
            uint64_t    Key  = 0;
            bool        Made = false;
            std::string Name;
        };
        std::deque<Row> m_Rows;
    };
} // namespace Desert::Graphic
