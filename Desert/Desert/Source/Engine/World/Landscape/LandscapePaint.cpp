// Ported from UE 5.8 Engine/Source/Editor/LandscapeEditor/Private/LandscapeEdModePaintTools.cpp:258-445
// (FLandscapeToolStrokePaint::Apply), adapted: see LandscapePaint.hpp.
#include <Engine/World/Landscape/LandscapePaint.hpp>

#include <Common/Core/Core.hpp>

#include <algorithm>
#include <cmath>

namespace Desert::World::Landscape
{
    namespace
    {
        /// UE's FWeightmapToolTarget::StrengthMultiplier: weights are 0..255, so a strength of 1 is 255 a step.
        constexpr float kWeightStrengthMultiplier = 255.0f;

        uint8_t ClampWeight( int32_t value )
        {
            return static_cast<uint8_t>( std::clamp( value, 0, 255 ) );
        }
    } // namespace

    void LandscapeNormalizeWeights( std::span<uint8_t> weights, std::span<const LandscapeLayerRule> rules,
                                    size_t painted, uint8_t value )
    {
        DESERT_VERIFY( weights.size() == rules.size() && painted < weights.size(),
                       "Normalising {} weights against {} rules, painted layer {}", weights.size(), rules.size(),
                       painted );
        if ( rules[painted].NoWeightBlend )
        {
            weights[painted] = value;
            return;
        }

        int32_t sumOthers = 0;
        for ( size_t i = 0; i < weights.size(); ++i )
            if ( i != painted && !rules[i].NoWeightBlend )
                sumOthers += weights[i];
        if ( sumOthers == 0 )
        {
            // Nothing else holds weight here: a gain is simply taken, a loss has nowhere to go.
            if ( value > weights[painted] )
                weights[painted] = value;
            return;
        }

        const int32_t      target = 255 - static_cast<int32_t>( value );
        const float        delta  = static_cast<float>( sumOthers - target ); // > 0: the others lose weight
        std::vector<float> next( weights.size(), 0.0f );
        if ( delta > 0.0f )
        {
            // Soft layers give first, in proportion to weight · (1 - Hardness)...
            float capacity = 0.0f;
            for ( size_t i = 0; i < weights.size(); ++i )
                if ( i != painted && !rules[i].NoWeightBlend )
                    capacity += weights[i] * ( 1.0f - std::clamp( rules[i].Hardness, 0.0f, 1.0f ) );
            const float softTake = std::min( delta, capacity );
            float       hardLeft = 0.0f;
            for ( size_t i = 0; i < weights.size(); ++i )
            {
                if ( i == painted || rules[i].NoWeightBlend )
                    continue;
                const float cap = weights[i] * ( 1.0f - std::clamp( rules[i].Hardness, 0.0f, 1.0f ) );
                next[i]         = weights[i] - ( capacity > 0.0f ? softTake * cap / capacity : 0.0f );
                hardLeft += next[i];
            }
            // ...and only what they cannot give comes from what the harder layers have left.
            const float hardTake = delta - softTake;
            if ( hardTake > 0.0f && hardLeft > 0.0f )
                for ( size_t i = 0; i < weights.size(); ++i )
                    if ( i != painted && !rules[i].NoWeightBlend )
                        next[i] -= hardTake * next[i] / hardLeft;
        }
        else
        {
            for ( size_t i = 0; i < weights.size(); ++i )
                if ( i != painted && !rules[i].NoWeightBlend )
                    next[i] = weights[i] * static_cast<float>( target ) / static_cast<float>( sumOthers );
        }

        int32_t total    = 0;
        size_t  heaviest = weights.size();
        for ( size_t i = 0; i < weights.size(); ++i )
        {
            if ( i == painted || rules[i].NoWeightBlend )
                continue;
            weights[i] = ClampWeight( static_cast<int32_t>( std::lround( next[i] ) ) );
            total += weights[i];
            if ( heaviest == weights.size() || weights[i] > weights[heaviest] )
                heaviest = i;
        }
        weights[painted] = value;
        // Rounding residue: to the heaviest other layer, so the weight-blended sum is exactly 255.
        weights[heaviest] = ClampWeight( weights[heaviest] + ( target - total ) );
    }

