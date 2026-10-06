#pragma once

#include <Common/Core/ResultStr.hpp>

#include <filesystem>
#include "ImportResult.hpp"

namespace Desert::Editor
{
    class ImportManager;

    // WHAT A SOURCE FILE HOLDS, known before it is imported (UE: the FBX factory reads the scene first and titles
    // its Import Options window Static Mesh, Skeletal Mesh or Animation by it).
    enum class ImportContentKind
    {
        StaticMesh,   // meshes, none of them skinned
        SkeletalMesh, // a skinned mesh (its skeleton and clips come with it)
        Animation     // no mesh: a skeleton and/or clips only
    };

    // WHO IS ASKING FOR THE PARSE (UE: a factory import vs. a DDC build). `Import` is the user's (re)import: it
    // creates the content the source yields - the texture assets of its embedded images, beside the source - and
    // those files are content, committed with the source; a drop and Rebuild Cooked Assets are imports too. `Cook`
    // is the editor deriving what it loads (the boot and background cook): it reads that content and writes only
    // derived data, never a file into Content - every write of the import is refused under it with the re-import
    // that makes the file (ImportManager.cpp ContentWriteAllowed), so a checkout that was merely opened stays
    // clean (SELF-COOK).
    enum class ImportPass
    {
        Import,
        Cook
    };

    class IAssetImporter
    {
    public:
        virtual ~IAssetImporter() = default;

        virtual ImportResult Import( const std::filesystem::path& path, ImportManager& manager,
                                     ImportPass pass ) = 0;
        // What @p path holds, read without building anything; an error naming the file when it does not parse.
        virtual Common::ResultStr<ImportContentKind> Probe( const std::filesystem::path& path ) = 0;
    };
} // namespace Desert::Editor