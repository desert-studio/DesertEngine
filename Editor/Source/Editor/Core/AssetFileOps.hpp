#pragma once

#include <functional>
#include <string>

namespace Desert::Editor::AssetFileOps
{
    // Cross-platform file operations for the Assets browser, on std::filesystem (no shell — the old
    // drag-move shelled out to Windows `move`, so it did nothing on macOS). Each returns false and fills
    // `error` on failure; the successful new path is written to `outNewPath` where relevant.

    // Move `src` into directory `destDir` (rename within the tree, with a copy+remove fallback across
    // devices). Fails if a same-named entry already exists in the destination.
    bool Move( const std::string& src, const std::string& destDir, std::string& outNewPath, std::string& error );

    // Rename `src` to `newFileName` (full name incl. extension) in its own folder.
    bool Rename( const std::string& src, const std::string& newFileName, std::string& outNewPath,
                 std::string& error );

    // A SCENE IS COPIED AND DELETED WITH ITS COMPANION FOLDERS (WP-ASSET-OPS, UE's duplicate / delete of a World
    // Partition map with its external actor packages): Common::Content::SceneCompanionDirectoriesOf - the entity
    // folder `__ExternalEntities__/<stem>/` with its descriptor index, and the built HLODs `<stem>_HLOD/` - under
    // the copy's name. A copy is all or nothing, and its name is unique for the file AND its folders.

    // Copy `src` next to itself under a unique "<stem> N" name.
    bool Duplicate( const std::string& src, std::string& outNewPath, std::string& error );

    // Copy `src` INTO directory `destDir`, keeping its name (or a unique "<stem> N" if taken). Used by
    // copy+paste across folders.
    bool CopyInto( const std::string& src, const std::string& destDir, std::string& outNewPath,
                   std::string& error );

    // Delete a file or a directory (recursively) - permanently: the editor's Delete goes to the project's trash
    // (Common::Content::MoveToTrash, which carries the same companion folders); this empties it.
    bool Delete( const std::string& path, std::string& error );

    // Pure: a "<stem><ext>" name that does not collide, trying "<stem><ext>", "<stem> 2<ext>", ... The
    // `exists` predicate reports whether a candidate file name is taken. No disk access — unit-tested.
    std::string UniqueName( const std::string& stem, const std::string& ext,
                            const std::function<bool( const std::string& )>& exists );
} // namespace Desert::Editor::AssetFileOps
