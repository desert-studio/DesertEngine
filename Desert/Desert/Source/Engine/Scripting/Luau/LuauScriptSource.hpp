#pragma once

#include <Common/Core/ResultStr.hpp>

#include <string>

namespace Desert::Scripting
{
    /// The script's text: a loose file on disk, else the packed archive's copy (shipped game). One reader for the
    /// running script (ScriptEngine) and its declarations (ReadScriptSchema, LuauScriptSchema.cpp).
    [[nodiscard]] Common::ResultStr<std::string> ReadScriptSource( const std::string& path );
} // namespace Desert::Scripting
