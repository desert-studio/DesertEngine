#pragma once

#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>

namespace Desert::Editor::ThumbnailFormats
{
    /**
     * @file
     * @brief HOW A PATH NAMES ITS FORMAT — the spelling every thumbnail question starts from.
     *
     * WHO DRAWS A FORMAT IS NOT DECIDED HERE ANY MORE (THM1n-3). This header used to carry its own table
     * "extension -> producer", while ThumbnailProducers.hpp answered "kind -> producer" and
     * FileExplorerPanel.cpp answered "extension -> kind": three tables, two of which answered the same
     * question and could disagree. The chain is now extension -> FileType (FileType.hpp, kFileExtensions)
     * -> Producer (ThumbnailProducers.hpp), asked through ThumbnailProducers::ProducerOfPath; what stays
     * here is the one piece that is this header's own — reading the extension off a path, the same way for
     * the browser tile, the sweep and the census.
     *
     * IT IS FREE OF THE DEVICE AND OF ImGui ON PURPOSE, exactly as ThumbnailKey.hpp and
     * ThumbnailFreshness.hpp next door are: the decision then belongs to a test instead of to a launched
     * editor.
     */

    /// The extension of @p path, lower case, without the dot. Empty for a file that has none.
    ///
    /// Written here rather than at each caller because "which format is this?" has to be asked the same
    /// way by the browser tile, by the background sweep and by the census, and `std::filesystem::path` is
    /// deliberately not in the signature: this header stays free of everything that would stop a test
    /// asking it about a string.
    [[nodiscard]] inline std::string ExtensionOf( std::string_view path )
    {
        const std::size_t dot = path.find_last_of( '.' );
        if ( dot == std::string_view::npos )
            return {};

        // A dot that is part of a directory name is not an extension: "Clouds/v1.2/Layout" has no
        // extension at all, and reading "2/Layout" as one would let a folder decide a file's format.
        const std::size_t slash = path.find_last_of( "/\\" );
        if ( slash != std::string_view::npos && dot < slash )
            return {};

        std::string ext( path.substr( dot + 1 ) );
        for ( char& c : ext )
            c = static_cast<char>( std::tolower( static_cast<unsigned char>( c ) ) );
        return ext;
    }
} // namespace Desert::Editor::ThumbnailFormats
