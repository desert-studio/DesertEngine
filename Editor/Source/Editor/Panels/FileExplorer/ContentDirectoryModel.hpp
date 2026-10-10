#pragma once

#include <Editor/Panels/FileExplorer/DirectoryInformation.hpp>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace Desert::Editor
{
    /// THE CONTENT BROWSER'S TREE (UE: ContentBrowserFileDataSource) — every folder and file under the browser's
    /// root, read from disk lazily: a folder's children are listed the first time it is opened, and re-listed
    /// only by Rescan. It owns every node; the panel and its views hold raw views into it.
    class ContentDirectoryModel
    {
    public:
        /// Lists @p rootPath (an empty path is "Assets") and its immediate children.
        explicit ContentDirectoryModel( const std::string& rootPath );

        /// The root node; null when the root could not be listed.
        [[nodiscard]] DirectoryInformation* Root() const
        {
            return m_Root;
        }

        /// The node of @p path as the tree spells it (a generic path under the root); null when the tree has not
        /// listed it.
        [[nodiscard]] DirectoryInformation* Find( const std::string& path ) const;

        /// Lists @p dir's children unless it is listed already.
        void Open( DirectoryInformation* dir );

        /// Drops @p dir's children and lists them from disk again — picks up files added, removed or renamed
        /// outside the editor. Every view into a child is invalid afterwards; re-resolve it by path (Find).
        void Rescan( DirectoryInformation* dir );

        /// Whether the tree lists @p path at all (the hidden-file rule, ContentBrowserUtils::IsHidden).
        [[nodiscard]] bool Shows( const std::filesystem::path& path ) const;
        [[nodiscard]] bool ShowsHiddenFiles() const
        {
            return m_ShowHiddenFiles;
        }

        /// Every folder / every file under the root, as the tree spells them (generic paths, its hidden-file rule
        /// applied), walked from disk rather than from the listed nodes.
        [[nodiscard]] std::vector<std::string> AllFolders() const;
        [[nodiscard]] std::vector<std::string> AllFiles() const;

    private:
        // Makes (or completes) the node of @p directoryPath; with @p processChildren, lists its children too.
        // Returns the node's key in m_Directories.
        std::string Process( const std::string& directoryPath, DirectoryInformation* parent,
                             bool processChildren );

        std::string           m_RootPath;
        bool                  m_ShowHiddenFiles = false;
        DirectoryInformation* m_Root            = nullptr;

        std::unordered_map<std::string, std::shared_ptr<DirectoryInformation>> m_Directories;
    };

    /// THE BROWSER'S WATCH ON THE OPEN FOLDER (UE: DirectoryWatcher, here without an OS watch API): a cheap
    /// signature of the folder's immediate entries (name + write time), taken at most every kPollFrames frames.
    class DirectoryWatcher
    {
    public:
        static constexpr int kPollFrames = 30; // ~0.5 s at 60 fps — directory_iterator is cheap but not free

        /// Called once a frame. True when @p dirPath's entries differ from the last signature taken; the new
        /// signature becomes the baseline.
        bool Poll( const std::string& dirPath );

        /// The folder just entered is the baseline: without it the first poll after every navigation saw a
        /// "change" and rescanned a folder that had just been read (TH3).
        void Rebase( const std::string& dirPath );

    private:
        int         m_PollCounter = 0;
        std::size_t m_Signature   = 0;
    };
} // namespace Desert::Editor
