#pragma once

// THE GUST FIELD's CPU half (WIND-FIELD-GPU): the twin of Common/WindGust.glslh (the same expressions in the same
// order; change one, change both) and the state the ECS advances it by. The GPU evaluates the field per swaying
// vertex at its instance's world point (FoliageWindOffset), from the two vec4 every view's camera block carries
// (CameraUB::WindGust / PrevWindGust, ViewFrame::WindGust): exact at any distance, the same in a scene view, a light
// view and a visibility resolve by construction, and nothing to bind beyond the block every mesh pass reads.
//
// UE: the wind source's MinGust / MaxGust modulate SpeedTree's wind over time only (one value per primitive);
// here the gusts are fronts in space that travel downwind, so a meadow shows the wind crossing it (Ghost of
// Tsushima GDC 2021).

#include <glm/glm.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <span>

namespace Desert::Graphic
{
    inline constexpr float  kWindGustPeriodCells   = 256.0f; // Common/WindGust.glslh kWindGustPeriodCells
    inline constexpr double kWindGustPeriodCellsD  = 256.0;

    [[nodiscard]] inline float WindGustHash( const float x, const float y )
    {
        const float a = glm::fract( x * 0.1031f );
        const float b = glm::fract( y * 0.1031f );
        const float s = a * ( b + 33.33f ) + b * ( a + 33.33f ) + a * ( a + 33.33f );
        return glm::fract( ( a + b + 2.0f * s ) * ( a + s ) );
    }

    [[nodiscard]] inline float WindGustLattice( const float ix, const float iy )
    {
        return WindGustHash( glm::mod( ix, kWindGustPeriodCells ), glm::mod( iy, kWindGustPeriodCells ) );
    }

    [[nodiscard]] inline float WindGustNoise( const float x, const float y )
    {
        const float ix   = std::floor( x );
        const float iy   = std::floor( y );
        const float fx   = x - ix;
        const float fy   = y - iy;
        const float ux   = fx * fx * ( 3.0f - 2.0f * fx );
        const float uy   = fy * fy * ( 3.0f - 2.0f * fy );
        const float low  = glm::mix( WindGustLattice( ix, iy ), WindGustLattice( ix + 1.0f, iy ), ux );
        const float high = glm::mix( WindGustLattice( ix, iy + 1.0f ), WindGustLattice( ix + 1.0f, iy + 1.0f ), ux );
        return glm::mix( low, high, uy );
    }

    /// The factor in [1 - Gustiness, 1] a swaying vertex's offset is scaled by at the world point (@p worldX,
    /// @p worldZ); 1 when @p gust carries no gusts (w = 0).
    [[nodiscard]] inline float WindGustScale( const float worldX, const float worldZ, const glm::vec4& gust )
    {
        if ( !( gust.w > 0.0f ) )
            return 1.0f;
        const float u = worldX * gust.z - gust.x;
        const float v = worldZ * gust.z - gust.y;
        const float n =
             0.65f * WindGustNoise( u, v ) + 0.35f * WindGustNoise( 2.0f * u + 17.0f, 2.0f * v + 31.0f );
        const float front = glm::smoothstep( 0.3f, 0.7f, n );
        return 1.0f - gust.w * ( 1.0f - front );
    }

    /// The fronts' offset (gust cells, wrapped to the lattice's period) after @p seconds of gameplay in air
    /// moving at @p velocityXZ (cm/s) with fronts @p gustSize (cm) across. Integrated, not t x v: a wind that
    /// changes speed moves the fronts on from where they are, never teleports them. Double: the step of a frame
    /// is far below a float's resolution of the offset after hours.
    [[nodiscard]] inline glm::dvec2 AdvanceWindGust( const glm::dvec2& offsetCells, const float gustSize,
                                                     const glm::vec2& velocityXZ, const double seconds )
    {
        if ( !( gustSize > 0.0f ) )
            return offsetCells;
        const glm::dvec2 step = glm::dvec2( velocityXZ ) * ( seconds / static_cast<double>( gustSize ) );
        const glm::dvec2 next = offsetCells + step;
        return { next.x - kWindGustPeriodCellsD * std::floor( next.x / kWindGustPeriodCellsD ),
                 next.y - kWindGustPeriodCellsD * std::floor( next.y / kWindGustPeriodCellsD ) };
    }

