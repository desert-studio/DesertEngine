// Ported from UE 5.8 ModelingComponents/Private/ModelingToolTargetUtil.cpp:453-500, adapted: see the header.
#include "EditedMeshAsset.hpp"

#include <Engine/Assets/MeshDerivedData.hpp>
#include <Engine/Geometry/DynamicMeshSerialization.hpp>

#include <algorithm>
#include <utility>
#include <vector>

namespace Desert::Editor
{
    Common::ResultStr<Assets::MeshSourceAsset> EditedMeshSourceAsset( const Assets::MeshSourceAsset& current,
                                                                      const Geometry::DynamicMesh3&  edited )
    {
        if ( current.Kind != Common::Content::ContentKind::StaticMesh || current.Source.Skin.has_value() )
            return Common::MakeFormattedError<Assets::MeshSourceAsset>(
                 "'{}' is not a static mesh: a modeling edit is committed into static meshes only", current.Name );
        if ( current.Source.Models.empty() )
            return Common::MakeFormattedError<Assets::MeshSourceAsset>( "'{}' has no source model", current.Name );

        // Section j of the render form the tool lifted is the j-th distinct material ID of LOD0, ascending
        // (ToRenderMesh), and the lift numbers its triangles by section.
        std::vector<int> used = current.Source.Models.front().Mesh.MaterialIds;
        std::sort( used.begin(), used.end() );
        used.erase( std::unique( used.begin(), used.end() ), used.end() );
        std::vector<Assets::MeshMaterialSlot> slots;
        for ( const int id : used )
        {
            if ( id < 0 || static_cast<std::size_t>( id ) >= current.Source.MaterialSlots.size() )
                return Common::MakeFormattedError<Assets::MeshSourceAsset>(
                     "'{}': LOD0 names material slot {} of {}", current.Name, id,
                     current.Source.MaterialSlots.size() );
            slots.push_back( current.Source.MaterialSlots[static_cast<std::size_t>( id )] );
        }
        if ( slots.empty() )
            slots = current.Source.MaterialSlots;

        std::vector<Common::Content::AssetGuid> slotMaterials;
        slotMaterials.reserve( slots.size() );
        for ( const auto& slot : slots )
            slotMaterials.push_back( slot.Material );
        if ( auto render = Geometry::DynamicMeshToMeshAssetData( edited, slotMaterials ); !render.IsSuccess() )
            return Common::MakeFormattedError<Assets::MeshSourceAsset>( "'{}': the edit cannot be written: {}",
                                                                        current.Name, render.GetError() );

        Assets::MeshSourceAsset asset      = current; // Kind, Guid, Name and the import provenance are kept
        asset.Import.Settings.UniformScale = 1.0f;
        asset.Import.Settings.UpAxis       = Assets::MeshSourceUpAxis::Y;
        asset.Source.Models.clear();
        asset.Source.Models.push_back( { Geometry::ToSerialized( edited ) } );
        asset.Source.MaterialSlots = std::move( slots );
        return Common::MakeSuccess( std::move( asset ) );
    }

    Common::BoolResultStr WriteEditedMeshAsset( const std::filesystem::path&  assetFile,
                                                const Geometry::DynamicMesh3& edited )
    {
        auto current = Assets::LoadMeshSourceAsset( assetFile );
        if ( !current.IsSuccess() )
            return Common::MakeFormattedError<bool>( "static mesh '{}': {}", assetFile.generic_string(),
                                                     current.GetError() );
        auto asset = EditedMeshSourceAsset( current.GetValue(), edited );
        if ( !asset.IsSuccess() )
            return Common::MakeError<bool>( asset.GetError() );
        if ( auto written = Assets::WriteMeshSourceAssetFile( assetFile, asset.GetValue() ); !written.IsSuccess() )
            return Common::MakeFormattedError<bool>( "static mesh '{}' could not be written: {}",
                                                     assetFile.generic_string(), written.GetError() );
        return BOOLSUCCESS;
    }
} // namespace Desert::Editor
