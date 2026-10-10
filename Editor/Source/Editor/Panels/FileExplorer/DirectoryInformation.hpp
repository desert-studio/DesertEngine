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

        // Lazily-resolved texture thumbnail handle (0 = none / not a registered texture). Cached so the
        // grid doesn't re-resolve every frame; resolution is existing-only (browsing never cooks).
        uint64_t ThumbnailHandle   = 0;
        bool     ThumbnailResolved = false;

        DirectoryInformation( std::string path, bool isFile ) : AssetPath( std::move( path ) ), IsFile( isFile )
        {
        }
    };
} // namespace Desert::Editor
