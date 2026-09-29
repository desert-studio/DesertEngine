#include <Engine/World/Landscape/LandscapeEditLayers.hpp>

#include <Engine/World/Landscape/LandscapeData.hpp>
#include <Engine/World/Landscape/LandscapePaint.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <string_view>

namespace Desert::World::Landscape
{
    namespace
    {
        bool SameGuid( const Common::UUID& a, const Common::UUID& b )
        {
            return static_cast<uint64_t>( a ) == static_cast<uint64_t>( b );
        }

        /// Whether weight layer @p name is weight-blended; nullopt when @p rules do not name it. The visibility
        /// layer is no root layer but UE's VisibilityLayer, bNoWeightBlend (LandscapePaintStroke::Rule).
        std::optional<bool> IsWeightBlended( std::string_view name, std::span<const LandscapeLayerRule> rules )
        {
            if ( name == kLandscapeVisibilityLayerName )
                return false;
            for ( const LandscapeLayerRule& rule : rules )
                if ( rule.Name == name )
                    return !rule.NoWeightBlend;
            return std::nullopt;
        }

        /// One visible layer's contribution to the merge, resolved once per call.
        struct Contribution
        {
            const LandscapeEditLayerTileData* Data        = nullptr;
            float                             HeightAlpha = 1.0f;
            float                             WeightAlpha = 1.0f;
            std::vector<size_t>               ResultIndex; ///< Per Data->Weights plane: its index in the result.
        };
    } // namespace

