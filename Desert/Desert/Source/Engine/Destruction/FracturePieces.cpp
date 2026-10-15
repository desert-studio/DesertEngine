#include <Engine/Destruction/FracturePieces.hpp>

#include <Common/Content/TextAssetHeader.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <format>

namespace Desert::Destruction
{
    std::vector<int32_t> DrawnPieces( const std::vector<FractureNode>& nodes )
    {
        std::vector<bool> hasChild( nodes.size(), false );
        for ( const FractureNode& node : nodes )
            if ( node.Parent >= 0 && node.Parent < static_cast<int32_t>( nodes.size() ) )
                hasChild[static_cast<size_t>( node.Parent )] = true;

        std::vector<int32_t> pieces;
        for ( size_t i = 0; i < nodes.size(); ++i )
            if ( !hasChild[i] && nodes[i].Kind == FractureNodeKind::Piece && !nodes[i].Mesh.Triangles.empty() )
                pieces.push_back( static_cast<int32_t>( i ) );
        return pieces;
    }

    std::vector<PieceInstance> PieceInstances( const std::vector<FractureNode>&          nodes,
                                               const glm::mat4&                          entityWorld,
                                               std::span<const std::optional<glm::mat4>> simulatedNodeWorld,
                                               std::span<const glm::dvec3>               previewOffsets )
    {
        const bool simulated = !simulatedNodeWorld.empty();
        const bool exploded  = !simulated && previewOffsets.size() == nodes.size();

        std::vector<PieceInstance> instances;
        for ( const int32_t node : DrawnPieces( nodes ) )
        {
            const auto index = static_cast<size_t>( node );
            if ( simulated )
            {
                if ( index >= simulatedNodeWorld.size() || !simulatedNodeWorld[index].has_value() )
                    continue; // its body left the simulation
                instances.push_back( { node, *simulatedNodeWorld[index] } );
                continue;
            }
            const glm::mat4 local = exploded
                                         ? glm::translate( glm::mat4( 1.0f ), glm::vec3( previewOffsets[index] ) )
                                         : glm::mat4( 1.0f );
            instances.push_back( { node, entityWorld * local } );
        }
        return instances;
    }

    std::vector<PieceSubmeshMaterial> PieceSubmeshMaterials( std::span<const int> submeshMaterialIds,
                                                             int32_t interiorMaterialId, size_t sourceSlotCount )
    {
        std::vector<PieceSubmeshMaterial> materials;
        materials.reserve( submeshMaterialIds.size() );
        for ( const int id : submeshMaterialIds )
        {
            if ( id == interiorMaterialId )
            {
                materials.push_back( { true, 0u } );
                continue;
            }
            const size_t last = sourceSlotCount == 0 ? 0 : sourceSlotCount - 1;
            materials.push_back(
                 { false, static_cast<uint32_t>( std::min( static_cast<size_t>( std::max( id, 0 ) ), last ) ) } );
        }
        return materials;
    }

    InteriorMaterialChoice
    ChooseInteriorMaterial( const FractureData& fracture, const std::string& fractureName,
                            const std::function<bool( const Common::Content::AssetGuid& )>& isMaterialAsset )
    {
        InteriorMaterialChoice choice;
        if ( fracture.InteriorMaterial.IsNull() )
        {
            choice.Error = std::format( "fracture '{}' has no interior material (Fracture Mode -> Interior "
                                        "Material); its interior faces (material ID {}) draw with the default "
                                        "surface",
                                        fractureName, fracture.InteriorMaterialId );
            return choice;
        }
        if ( !isMaterialAsset( fracture.InteriorMaterial ) )
        {
            choice.Error =
                 std::format( "fracture '{}' names interior material {}, which is not a material asset "
                              "in the content registry; its interior faces (material ID {}) draw with "
                              "the default surface",
                              fractureName, Common::Content::AssetGuidToText( fracture.InteriorMaterial ),
                              fracture.InteriorMaterialId );
            return choice;
        }
        choice.Material = fracture.InteriorMaterial;
        return choice;
    }
} // namespace Desert::Destruction
