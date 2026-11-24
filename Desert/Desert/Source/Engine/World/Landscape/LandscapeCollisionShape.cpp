#include <Engine/World/Landscape/LandscapeCollisionShape.hpp>

namespace Desert::World::Landscape
{
    void DecodeLandscapeCollisionHeights( const LandscapeTileData& tile, float zScale, std::vector<float>& out )
    {
        const auto& samples = tile.Samples();
        out.resize( samples.size() );
        for ( size_t i = 0; i < samples.size(); ++i )
            out[i] = LandscapeHeightCm( samples[i], zScale );
        if ( const auto visibility = tile.VisibilityLayer() )
        {
            const std::vector<uint8_t>& weights = tile.WeightLayers()[*visibility].Weights;
            for ( size_t i = 0; i < weights.size(); ++i )
                if ( LandscapeWeightIsHole( weights[i] ) )
                    out[i] = Physics::kHeightFieldNoCollision;
        }
    }

    Physics::HeightFieldDesc LandscapeHeightFieldDesc( const LandscapeTileData& tile, const LandscapeFrame& frame,
                                                       const std::vector<float>& heights )
    {
        Physics::HeightFieldDesc desc;
        desc.Position    = glm::vec3( frame.OriginX, frame.BaseY, frame.OriginZ );
        desc.SampleCount = tile.SamplesX();
        desc.SpacingCm   = frame.SpacingCm;
        desc.HeightsCm   = heights;
        return desc;
    }
} // namespace Desert::World::Landscape
