// The stack half of LandscapeEditLayers.hpp: Find and ValidateLandscapeEditLayerStack need nothing of a tile,
// so they live apart from the merge and a reader of the root's block (AuthoredComponentIO.hpp) links only this.
#include <Engine/World/Landscape/LandscapeEditLayers.hpp>

#include <cmath>
#include <cstdint>

namespace Desert::World::Landscape
{
    const LandscapeEditLayer* LandscapeEditLayerStack::Find( const Common::UUID& guid ) const
    {
        for ( const LandscapeEditLayer& layer : Layers )
            if ( static_cast<uint64_t>( layer.Guid ) == static_cast<uint64_t>( guid ) )
                return &layer;
        return nullptr;
    }

    Common::BoolResultStr ValidateLandscapeEditLayerStack( const LandscapeEditLayerStack& stack )
    {
        for ( size_t i = 0; i < stack.Layers.size(); ++i )
        {
            const LandscapeEditLayer& layer = stack.Layers[i];
            if ( layer.Guid.IsNull() )
                return Common::MakeFormattedError<bool>( "Landscape edit layer {} ('{}') has a null Guid", i,
                                                         layer.Name );
            if ( layer.Name.empty() )
                return Common::MakeFormattedError<bool>( "Landscape edit layer {} has an empty name", i );
            if ( !std::isfinite( layer.HeightAlpha ) || layer.HeightAlpha < -1.0f || layer.HeightAlpha > 1.0f )
                return Common::MakeFormattedError<bool>(
                     "Landscape edit layer '{}' height alpha {} is outside -1..1", layer.Name, layer.HeightAlpha );
            if ( !std::isfinite( layer.WeightAlpha ) || layer.WeightAlpha < 0.0f || layer.WeightAlpha > 1.0f )
                return Common::MakeFormattedError<bool>(
                     "Landscape edit layer '{}' weight alpha {} is outside 0..1", layer.Name, layer.WeightAlpha );
            for ( size_t j = 0; j < i; ++j )
                if ( static_cast<uint64_t>( stack.Layers[j].Guid ) == static_cast<uint64_t>( layer.Guid ) )
                    return Common::MakeFormattedError<bool>( "Landscape edit layers '{}' and '{}' share Guid {}",
                                                             stack.Layers[j].Name, layer.Name,
                                                             static_cast<uint64_t>( layer.Guid ) );
        }
        return Common::MakeSuccess( true );
    }
} // namespace Desert::World::Landscape