    LandscapePaintStroke::LandscapePaintStroke( const LandscapeRoot& root, LandscapeTileLookup lookup,
                                                std::vector<LandscapeLayerRule> rules )
         : m_Root( root ), m_Lookup( std::move( lookup ) ), m_Rules( std::move( rules ) )
    {
    }

    const LandscapeLayerRule* LandscapePaintStroke::Rule( std::string_view name ) const
    {
        for ( const LandscapeLayerRule& rule : m_Rules )
            if ( rule.Name == name )
                return &rule;
        return nullptr;
    }

    LandscapePaintStroke::TileState& LandscapePaintStroke::StateFor( int32_t tileX, int32_t tileZ,
                                                                     const LandscapeTileData& tile )
    {
        for ( TileState& state : m_Tiles )
            if ( state.TileX == tileX && state.TileZ == tileZ )
                return state;
        TileState state;
        state.TileX    = tileX;
        state.TileZ    = tileZ;
        state.Original = tile.WeightLayers();
        state.Influence.assign( static_cast<size_t>( tile.SamplesX() ) * tile.SamplesZ(), 0.0f );
        m_Tiles.push_back( std::move( state ) );
        return m_Tiles.back();
    }

    Common::BoolResultStr LandscapePaintStroke::Apply( const LandscapeBrushWeights&  weights,
                                                       const LandscapeBrushSettings& brush,
                                                       const LandscapePaintSettings& paint, bool invert )
    {
        if ( Rule( paint.Layer ) == nullptr )
            return Common::MakeFormattedError<bool>( "Paint target layer '{}' is not one of the landscape's {} "
                                                     "layers",
                                                     paint.Layer, m_Rules.size() );
        if ( weights.Empty() )
            return Common::MakeSuccess( true );

        // UE: PaintStrength = strength · pressure · AdjustedStrength, at least 1 unless lerping to a target.
        float         paintStrength = brush.Strength * kWeightStrengthMultiplier;
        const bool    useTarget     = paint.UseTargetValue && !invert; // erasing cancels target-value mode (UE)
        const int32_t destValue     = ClampWeight( static_cast<int32_t>( 255.0f * paint.TargetValue ) );
        if ( paintStrength <= 0.0f )
            return Common::MakeSuccess( true );
        if ( !useTarget )
            paintStrength = std::max( paintStrength, 1.0f );

        for ( const LandscapeBrushTile& piece : LandscapeBrushTiles( m_Root, weights ) )
        {
            const LandscapeTileSlot slot = m_Lookup( piece.TileX, piece.TileZ );
            if ( slot.State != LandscapeTileState::Present )
                continue;
            LandscapeTileData& tile = *slot.Data;
            for ( const LandscapeWeightLayer& layer : tile.WeightLayers() )
                if ( Rule( layer.Name ) == nullptr )
                    return Common::MakeFormattedError<bool>( "Tile ({}, {}) carries weight layer '{}', which the "
                                                             "landscape no longer names",
                                                             piece.TileX, piece.TileZ, layer.Name );
            TileState& state = StateFor( piece.TileX, piece.TileZ, tile );
            auto       added = tile.AddWeightLayer( paint.Layer );
            if ( !added )
                return Common::MakeError<bool>( added.GetError() );
            const size_t target = added.GetValue();

            // The step works on copies of the rectangle's planes, then writes each layer back once.
            const size_t                      layerCount = tile.WeightLayers().size();
            std::vector<std::vector<uint8_t>> planes( layerCount );
            for ( size_t l = 0; l < layerCount; ++l )
            {
                auto read = tile.ReadWeightRegion( l, piece.Samples );
                if ( !read )
                    return Common::MakeError<bool>( read.GetError() );
                planes[l] = read.GetValue();
            }
            std::vector<LandscapeLayerRule> rules;
            for ( const LandscapeWeightLayer& layer : tile.WeightLayers() )
                rules.push_back( *Rule( layer.Name ) );
            const auto original =
                 std::find_if( state.Original.begin(), state.Original.end(),
                               [&]( const LandscapeWeightLayer& l ) { return l.Name == paint.Layer; } );

            std::vector<uint8_t> sample( layerCount );
            for ( uint32_t z = piece.Samples.Z0; z < piece.Samples.Z1; ++z )
                for ( uint32_t x = piece.Samples.X0; x < piece.Samples.X1; ++x )
                {
                    // UE's BrushValue is the falloff weight alone; ComputeLandscapeBrush folds the tool strength
                    // into every weight, and PaintStrength multiplies it again below (as does the influence map),
                    // so the weight is divided back out — as LandscapeSculpt's BrushValue does. paintStrength > 0
                    // here, so brush.Strength > 0.
                    const float brushValue =
                         LandscapeBrushTileWeight( m_Root, weights, piece.TileX, piece.TileZ, x, z ) /
                         brush.Strength;
                    const size_t tileIndex = static_cast<size_t>( z ) * tile.SamplesX() + x;
                    const size_t local     = static_cast<size_t>( z - piece.Samples.Z0 ) * piece.Samples.Width() +
                                         ( x - piece.Samples.X0 );
                    float&        influence = state.Influence[tileIndex];
                    const int32_t current   = planes[target][local];

                    // UE's startup slowdown: the value added to drifts from the stroke's original towards the
                    // current one as the brush dwells, so the first dabs of a stroke are gentle.
                    int32_t source = original != state.Original.end() ? original->Weights[tileIndex] : 0;
                    if ( paint.DisableStartupSlowdown )
                        source = current;
                    else
                        source = static_cast<int32_t>( static_cast<float>( source ) +
                                                       static_cast<float>( current - source ) *
                                                            std::min( influence * 0.05f, 1.0f ) );
                    influence += brushValue;
                    if ( brushValue <= 0.0f )
                        continue;

                    const float paintAmount = brushValue * paintStrength;
                    int32_t     next        = current;
                    if ( useTarget )
                        next = static_cast<int32_t>( static_cast<float>( current ) +
                                                     static_cast<float>( destValue - current ) *
                                                          ( paintAmount / kWeightStrengthMultiplier ) );
                    else if ( invert )
                        next = std::min( source - static_cast<int32_t>( std::lround( paintAmount ) ), current );
                    else
                        next = std::max( source + static_cast<int32_t>( std::lround( paintAmount ) ), current );
                    if ( ClampWeight( next ) == current )
                        continue;

                    for ( size_t l = 0; l < layerCount; ++l )
                        sample[l] = planes[l][local];
                    LandscapeNormalizeWeights( sample, rules, target, ClampWeight( next ) );
                    for ( size_t l = 0; l < layerCount; ++l )
                        planes[l][local] = sample[l];
                }
            for ( size_t l = 0; l < layerCount; ++l )
                if ( auto written = tile.WriteWeightRegion( l, piece.Samples, planes[l] ); !written )
                    return written;
        }
        return Common::MakeSuccess( true );
    }

