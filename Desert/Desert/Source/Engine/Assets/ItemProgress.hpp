#pragma once

// A LONG CALL THAT WORKS THROUGH A LIST SAYS WHICH ITEM IT IS ON. The editor's splash names the texture
// being cooked and the shader being compiled ("T_Rock_Albedo (37 / 212)") from inside calls that take
// seconds and would otherwise be one silent block; the engine calls this and knows nothing of a splash.
//
// @p item is what is being worked on now (a file name), @p done how many items of this call are already
// finished (0 for the first), @p total how many the call will work through. An empty function is a call
// nobody watches, and costs one branch per item.

#include <cstddef>
#include <functional>
#include <string>

namespace Desert::Assets
{
    using ItemProgress = std::function<void( const std::string& item, std::size_t done, std::size_t total )>;

    // Asked between items of a long call; true stops it after the item in hand (the editor's splash close
    // button, pressed while the shader preload runs).
    using StopRequested = std::function<bool()>;

    inline void ReportItem( const ItemProgress& progress, const std::string& item, const std::size_t done,
                            const std::size_t total )
    {
        if ( progress )
            progress( item, done, total );
    }
} // namespace Desert::Assets
