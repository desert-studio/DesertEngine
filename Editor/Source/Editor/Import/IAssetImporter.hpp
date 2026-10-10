#pragma once

#include <Common/Core/ResultStr.hpp>

#include <Engine/Assets/MeshSourceAsset.hpp>

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

    class IAssetImporter
    {
    public:
        virtual ~IAssetImporter() = default;

        // @p settings are the source's import options (its import record); an importer reads the ones that
        // shape what it parses (the file unit) - the rest are applied to its result by ImportManager.
        virtual ImportResult Import( const std::filesystem::path& path, ImportManager& manager,
                                     const Assets::SourceImportSettings& settings ) = 0;
        // What @p path holds, read without building anything; an error naming the file when it does not parse.
        virtual Common::ResultStr<ImportContentKind> Probe( const std::filesystem::path& path ) = 0;
    };
} // namespace Desert::Editor