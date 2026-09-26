#pragma once

#include <Common/Core/Constants.hpp>
#include <Common/Core/ResultStr.hpp>
#include <Common/Core/UUID.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/MeshSourceAsset.hpp>
#include <Engine/Geometry/DynamicMeshAsset.hpp>
#include <Engine/Geometry/DynamicMeshSerialization.hpp>

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
    // WHAT THE FILE IS. The same source asset an import writes (MeshSourceAsset.hpp: the editable mesh, the slot
    // table, the build settings), with no source file behind it - provenance `Recovered`, exactly as UE's
    // modeling output is a UStaticMesh whose SourceModels carry a MeshDescription and no AssetImportData. The
    // render form is derived from it by the loader through the DDC, like every other static mesh.
    //
    // WHERE THE FILE GOES. Under the mesh root (`MESH_PATH`, Assets/Meshes), in a sub-folder the tool names:
    // static meshes are registered from the Assets tree (ContentKinds.hpp: StaticMesh -> ASSETS_PATH), and the
    // cooked mesh root now holds skinned outputs only.
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

    // The folder a tool's "Asset Folder" setting names: relative to the mesh root, and refused when it
    // would climb out of it (a rooted path or a `..` component) - a file outside the root is not content. Rooted,
    // not absolute: on Windows "/abs" and "C:abs" are not is_absolute() yet still leave the root when appended.
    [[nodiscard]] inline Common::ResultStr<std::filesystem::path>
    StaticMeshOutputFolder( std::string_view relative )
    {
        const std::filesystem::path rel = std::filesystem::path( relative ).lexically_normal();
        if ( rel.is_absolute() || rel.has_root_directory() || rel.has_root_name() ||
             ( !rel.empty() && *rel.begin() == ".." ) )
            return Common::MakeFormattedError<std::filesystem::path>(
                 "the asset folder '{}' is outside the mesh folder; name a folder inside it", relative );
        return Common::MakeSuccess( ( Common::Constants::Path::MESH_PATH / rel ).lexically_normal() );
    }

    // Writes @p mesh as a NEW static mesh source asset in @p folder, slot k naming @p slotMaterials[k], and
    // enters it in the content registry (its box comes from the file's own header). Returns the path written.
    // Nothing is written when the mesh is refused: the render encoding is tried first, so a mesh the loader
    // could not derive (a colour layer, a second UV layer) never becomes a file that draws nothing.
    [[nodiscard]] inline Common::ResultStr<std::filesystem::path>
    WriteStaticMeshAsset( const Geometry::FDynamicMesh3&              mesh,
                          std::span<const Common::Content::AssetGuid> slotMaterials,
                          const std::filesystem::path& folder, std::string_view baseName )
    {
        if ( auto render = Geometry::DynamicMeshToMeshAssetData( mesh, slotMaterials ); !render.IsSuccess() )
            return Common::MakeFormattedError<std::filesystem::path>( "'{}' was not written as a static mesh: {}",
                                                                      baseName, render.GetError() );

        const std::filesystem::path path = UniqueStaticMeshPath( folder, baseName );

        Assets::MeshSourceAsset asset;
        asset.Kind              = Common::Content::ContentKind::StaticMesh;
        asset.Guid              = Common::Content::AssetGuid::Generate(); // a new file: a new identity
        asset.Name              = path.stem().string();
        asset.Import.Provenance = Assets::MeshSourceProvenance::Recovered;
        asset.Source.Models.push_back( { Geometry::ToSerialized( mesh ) } );
        asset.Source.MaterialSlots.reserve( slotMaterials.size() );
        for ( std::size_t k = 0; k < slotMaterials.size(); ++k )
            asset.Source.MaterialSlots.push_back( { "Slot" + std::to_string( k ), slotMaterials[k] } );

        std::error_code ec;
        std::filesystem::create_directories( folder, ec );
        if ( auto written = Assets::WriteMeshSourceAssetFile( path, asset ); !written.IsSuccess() )
            return Common::MakeFormattedError<std::filesystem::path>( "'{}' could not be written: {}",
                                                                      path.generic_string(), written.GetError() );
        // The file enters the registry the moment it exists (CookedJsonWrite.hpp says why).
        Assets::ContentRegistry::NoteFile( path );
        return Common::MakeSuccess( path );
    }
} // namespace Desert::Editor