    Common::BoolResultStr MergeLandscapeEditLayers( const LandscapeEditLayerStack&      stack,
                                                    std::span<const LandscapeLayerRule> rules,
                                                    const LandscapeRect& rect, LandscapeTileData& tile )
    {
        if ( auto valid = ValidateLandscapeEditLayerStack( stack ); !valid )
            return valid;
        if ( rect.Area() == 0u || rect.X1 > tile.SamplesX() || rect.Z1 > tile.SamplesZ() )
            return Common::MakeFormattedError<bool>(
                 "Landscape merge region [{}, {}) x [{}, {}) is empty or leaves "
                 "the {} x {} tile",
                 rect.X0, rect.X1, rect.Z0, rect.Z1, tile.SamplesX(), tile.SamplesZ() );
        for ( const LandscapeEditLayerTileData& data : tile.EditLayers() )
            if ( stack.Find( data.Layer ) == nullptr )
                return Common::MakeFormattedError<bool>( "Landscape tile carries data of edit layer {}, which the "
                                                         "stack does not name",
                                                         static_cast<uint64_t>( data.Layer ) );

        // The result's weight planes: every plane the tile already has (inside the rectangle it becomes the
        // merge, which may be zero), then every plane a layer paints, in stack order.
        std::vector<std::string> names;
        std::vector<bool>        blended;
        auto                     indexOf = [&]( const std::string& name ) -> Common::ResultStr<size_t>
        {
            for ( size_t i = 0; i < names.size(); ++i )
                if ( names[i] == name )
                    return Common::MakeSuccess( i );
            const std::optional<bool> isBlended = IsWeightBlended( name, rules );
            if ( !isBlended )
                return Common::MakeFormattedError<size_t>( "Landscape weight layer '{}' is not a layer of the "
                                                           "landscape",
                                                           name );
            names.push_back( name );
            blended.push_back( *isBlended );
            return Common::MakeSuccess( names.size() - 1u );
        };
        for ( const LandscapeWeightLayer& layer : tile.WeightLayers() )
            if ( auto index = indexOf( layer.Name ); !index )
                return Common::MakeError<bool>( index.GetError() );

        std::vector<Contribution> contributions;
        for ( const LandscapeEditLayer& layer : stack.Layers )
        {
            const LandscapeEditLayerTileData* data = tile.FindEditLayer( layer.Guid );
            if ( !layer.Visible || data == nullptr )
                continue;
            Contribution contribution{ data, layer.HeightAlpha, layer.WeightAlpha, {} };
            for ( const LandscapeWeightLayer& plane : data->Weights )
            {
                auto index = indexOf( plane.Name );
                if ( !index )
                    return Common::MakeError<bool>( index.GetError() );
                contribution.ResultIndex.push_back( index.GetValue() );
            }
            contributions.push_back( std::move( contribution ) );
        }

        // Compute everything first: a refusal below must leave the tile as it was.
        const uint32_t        samplesX = tile.SamplesX();
        std::vector<uint16_t> heights( rect.Area() );
        std::vector<uint8_t>  weights( rect.Area() * names.size() ); // plane-major: [name][sample]
        std::vector<float>    sample( names.size() );
        constexpr auto        kMid = static_cast<float>( kLandscapeMidSample );
        size_t                out  = 0u;
        for ( uint32_t z = rect.Z0; z < rect.Z1; ++z )
            for ( uint32_t x = rect.X0; x < rect.X1; ++x, ++out )
            {
                const size_t at     = static_cast<size_t>( z ) * samplesX + x;
                float        height = 0.0f; // relative to mid: the canvas is flat at height zero
                std::fill( sample.begin(), sample.end(), 0.0f );
                for ( const Contribution& c : contributions )
                {
                    if ( !c.Data->Heights.empty() )
                        height = std::clamp( height + ( static_cast<float>( c.Data->Heights[at] ) - kMid ) *
                                                           c.HeightAlpha,
                                             -kMid, 65535.0f - kMid );

                    float cover = 0.0f; // how much of the weight-blended paint below this layer hides
                    for ( size_t p = 0; p < c.Data->Weights.size(); ++p )
                        if ( blended[c.ResultIndex[p]] )
                            cover += static_cast<float>( c.Data->Weights[p].Weights[at] );
                    cover = std::min( cover * c.WeightAlpha / 255.0f, 1.0f );
                    for ( size_t n = 0; n < names.size(); ++n )
                        if ( blended[n] )
                            sample[n] *= 1.0f - cover;
                    for ( size_t p = 0; p < c.Data->Weights.size(); ++p )
                    {
                        const size_t n = c.ResultIndex[p];
                        sample[n] += static_cast<float>( c.Data->Weights[p].Weights[at] ) * c.WeightAlpha;
                        if ( !blended[n] )
                            sample[n] = std::min( sample[n], 255.0f );
                    }
                }
                heights[out] = static_cast<uint16_t>( std::lround( height + kMid ) );
                for ( size_t n = 0; n < names.size(); ++n )
                    weights[n * rect.Area() + out] =
                         static_cast<uint8_t>( std::clamp( std::lround( sample[n] ), 0L, 255L ) );
            }

        // A plane the tile lacks is allocated only when the merge puts paint on it.
        std::vector<bool> needed( names.size() );
        size_t            allocated = tile.WeightLayers().size();
        for ( size_t n = 0; n < names.size(); ++n )
        {
            const auto first = weights.begin() + static_cast<std::ptrdiff_t>( n * rect.Area() );
            needed[n]        = tile.FindWeightLayer( names[n] ).has_value() ||
                        std::any_of( first, first + static_cast<std::ptrdiff_t>( rect.Area() ),
                                     []( uint8_t w ) { return w != 0u; } );
            if ( needed[n] && !tile.FindWeightLayer( names[n] ) && ++allocated > kLandscapeMaxWeightLayers )
                return Common::MakeFormattedError<bool>(
                     "Landscape merge would put '{}' on the tile as weight layer "
                     "{}, more than the {} a tile holds",
                     names[n], allocated, kLandscapeMaxWeightLayers );
        }

        if ( auto written = tile.WriteRegionUnchecked( rect, heights ); !written )
            return written;
        for ( size_t n = 0; n < names.size(); ++n )
        {
            if ( !needed[n] )
                continue;
            auto index = tile.AddWeightLayerUnchecked( names[n] );
            if ( !index )
                return Common::MakeError<bool>( index.GetError() );
            const auto first = weights.begin() + static_cast<std::ptrdiff_t>( n * rect.Area() );
            if ( auto written = tile.WriteWeightRegionUnchecked(
                      index.GetValue(), rect, std::span<const uint8_t>( &*first, rect.Area() ) );
                 !written )
                return written;
        }
        return Common::MakeSuccess( true );
    }
} // namespace Desert::World::Landscape
