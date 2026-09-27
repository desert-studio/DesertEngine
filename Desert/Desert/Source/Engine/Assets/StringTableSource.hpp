#pragma once

#include <Common/Core/Core.hpp>
#include <Common/Core/ResultStr.hpp>

#include <memory>

namespace Desert::Assets
{
    class AssetManager;

    /**
     * @brief Binds the project's string tables to the lookup and REQUESTS the current language's (AL1-7b).
     *
     * Every `.destrings` the content registry lists is sorted by its language - its directory - without
     * opening it, so the lookup can offer every language and a switch knows which files to ask for. Only
     * the requested language's files are read, on AsyncAssetLoader workers; the rest wait for a
     * `SetLanguage`. A table in a directory that is not a language is refused by name (and not read).
     */
    NO_DISCARD Common::BoolResultStr BeginStringTables( const std::weak_ptr<AssetManager>& assets );

    /// Releases every live table request (no delegate fires) and unbinds the lookup's source.
    void EndStringTables();
} // namespace Desert::Assets
