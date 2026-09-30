#pragma once

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace Desert::Editor::Tools
{
    // ── THE PLAYER START'S SHAPE, AS PURE FUNCTIONS (UE's APlayerStart) ─────────────────────────────
    //
    // WHAT WAS ON SCREEN. A map-marker billboard plus a two-pixel 2D line with a two-stroke "V" at its
    // end, drawn in screen space: it did not shrink with distance, it had no body, and nothing said how
    // much room the pawn needs. The owner's word for it was "ужас".
    //
    // WHAT UE DRAWS, AND WHAT THIS IS. APlayerStart is three components:
    //   (1) a CAPSULE, drawn as a wireframe in the world, the size of the pawn that will stand there —
    //       here the CharacterControllerData capsule (Radius + total Height), the one home of the pawn's
    //       size; the start's origin is the capsule's CENTRE, as UE's actor location is;
    //   (2) an ARROW (UArrowComponent) from that centre along the facing, a solid WORLD-SPACE mesh —
    //       shaft and cone head — so it foreshortens and shrinks with the camera like any geometry;
    //   (3) the billboard sprite, constant in pixels, drawn by the caller from GizmoIconSet.
    // Only yaw turns the capsule (a character stands upright whatever the start's pitch); the arrow
    // follows the full facing.

    struct GizmoSegment
    {
        glm::vec3 A{ 0.0f };
        glm::vec3 B{ 0.0f };
    };

    struct GizmoTriangle
    {
        glm::vec3 P[3]{};
        glm::vec3 Normal{ 0.0f }; // outward, unit
    };

    // UE's ArrowComponent defaults, in centimetres: ArrowSize 1 → 80 cm long; the head a fifth of it.
    inline constexpr float kPlayerStartArrowLength      = 80.0f;
    inline constexpr float kPlayerStartArrowHeadLength  = 20.0f;
    inline constexpr float kPlayerStartArrowHeadRadius  = 7.0f;
    inline constexpr float kPlayerStartArrowShaftRadius = 2.0f;
    inline constexpr int   kGizmoRoundSides             = 16;

    // The start's facing projected onto the ground plane (world up = +Y). A facing straight up or down
    // has no horizontal part; the capsule is then oriented by world -Z so the answer is still a frame.
    [[nodiscard]] inline glm::vec3 PlanarFacing( const glm::vec3& forward )
    {
        const glm::vec3 flat( forward.x, 0.0f, forward.z );
        const float     len = glm::length( flat );
        return len > 1e-5f ? flat / len : glm::vec3( 0.0f, 0.0f, -1.0f );
    }

    /**
     * @brief The pawn capsule as wire segments, centred on @p centre, upright (+Y), turned by @p forward's yaw.
     *
     * Three rings (top-hemisphere equator, bottom-hemisphere equator, and nothing at the poles), four
     * vertical lines joining the rings, and two profile outlines (along and across the facing) made of the
     * hemispheres' half-circles — UE's DrawWireCapsule. @p height is the TOTAL height, clamped so the
     * cylinder part is never negative.
     */
    [[nodiscard]] inline std::vector<GizmoSegment> BuildPawnCapsuleWire( const glm::vec3& centre,
                                                                         const glm::vec3& forward, float radius,
                                                                         float height,
                                                                         int   sides = kGizmoRoundSides )
    {
        std::vector<GizmoSegment> out;
        const glm::vec3           up( 0.0f, 1.0f, 0.0f );
        const glm::vec3           fwd     = PlanarFacing( forward );
        const glm::vec3           right   = glm::normalize( glm::cross( fwd, up ) );
        const float               halfCyl = std::max( 0.0f, height * 0.5f - radius );
        const glm::vec3           top     = centre + up * halfCyl;
        const glm::vec3           bottom  = centre - up * halfCyl;
        const float               step    = 6.28318530718f / static_cast<float>( sides );

        for ( int i = 0; i < sides; ++i ) // the two rings
        {
            const float     a0 = step * static_cast<float>( i );
            const float     a1 = step * static_cast<float>( i + 1 );
            const glm::vec3 p0 = ( fwd * std::cos( a0 ) + right * std::sin( a0 ) ) * radius;
            const glm::vec3 p1 = ( fwd * std::cos( a1 ) + right * std::sin( a1 ) ) * radius;
            out.push_back( { top + p0, top + p1 } );
            out.push_back( { bottom + p0, bottom + p1 } );
        }
        const glm::vec3 sidesDir[4] = { fwd, right, -fwd, -right };
        for ( const glm::vec3& d : sidesDir ) // the four vertical lines
            out.push_back( { bottom + d * radius, top + d * radius } );

        const int half = sides / 2; // the hemisphere arcs, in the two profile planes
        for ( const glm::vec3& axis : { fwd, right } )
            for ( int i = 0; i < half; ++i )
            {
                const float a0 = step * static_cast<float>( i );
                const float a1 = step * static_cast<float>( i + 1 );
                out.push_back( { top + ( axis * std::cos( a0 ) + up * std::sin( a0 ) ) * radius,
                                 top + ( axis * std::cos( a1 ) + up * std::sin( a1 ) ) * radius } );
                out.push_back( { bottom + ( axis * std::cos( a0 ) - up * std::sin( a0 ) ) * radius,
                                 bottom + ( axis * std::cos( a1 ) - up * std::sin( a1 ) ) * radius } );
            }
        return out;
    }

    /**
     * @brief UE's ArrowComponent as a closed solid: a cylinder shaft from @p origin and a cone head whose
     * tip is exactly @p length along @p forward. World units, so it scales with the camera.
     */
    [[nodiscard]] inline std::vector<GizmoTriangle>
    BuildArrowMesh( const glm::vec3& origin, const glm::vec3& forward, float length = kPlayerStartArrowLength,
                    float headLength = kPlayerStartArrowHeadLength, float headRadius = kPlayerStartArrowHeadRadius,
                    float shaftRadius = kPlayerStartArrowShaftRadius, int sides = kGizmoRoundSides )
    {
        std::vector<GizmoTriangle> out;
        const glm::vec3            f = glm::normalize( forward );
        const glm::vec3            ref =
             std::abs( f.y ) < 0.99f ? glm::vec3( 0.0f, 1.0f, 0.0f ) : glm::vec3( 1.0f, 0.0f, 0.0f );
        const glm::vec3 u    = glm::normalize( glm::cross( f, ref ) );
        const glm::vec3 v    = glm::cross( f, u ); // (u, v, f) right-handed: every face below winds outward
        const glm::vec3 tip  = origin + f * length;
        const glm::vec3 neck = origin + f * std::max( 0.0f, length - headLength );
        const float     step = 6.28318530718f / static_cast<float>( sides );

        const auto ring = [&]( float a ) { return u * std::cos( a ) + v * std::sin( a ); };
        const auto add  = [&]( const glm::vec3& a, const glm::vec3& b, const glm::vec3& c )
        { out.push_back( GizmoTriangle{ { a, b, c }, glm::normalize( glm::cross( b - a, c - a ) ) } ); };
        for ( int i = 0; i < sides; ++i )
        {
            const glm::vec3 r0 = ring( step * static_cast<float>( i ) );
            const glm::vec3 r1 = ring( step * static_cast<float>( i + 1 ) );
            // shaft wall (two triangles), and its back cap at the origin
            add( origin + r0 * shaftRadius, origin + r1 * shaftRadius, neck + r1 * shaftRadius );
            add( origin + r0 * shaftRadius, neck + r1 * shaftRadius, neck + r0 * shaftRadius );
            add( origin, origin + r1 * shaftRadius, origin + r0 * shaftRadius );
            // cone wall, and the cone's base disc facing back
            add( neck + r0 * headRadius, neck + r1 * headRadius, tip );
            add( neck, neck + r1 * headRadius, neck + r0 * headRadius );
        }
        return out;
    }
} // namespace Desert::Editor::Tools
