#include <Engine/Destruction/FractureEdit.hpp>

#include <algorithm>

namespace Desert::Destruction
{
    std::vector<glm::dvec3> ExplodedOffsets( const std::vector<FractureNode>& nodes,
                                             const FractureViewSettings&      view )
    {
        std::vector<glm::dvec3> offsets( nodes.size(), glm::dvec3( 0.0 ) );
        if ( nodes.empty() || view.ExplodeAmount == 0.0f )
            return offsets;

        const double     amount = view.ExplodeAmount;
        const glm::dvec3 root   = nodes[0].CenterOfMass;
        // Parents precede children (FractureBake's order, checked by DecodeFracturePayload), so one forward pass
        // sees every parent's offset before its children.
        for ( size_t i = 1; i < nodes.size(); ++i )
        {
            const FractureNode& node   = nodes[i];
            const size_t        parent = static_cast<size_t>( node.Parent );
            if ( view.ViewLevel < 0 )
                offsets[i] = offsets[parent] + ( node.CenterOfMass - nodes[parent].CenterOfMass ) * amount;
            else if ( node.Level == static_cast<uint32_t>( view.ViewLevel ) )
                offsets[i] = ( node.CenterOfMass - root ) * amount;
            else
                offsets[i] = offsets[parent]; // above the level: 0; below it: moves with its level-N ancestor
        }
        return offsets;
    }

    Common::ResultStr<FractureData> GenerateFracture( const Geometry::DynamicMesh3&     source,
                                                      const Common::Content::AssetGuid& sourceMesh,
                                                      const FractureData&               current,
                                                      const FractureSettings&           settings )
    {
        auto baked = BakeFracture( source, settings );
        if ( !baked )
            return Common::MakeFormattedError<FractureData>( "Generate refused: {}", baked.GetError() );

        FractureData next;
        next.Guid               = current.Guid;
        next.SourceMesh         = sourceMesh;
        next.InteriorMaterial   = current.InteriorMaterial;
        next.Settings           = settings;
        next.InteriorMaterialId = baked.GetValue().InteriorMaterialId;
        next.Nodes              = std::move( baked.GetValue().Nodes );
        return Common::MakeSuccess( std::move( next ) );
    }

    uint32_t DeepestLevel( const std::vector<FractureNode>& nodes )
    {
        uint32_t deepest = 0;
        for ( const FractureNode& node : nodes )
            deepest = std::max( deepest, node.Level );
        return deepest;
    }
} // namespace Desert::Destruction
