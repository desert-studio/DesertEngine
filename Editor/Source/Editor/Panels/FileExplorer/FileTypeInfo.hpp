#pragma once

#include <Editor/Panels/FileExplorer/FileType.hpp>

#include <ImGui/imgui.h>

namespace Desert::Editor
{
    /// HOW THE CONTENT BROWSER PRESENTS ONE FileType (UE: the asset type actions' name, colour and class icon).
    /// One row per kind, in one table (FileTypeInfo.cpp), rather than three maps that each had to remember every
    /// kind — the shape in which Material came to have a name but no colour and no icon.
    struct FileTypeInfo
    {
        FileType    Type{};
        const char* Name = nullptr; ///< the tooltip's type line
        ImVec4      Colour;         ///< the tile's class stripe
        const char* Icon = nullptr; ///< the type glyph drawn when the kind has no picture
    };

    /// The row of @p type. Every enumerator up to kLastFileType has one.
    [[nodiscard]] const FileTypeInfo& FileTypeInfoOf( FileType type );
} // namespace Desert::Editor
