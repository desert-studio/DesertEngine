#pragma once

#include <Editor/Import/CookedJsonWrite.hpp>

#include <Common/Core/Constants.hpp>
#include <Common/Core/ResultStr.hpp>
#include <Common/Core/UUID.hpp>
#include <Engine/Assets/Serialization/MeshBinary.hpp>
#include <Engine/Geometry/DynamicMeshAsset.hpp>

#include <cctype>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace Desert::Editor
{
    // A MODELING RESULT WRITTEN AS A STATIC MESH ASSET (UE: "Output: New Static Mesh", the modeling mode's
    // default; UE::Modeling::CreateMeshObject with ECreateObjectTypeHint::StaticMesh).
    //
    // WHERE THE FILE GOES. Under the cooked mesh root (`MESH_PATH_COOKED`), in a sub-folder the tool names.
    // Not beside the scene and not in Assets/Meshes: `.stmesh` is registered, rebuilt and preloaded ONLY from
    // that root (ContentKinds.hpp: StaticMesh -> MESH_PATH_COOKED), so a file anywhere else is a mesh the
    // next "Rebuild Content" forgets and `AssetRegistryTool check` calls a stray row. The hand-authored probes
    // (SkinProbe, StaticProbe) live under the same root for the same reason.
    //
    // A NAME IS NEVER OVERWRITTEN (UE: CreateUniqueAssetName). A taken name gets `_1`, `_2`, ... - an entity
    // elsewhere in the project may already draw the file that sits under the plain name.

    // Letters, digits, '-' and '_' kept; everything else becomes '_', so the name written is the name asked
    // for (WriteCookedBytes would otherwise rename illegal characters behind the caller's back). Empty -> "Mesh".
    [[nodiscard]] inline std::string StaticMeshAssetName( std::string_view base )
    {
        std::string name;
        name.reserve( base.size() );
        for ( const char c : base )
        {
            const auto u = static_cast<unsigned char>( c );
            name.push_back( std::isalnum( u ) != 0 || c == '-' || c == '_' ? c : '_' );
        }
        return name.empty() ? std::string( "Mesh" ) : name;
    }

    // `folder/<name>.stmesh`, or the first free `folder/<name>_N.stmesh`.
    [[nodiscard]] inline std::filesystem::path UniqueStaticMeshPath( const std::filesystem::path& folder,
                                                                     std::string_view             baseName )
    {
        const std::string     name = StaticMeshAssetName( baseName );
        std::filesystem::path path = folder / ( name + ".stmesh" );
        for ( int n = 1; std::filesystem::exists( path ); ++n )
            path = folder / ( name + "_" + std::to_string( n ) + ".stmesh" );
        return path;
    }

    // The folder a tool's "Asset Folder" setting names: relative to the cooked mesh root, and refused when it
    // would climb out of it (a rooted path or a `..` component) - a file outside the root is not content. Rooted,
    // not absolute: on Windows "/abs" and "C:abs" are not is_absolute() yet still leave the root when appended.
    [[nodiscard]] inline Common::ResultStr<std::filesystem::path>
    StaticMeshOutputFolder( std::string_view relative )
    {
        const std::filesystem::path rel = std::filesystem::path( relative ).lexically_normal();
        if ( rel.is_absolute() || rel.has_root_directory() || rel.has_root_name() ||
             ( !rel.empty() && *rel.begin() == ".." ) )
            return Common::MakeFormattedError<std::filesystem::path>(
                 "the asset folder '{}' is outside the cooked mesh folder; name a folder inside it", relative );
        return Common::MakeSuccess( ( Common::Constants::Path::MESH_PATH_COOKED / rel ).lexically_normal() );
    }

    // Encodes @p mesh (Geometry::DynamicMeshToMeshAssetData - polygroups included) and writes it as a NEW file in
    // @p folder, registered in the content registry with its bounds, exactly as the importer's cook does.
    // Returns the path written. Nothing is written when the mesh is refused.
    [[nodiscard]] inline Common::ResultStr<std::filesystem::path>
    WriteStaticMeshAsset( const Geometry::FDynamicMesh3& mesh, std::span<const Common::UUID> slotMaterials,
                          const std::filesystem::path& folder, std::string_view baseName )
    {
        auto data = Geometry::DynamicMeshToMeshAssetData( mesh, slotMaterials );
        if ( !data.IsSuccess() )
            return Common::MakeFormattedError<std::filesystem::path>( "'{}' was not written as a static mesh: {}",
                                                                      baseName, data.GetError() );
        const Assets::Serialization::MeshAssetData asset = data.ExtractValue();

        const std::filesystem::path path = UniqueStaticMeshPath( folder, baseName );
        if ( auto written = WriteCookedBytes( Assets::Serialization::EncodeMeshBinary( asset ), path,
                                              Assets::Serialization::MeshDataBounds( asset ) );
             !written.IsSuccess() )
            return Common::MakeFormattedError<std::filesystem::path>( "'{}' could not be written: {}",
                                                                      path.generic_string(), written.GetError() );
        return Common::MakeSuccess( path );
    }
} // namespace Desert::Editor