    /// CameraUB::WindGust: x, y the offset, z 1 / GustSize, w Gustiness. Zero (no gusts) when the air carries
    /// none: no gust size, no gustiness, or still air.
    [[nodiscard]] inline glm::vec4 PackWindGust( const glm::dvec2& offsetCells, const float gustiness,
                                                 const float gustSize, const glm::vec2& velocityXZ )
    {
        if ( !( gustiness > 0.0f ) || !( gustSize > 0.0f ) || !( glm::length( velocityXZ ) > 1e-6f ) )
            return glm::vec4( 0.0f );
        return { static_cast<float>( offsetCells.x ), static_cast<float>( offsetCells.y ), 1.0f / gustSize,
                 glm::clamp( gustiness, 0.0f, 1.0f ) };
    }

    // THE LOCAL WIND SOURCES (WIND-FIELD-GPU step 2; UE: a point WindDirectionalSource, which UE hands only to the
    // primitive's one GetWindParameters at its bounds; here every instance asks). The scene's point sources
    // nearest the main camera (at most kMaxLocalWindSources, SelectLocalWind), in every view's camera block
    // (CameraUB::LocalWind / LocalWindSpeed and the Prev pair), evaluated per instance at its world origin by
    // Common/LocalWind.glslh and combined with the per-draw directional wind by FoliageWindAir below.
    inline constexpr uint32_t kMaxLocalWindSources = 8; // Common/LocalWind.glslh's loop, CameraUB's arrays

    /// The sources as the camera block carries them. Source[i] = (world position cm, 1 / Radius); the speed of
    /// slot i is Speed[i / 4][i % 4], in units of the frame's reference speed (SelectLocalWind), 0 = an empty
    /// slot. The filled slots come first, so the GPU stops at the first empty one.
    struct LocalWindSources
    {
        std::array<glm::vec4, kMaxLocalWindSources>     Source{};
        std::array<glm::vec4, kMaxLocalWindSources / 4> Speed{};

        [[nodiscard]] float SpeedAt( const uint32_t slot ) const
        {
            return Speed[slot / 4][static_cast<glm::length_t>( slot % 4 )];
        }

        void SetSpeed( const uint32_t slot, const float speed )
        {
            Speed[slot / 4][static_cast<glm::length_t>( slot % 4 )] = speed;
        }
    };

    /// THE SCENE'S AIR as every view's camera block carries it: the gust fronts (PackWindGust) and the local
    /// sources.
    struct SceneWind
    {
        glm::vec4        Gust{ 0.0f };
        LocalWindSources Local;
    };

    /// The world-XZ wind the local sources add at @p point, in reference-speed units: each source's
    /// ECS::WindVelocityFrom (outward, full at the centre, linear to nothing at Radius) divided by the reference
    /// speed. The C++ twin of Common/LocalWind.glslh's LocalWindXZ: the same expressions in the same order.
    [[nodiscard]] inline glm::vec2 LocalWindXZ( const glm::vec3& point, const LocalWindSources& local )
    {
        glm::vec2 air( 0.0f );
        for ( uint32_t i = 0; i < kMaxLocalWindSources; ++i )
        {
            const float speed = local.SpeedAt( i );
            if ( !( speed > 0.0f ) )
                break;
            const glm::vec3 away     = point - glm::vec3( local.Source[i] );
            const float     distance = glm::length( away );
            const float     falloff  = glm::max( 1.0f - distance * local.Source[i].w, 0.0f );
            air += glm::vec2( away.x, away.z ) * ( speed * falloff / glm::max( distance, 1e-6f ) );
        }
        return air;
    }

    /// The direction and fraction of a type's Strength an instance sways by: the per-draw directional wind
    /// (@p directionXZ, InstanceWind::Direction: unit, or zero when no directional source blows) plus the local
    /// sources' (@p localXZ, LocalWindXZ), shortened to length 1 at most, so the offset never exceeds Strength
    /// (WindBoundsPad). No local air: the directional wind exactly, as before the local sources.
    [[nodiscard]] inline glm::vec2 FoliageWindAir( const glm::vec2& directionXZ, const glm::vec2& localXZ )
    {
        if ( localXZ.x == 0.0f && localXZ.y == 0.0f )
            return directionXZ;
        const glm::vec2 air          = directionXZ + localXZ;
        const float     lengthSquare = glm::dot( air, air );
        return lengthSquare > 1.0f ? air / std::sqrt( lengthSquare ) : air;
    }

