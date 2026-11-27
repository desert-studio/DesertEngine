#pragma once

#include "CoffObject.hpp"

#include <filesystem>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>

namespace Desert::Editor::LiveCoding::CodeView
{
    /// Which code in @p object carries code of @p sources, read from its .debug$S (/Z7): a function's own file is the
    /// first DEBUG_S_LINES block of its section; an S_INLINESITE at any depth names an inlinee whose file is in
    /// DEBUG_S_INLINEELINES -> DEBUG_S_FILECHKSMS -> DEBUG_S_STRINGTABLE. The function's decorated name is the
    /// symbol the S_*PROC32 record's SECREL relocation points at.
    struct Carriers
    {
        std::vector<std::filesystem::path> Files;     // own files (not in @p sources) of the carrying functions
        std::unordered_set<std::string>    Functions; // decorated names, own file in @p sources included
    };

    [[nodiscard]] Common::ResultStr<Carriers> FindCarriers( const Coff::Object&                  object,
                                                           std::span<const std::filesystem::path> sources );
} // namespace Desert::Editor::LiveCoding::CodeView
