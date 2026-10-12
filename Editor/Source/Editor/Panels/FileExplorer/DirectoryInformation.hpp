#pragma once

#include <Editor/Panels/FileExplorer/FileType.hpp>

#include <ImGui/imgui.h>

#include <cstdint>
#include <utility>
#include <string>
#include <vector>

namespace Desert::Editor
{
    /// ONE NODE OF THE CONTENT BROWSER'S TREE — a folder or a file under the browser's root. Every node lives in
    /// ContentDirectoryModel's map (by shared_ptr, keyed by AssetPath); Parent and Children are views into that
    /// same map, valid for as long as the model keeps the node.
    struct DirectoryInformation
    {
        DirectoryInformation*              Parent = nullptr;
        std::vector<DirectoryInformation*> Children;

        std::string AssetPath;
        FileType    Type       = FileType::Unknown;
        uint64_t    FileSize   = 0;
        uint64_t LastWriteTime = 0; // filesystem mtime (for "sort by date"); cached so sorting needs no syscalls
        ImVec4   FileTypeColour;

        bool Hidden = false;
        bool IsFile = true;
        bool Opened = false;
        bool Leaf   = true;

        DirectoryInformation( std::string path, bool isFile ) : AssetPath( std::move( path ) ), IsFile( isFile )
        {
        }
    };
} // namespace Desert::Editor
