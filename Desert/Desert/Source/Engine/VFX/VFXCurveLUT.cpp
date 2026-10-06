#include <Engine/VFX/VFXCurveLUT.hpp>

#include <Engine/Animation/KeyInterpolation.hpp>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <format>

namespace Desert::VFX
{
    namespace S = Assets::Serialization;

    float EvaluateCurve( std::span<const S::VFXCurveKey> keys, float time )
    {
        if ( time <= keys.front().Time )
            return keys.front().Value;
        if ( time >= keys.back().Time )
            return keys.back().Value;
        // The first key AFTER time; the segment is the key before it to it.
        const auto            next = std::upper_bound( keys.begin(), keys.end(), time,
                                                       []( float t, const S::VFXCurveKey& k ) { return t < k.Time; } );
        const S::VFXCurveKey& a    = *( next - 1 );
        const S::VFXCurveKey& b    = *next;
        const float           span = b.Time - a.Time;
        return Animation::EvaluateSegment( a.Value, a.LeaveTangent, b.Value, b.ArriveTangent, a.Interp,
                                           static_cast<double>( span ), ( time - a.Time ) / span );
    }

    VFXCurveLUTEntry BakeCurveLUT( const std::vector<std::vector<S::VFXCurveKey>>& channels,
                                   std::vector<float>& atlas, uint32_t samples )
    {
        VFXCurveLUTEntry entry;
        entry.Offset   = static_cast<uint32_t>( atlas.size() );
        entry.Channels = static_cast<uint32_t>( channels.size() );
        entry.Samples  = samples;

        // UpdateTimeRanges (NiagaraDataInterfaceVectorCurve.cpp:76-98): the union of the channels' key ranges.
        entry.MinTime = FLT_MAX;
        entry.MaxTime = -FLT_MAX;
        for ( const auto& keys : channels )
        {
            entry.MinTime = std::min( entry.MinTime, keys.front().Time );
            entry.MaxTime = std::max( entry.MaxTime, keys.back().Time );
        }
        // UE divides by zero here when every key sits at one time; every sample is then the same value, so the
        // range collapses to MinTime instead of an infinity the shader would multiply by zero (NaN).
        entry.InvTimeRange = entry.MaxTime > entry.MinTime ? 1.0f / ( entry.MaxTime - entry.MinTime ) : 0.0f;

        // BuildLUT (:100-114): sample i at UnnormalizeTime( i / (N - 1) ), channels interleaved.
        const float invEntry = samples > 1 ? 1.0f / static_cast<float>( samples - 1 ) : 0.0f;
        atlas.reserve( atlas.size() + static_cast<std::size_t>( samples ) * channels.size() );
        for ( uint32_t i = 0; i < samples; ++i )
        {
            const float t = std::lerp( entry.MinTime, entry.MaxTime, static_cast<float>( i ) * invEntry );
            for ( const auto& keys : channels )
                atlas.push_back( EvaluateCurve( keys, t ) );
        }
        return entry;
    }

    const VFXCurveLUTEntry* VFXCurveAtlas::Find( const VFXCurveRef& ref ) const
    {
        for ( const auto& [r, e] : Entries )
            if ( r == ref )
                return &e;
        return nullptr;
    }

    Common::ResultStr<VFXCurveAtlas> BuildCurveAtlas( const S::VFXSystemData& system )
    {
        VFXCurveAtlas atlas;
        for ( std::size_t e = 0; e < system.Emitters.size(); ++e )
            for ( const VFXStackGroup group : { VFXStackGroup::ParticleSpawn, VFXStackGroup::ParticleUpdate } )
            {
                const auto& uses = group == VFXStackGroup::ParticleSpawn ? system.Emitters[e].Stack.ParticleSpawn
                                                                         : system.Emitters[e].Stack.ParticleUpdate;
                for ( std::size_t m = 0; m < uses.size(); ++m )
                {
                    if ( !uses[m].Enabled )
                        continue;
                    for ( const S::VFXModuleInput& in : uses[m].Inputs )
                    {
                        if ( in.Source != S::VFXInputSource::Curve || !in.Curve )
                            continue;
                        atlas.Entries.emplace_back( VFXCurveRef{ e, group, static_cast<uint32_t>( m ), in.Name },
                                                    BakeCurveLUT( *in.Curve, atlas.Floats ) );
                    }
                }
            }
        // The offset travels as a float component of the parameter row: exact only below 2^24.
        if ( atlas.Floats.size() > ( std::size_t( 1 ) << 24 ) )
            return Common::MakeFormattedError<VFXCurveAtlas>(
                 "the system's curve atlas is {} floats; a curve offset is exact only below 2^24",
                 atlas.Floats.size() );
        return Common::MakeSuccess( std::move( atlas ) );
    }

    glm::vec4 CurveParamRow( const VFXCurveLUTEntry& entry )
    {
        return { entry.MinTime, entry.InvTimeRange, static_cast<float>( entry.Offset ),
                 static_cast<float>( entry.Samples - 1 ) };
    }

    glm::vec4 SampleCurveLUT( std::span<const float> atlas, const VFXCurveLUTEntry& entry, float time )
    {
        // GetCurveLUTIndices (NiagaraDataInterfaceCurveTemplate.ush): saturate the normalised time, then the two
        // neighbouring samples and the fraction between them.
        const float    last  = static_cast<float>( entry.Samples - 1 );
        const float    x     = std::clamp( ( time - entry.MinTime ) * entry.InvTimeRange, 0.0f, 1.0f ) * last;
        const float    a     = std::floor( x );
        const float    b     = std::min( a + 1.0f, last );
        const float    f     = x - a;
        const uint32_t ia    = entry.Offset + static_cast<uint32_t>( a ) * entry.Channels;
        const uint32_t ib    = entry.Offset + static_cast<uint32_t>( b ) * entry.Channels;
        glm::vec4      value = glm::vec4( 0.0f );
        for ( uint32_t c = 0; c < entry.Channels && c < 4; ++c )
            value[static_cast<int>( c )] = std::lerp( atlas[ia + c], atlas[ib + c], f );
        return value;
    }
} // namespace Desert::VFX
