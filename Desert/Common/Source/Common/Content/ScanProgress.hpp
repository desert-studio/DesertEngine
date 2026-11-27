#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>

namespace Common::Content
{
    // What a scan says while it runs (UE FScopedSlowTask: the walking code reports, nobody guesses from outside):
    // the number of files it will describe, once, after the walk and before the first header is read; then each
    // file around the read of its header, which is the slow half on a cold disk. Every member may be empty.
    // Apart from ContentScan.hpp so that a reader of the scan's results does not compile <functional>.
    struct ScanProgress
    {
        std::function<void( std::size_t files )>                 Listed;
        std::function<void( const std::filesystem::path& file )> Begin;
        std::function<void()>                                    End;
    };
} // namespace Common::Content