    Common::ResultStr<LandscapePaintRecord> LandscapePaintStroke::Finish() const
    {
        if ( m_Tiles.empty() )
            return Common::MakeFormattedError<LandscapePaintRecord>( "Paint stroke touched no loaded tile" );
        LandscapePaintRecord record;
        for ( const TileState& state : m_Tiles )
        {
            const LandscapeTileSlot slot = m_Lookup( state.TileX, state.TileZ );
            if ( slot.State != LandscapeTileState::Present )
                return Common::MakeFormattedError<LandscapePaintRecord>( "Tile ({}, {}) was unloaded during the "
                                                                         "stroke",
                                                                         state.TileX, state.TileZ );
            record.Tiles.push_back( { state.TileX, state.TileZ, state.Original, slot.Data->WeightLayers() } );
        }
        return Common::MakeSuccess( std::move( record ) );
    }

    Common::BoolResultStr ApplyLandscapePaintRecord( LandscapeTileLookup         lookup,
                                                     const LandscapePaintRecord& record, bool before )
    {
        for ( const LandscapePaintTileRecord& tile : record.Tiles )
            if ( lookup( tile.TileX, tile.TileZ ).State != LandscapeTileState::Present )
                return Common::MakeFormattedError<bool>( "Paint undo needs tile ({}, {}), which is not loaded",
                                                         tile.TileX, tile.TileZ );
        for ( const LandscapePaintTileRecord& tile : record.Tiles )
            if ( auto set =
                      lookup( tile.TileX, tile.TileZ ).Data->SetWeightLayers( before ? tile.Before : tile.After );
                 !set )
                return set;
        return Common::MakeSuccess( true );
    }
} // namespace Desert::World::Landscape
