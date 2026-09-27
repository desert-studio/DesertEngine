#pragma once

// A landscape root's target layers as the paint stroke's rules. Its own header, not LandscapeRootOf.hpp:
// that one is compiled by the loader, physics and a dozen suites that have no business with the asset
// loader this needs.

#include <Engine/ECS/Components.hpp>
#include <Engine/Runtime/Services/Landscape/LandscapeLayerInfoService.hpp>
#include <Engine/World/Landscape/LandscapePaint.hpp>

#include <Common/Core/ResultStr.hpp>

#include <string>
#include <vector>

namespace Desert::ECS
{
    /// The paint stroke's layer rules, in the component's order — the one conversion from the authored layer
    /// list, so the panel's Hardness and the stroke's normalisation cannot read two different lists. A layer
    /// whose `.delayerinfo` is still loading or failed is an ERROR naming it, not a skipped layer: a stroke
    /// normalised without one layer would move weight out of that layer's plane silently.
    inline Common::ResultStr<std::vector<World::Landscape::LandscapeLayerRule>>
    LandscapeLayerRulesOf( const LandscapeComponent& c, Runtime::LandscapeLayerInfoService& layers )
    {
        using Rules = std::vector<World::Landscape::LandscapeLayerRule>;
        Rules rules;
        rules.reserve( c.Layers.size() );
        for ( size_t i = 0; i < c.Layers.size(); ++i )
        {
            const auto* info = layers.Get( c.Layers[i] );
            if ( !info )
                return Common::MakeFormattedError<Rules>(
                     "target layer {} (layer info {}) is {}", i, static_cast<uint64_t>( c.Layers[i] ),
                     layers.StateOf( c.Layers[i] ) == Runtime::LandscapeLayerInfoService::State::Pending
                          ? std::string( "still loading" )
                          : "unusable: " + layers.ErrorOf( c.Layers[i] ) );
            rules.push_back( { info->LayerName, info->Hardness, info->NoWeightBlend } );
        }
        return Common::MakeSuccess( std::move( rules ) );
    }

} // namespace Desert::ECS