    /// One point source as the ECS hands it to the renderer (Render::LocalWindSourceCommand).
    struct LocalWindCandidate
    {
        uint32_t  Id = 0;           // the entity: what matches a source to its previous step
        glm::vec3 Position{ 0.0f }; // world, cm
        float     Radius = 0.0f;    // cm, > 0
        float     Speed  = 0.0f;    // cm/s, > 0
    };

    /// The selected sources with their ids and raw speeds: what the next step's previous sources are matched
    /// against.
    struct LocalWindSelection
    {
        std::array<LocalWindCandidate, kMaxLocalWindSources> Picked{};
        uint32_t                                             Count          = 0;
        float                                                ReferenceSpeed = 0.0f;
    };

    /// THE SELECTION: the kMaxLocalWindSources candidates whose sphere of influence is nearest @p camera (distance
    /// to the sphere, 0 inside), packed in reference-speed units. The reference speed is the directional wind's
    /// (@p directionalSpeed, the speed the per-draw unit direction stands for), or with no directional wind the
    /// strongest picked source's, so the strongest source sways a plant at its centre by the type's whole
    /// Strength.
    /// @p prevSelection is the previous step's: a picked source found there by id packs its previous position and
    /// speed into @p prevLocal (one that was not picked then is taken as unmoved).
    inline void SelectLocalWind( const std::span<const LocalWindCandidate> candidates, const glm::vec3& camera,
                                 const float directionalSpeed, const LocalWindSelection& prevSelection,
                                 LocalWindSelection& selection, LocalWindSources& local,
                                 LocalWindSources& prevLocal )
    {
        selection = LocalWindSelection{};
        std::array<float, kMaxLocalWindSources> gap{};
        for ( const LocalWindCandidate& candidate : candidates )
        {
            const float d  = glm::max( glm::length( candidate.Position - camera ) - candidate.Radius, 0.0f );
            uint32_t    at = selection.Count;
            while ( at > 0 && gap[at - 1] > d )
                --at;
            if ( at >= kMaxLocalWindSources )
                continue;
            const uint32_t last = glm::min( selection.Count, kMaxLocalWindSources - 1 );
            for ( uint32_t k = last; k > at; --k )
            {
                gap[k]              = gap[k - 1];
                selection.Picked[k] = selection.Picked[k - 1];
            }
            gap[at]              = d;
            selection.Picked[at] = candidate;
            selection.Count      = glm::min( selection.Count + 1, kMaxLocalWindSources );
        }
        float strongest = 0.0f;
        for ( uint32_t i = 0; i < selection.Count; ++i )
            strongest = glm::max( strongest, selection.Picked[i].Speed );
        selection.ReferenceSpeed = directionalSpeed > 0.0f ? directionalSpeed : strongest;

        local     = LocalWindSources{};
        prevLocal = LocalWindSources{};
        if ( !( selection.ReferenceSpeed > 0.0f ) )
            return;
        const float prevReference =
             prevSelection.ReferenceSpeed > 0.0f ? prevSelection.ReferenceSpeed : selection.ReferenceSpeed;
        for ( uint32_t i = 0; i < selection.Count; ++i )
        {
            const LocalWindCandidate& now           = selection.Picked[i];
            LocalWindCandidate        then          = now;
            float                     thenReference = selection.ReferenceSpeed;
            for ( uint32_t k = 0; k < prevSelection.Count; ++k )
                if ( prevSelection.Picked[k].Id == now.Id )
                {
                    then          = prevSelection.Picked[k];
                    thenReference = prevReference;
                }
            local.Source[i]     = glm::vec4( now.Position, 1.0f / now.Radius );
            prevLocal.Source[i] = glm::vec4( then.Position, 1.0f / then.Radius );
            local.SetSpeed( i, now.Speed / selection.ReferenceSpeed );
            prevLocal.SetSpeed( i, then.Speed / thenReference );
        }
    }
} // namespace Desert::Graphic
