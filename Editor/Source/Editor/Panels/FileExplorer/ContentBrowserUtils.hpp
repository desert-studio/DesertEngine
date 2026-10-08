#pragma once

#include <filesystem>
#include <string>

// THE CONTENT BROWSER'S FREE FUNCTIONS (UE: ContentBrowserUtils) — file operations, the OS shell, the
// hidden-file rule — with no panel state, so the views that need them do not need the panel.
namespace Desert::Editor::ContentBrowserUtils
{
    /// THE ONE ROUTE FOR A RENAME OR A MOVE (AF10c). Content the registry has a row for moves through it: a
    /// redirector stays at the old path, so every scene still naming that path keeps loading, and the move
    /// lands on the undo stack. A folder moves the same way for every row inside it, all or nothing, as one
    /// undo step. Only a loose file the registry does not know (a source image, a note) is a plain file
    /// operation - nothing names it by path through the registry. False with @p error on a refusal.
    bool MoveOrRename( const std::string& src, const std::filesystem::path& dst, const char* label,
                       std::string& error );

    /// A drag onto a folder: the file keeps its name in the destination DIRECTORY. A refusal is logged.
    bool MoveFileTo( const std::string& filePath, const std::string& movePath );

    /// Opens @p path with the OS's default application (no-op on an unsupported platform).
    void ShellOpenDefault( const std::string& path );

    /// Opens Explorer/Finder with @p path selected in its parent (no-op on an unsupported platform).
    void ShellRevealInExplorer( const std::string& path );

    /// Unreadable, or the platform's own junk. A path that cannot be STAT'd is NOT hidden — it is a browser row
    /// we know nothing about, and treating it as visible is the honest answer.
    [[nodiscard]] bool IsHidden( const std::filesystem::path& filePath );

    /// @p s in ASCII lower case (the search box and the name sort compare this).
    [[nodiscard]] std::string ToLowerCopy( std::string s );
} // namespace Desert::Editor::ContentBrowserUtils
