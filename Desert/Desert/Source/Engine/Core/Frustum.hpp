#pragma once

#include <Common/Core/Math/AABB.hpp>
#include <Common/Core/Math/AABBLanes.hpp>
#include <Common/Core/Math/Simd.hpp>

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>

namespace Desert::Core
{
    class Camera;

    enum FrustumPlane
    {
        PLANE_NEAR = 0,
        PLANE_LEFT,
        PLANE_RIGHT,
        PLANE_UP,
        PLANE_DOWN,
        PLANE_FAR,
    };

    class Frustum final
    {
    public:
        struct Plane
        {
            // INITIALIZED, because `Frustum() = default` is a legal way to make one and a plane read
            // before Rebuild would otherwise decide visibility from whatever was on the stack. A zero
            // normal makes every distance 0, so an unbuilt frustum accepts everything — the same
            // direction every other failure in this file is made to fall.
            glm::vec3 Normal   = glm::vec3( 0.0f );
            float     Distance = 0.0f;
        };

    public:
        Frustum() = default;

        explicit Frustum( const glm::mat4& projection, const glm::mat4& view );

        void Rebuild( const glm::mat4& projection, const glm::mat4& view );

        [[nodiscard]] bool IsInside( const glm::vec3& point ) const noexcept;

        // THE TEST THE RENDER PATH USES, and the reason the point test above could never have been it.
        //
        // An object is not a point. A building whose CENTRE is behind the camera can still fill half the
        // screen, and a wall the camera stands next to has its centre outside every side plane. Culling on
        // the centre deletes both — silently, and only from certain angles, which is the worst shape a
        // rendering defect can have.
        //
        // Standard "positive vertex" test: for each plane take the box corner FARTHEST along that plane's
        // normal; if even that corner is on the negative side, every corner is, and the box is rejected.
        // Six dot products and no square roots.
        //
        // CONSERVATIVE BY CONSTRUCTION. A box wedged past the corner where two planes meet can satisfy all
        // six half-space tests while touching no part of the frustum, so this says "visible" slightly more
        // often than geometry requires. That error costs one draw call. The opposite error costs a hole in
        // the picture, and this function will not trade in that direction.
        //
        // FOUR PLANES AT A TIME (UE FConvexVolume::IntersectBox): the planes are kept lane per plane and the six
        // tests run as two groups of four through Common/Core/Math/Simd.hpp. Each lane does the arithmetic the
        // plain per-plane loop did, in its order -- the corner picked by `n >= 0`, then
        // ((x*nx + y*ny) + z*nz) + d, then `>= 0` -- so it accepts and rejects exactly the same boxes, NaN
        // included. Inline: a caller's loop over many boxes loads the planes once.
        [[nodiscard]] bool Intersects( const Common::Math::AABB& worldBox ) const noexcept;

        // FOUR BOXES AT A TIME, box per lane (UE FConvexVolume::IntersectBox over a primitive batch): bit i set
        // when box @p index + i of @p boxes passes Intersects. The other transposition of the same test - the
        // plane is splatted and each lane is a box - with each lane's arithmetic exactly Intersects' for that
        // plane (the corner picked by `n >= 0`, then ((x*nx + y*ny) + z*nz) + d, then `>= 0`), so a lane's verdict
        // is Intersects' verdict bit for bit, NaN included. Reads lanes index..index + 3, which AABBLanes' padding
        // keeps in the allocation for any index below Size(); the caller masks the bits at or past its count.
        [[nodiscard]] uint32_t Intersects4( const Common::Math::AABBLanes& boxes,
                                            std::size_t                     index ) const noexcept;

        // A sphere (a local light's attenuation range): rejected only when it lies wholly behind one plane (the
        // planes are normalised, so the signed distance is in world units). Conservative in the same direction as
        // the box test: a sphere past a corner of two planes is kept.
        [[nodiscard]] bool IntersectsSphere( const glm::vec3& center, float radius ) const noexcept;

        // Opens the frustum on one side: @p plane accepts every point from now on (a zero plane). The sun's
        // virtual shadow map culls its casters without the plane on the sun's side: they draw with depth clamp, so
        // a caster nearer the sun than a clipmap level's depth range is flattened onto it rather than clipped (UE
        // VSM).
        void Open( const FrustumPlane plane ) noexcept
        {
            SetPlane( static_cast<uint32_t>( plane ), Plane{} );
        }

        // One plane as Rebuild / Open left it: the GPU cull (GpuDriven::InstanceCulling) tests these same six.
        [[nodiscard]] Plane GetPlane( const FrustumPlane plane ) const noexcept
        {
            const uint32_t lane = static_cast<uint32_t>( plane );
            return Plane{ glm::vec3( m_Lanes.Nx[lane], m_Lanes.Ny[lane], m_Lanes.Nz[lane] ), m_Lanes.D[lane] };
        }

    private:
        void SetPlane( const uint32_t lane, const Plane& plane ) noexcept
        {
            m_Lanes.Nx[lane] = plane.Normal.x;
            m_Lanes.Ny[lane] = plane.Normal.y;
            m_Lanes.Nz[lane] = plane.Normal.z;
            m_Lanes.D[lane]  = plane.Distance;
        }

        // Which lanes of each group of four hold a plane: 0..3, then 4 and 5. Lanes 6 and 7 are padding (zero)
        // and never vote: a zero plane gives 0 * inf = NaN for an unbounded box, which a real lane rejects -- as
        // the plain loop did for an Open() plane -- but a padding lane is not a plane at all.
        static constexpr uint32_t kGroupLanes[2] = { 0xFu, 0x3u };

        // THE planes (the only copy), lane per plane in FrustumPlane order: normal x, y, z and distance. Zero
        // until Rebuild: a zero normal makes every distance 0, so an unbuilt frustum accepts everything.
        struct PlaneLanes
        {
            float Nx[8] = {};
            float Ny[8] = {};
            float Nz[8] = {};
            float D[8]  = {};
        };
        PlaneLanes m_Lanes;
    };

    inline bool Frustum::Intersects( const Common::Math::AABB& worldBox ) const noexcept
    {
        namespace S          = Common::Math::Simd;
        const S::Float4 zero = S::Zero();
        const S::Float4 minX = S::Splat( worldBox.Min.x );
        const S::Float4 minY = S::Splat( worldBox.Min.y );
        const S::Float4 minZ = S::Splat( worldBox.Min.z );
        const S::Float4 maxX = S::Splat( worldBox.Max.x );
        const S::Float4 maxY = S::Splat( worldBox.Max.y );
        const S::Float4 maxZ = S::Splat( worldBox.Max.z );
        for ( uint32_t group = 0; group < 2u; ++group )
        {
            const uint32_t  base = group * 4u;
            const S::Float4 nx   = S::Load4( m_Lanes.Nx + base );
            const S::Float4 ny   = S::Load4( m_Lanes.Ny + base );
            const S::Float4 nz   = S::Load4( m_Lanes.Nz + base );
            const S::Float4 d    = S::Load4( m_Lanes.D + base );
            // The corner farthest along each plane's normal: three independent per-axis choices.
            const S::Float4 px   = S::Select( S::CompareGE( nx, zero ), maxX, minX );
            const S::Float4 py   = S::Select( S::CompareGE( ny, zero ), maxY, minY );
            const S::Float4 pz   = S::Select( S::CompareGE( nz, zero ), maxZ, minZ );
            const S::Float4 dot  = S::Add( S::Add( S::Mul( px, nx ), S::Mul( py, ny ) ), S::Mul( pz, nz ) );
            const uint32_t  kept = S::MaskBits( S::CompareGE( S::Add( dot, d ), zero ) );
            if ( ( kept & kGroupLanes[group] ) != kGroupLanes[group] )
                return false;
        }
        return true;
    }

    inline uint32_t Frustum::Intersects4( const Common::Math::AABBLanes& boxes,
                                          const std::size_t               index ) const noexcept
    {
        namespace S          = Common::Math::Simd;
        const S::Float4 zero = S::Zero();
        const S::Float4 minX = S::Load4( boxes.MinX.data() + index );
        const S::Float4 minY = S::Load4( boxes.MinY.data() + index );
        const S::Float4 minZ = S::Load4( boxes.MinZ.data() + index );
        const S::Float4 maxX = S::Load4( boxes.MaxX.data() + index );
        const S::Float4 maxY = S::Load4( boxes.MaxY.data() + index );
        const S::Float4 maxZ = S::Load4( boxes.MaxZ.data() + index );
        uint32_t        kept = 0xFu;
        // The six planes (lanes 0..5; 6 and 7 are padding, kGroupLanes). A box rejected by every lane already
        // stops the walk: the remaining planes could only clear bits that are clear.
        for ( uint32_t plane = 0; plane < 6u && kept != 0u; ++plane )
        {
            const float     nx  = m_Lanes.Nx[plane];
            const float     ny  = m_Lanes.Ny[plane];
            const float     nz  = m_Lanes.Nz[plane];
            // Intersects' per-lane choice `n >= 0` (a NaN normal takes Min), made once for the four boxes.
            const S::Float4 px  = nx >= 0.0f ? maxX : minX;
            const S::Float4 py  = ny >= 0.0f ? maxY : minY;
            const S::Float4 pz  = nz >= 0.0f ? maxZ : minZ;
            const S::Float4 dot = S::Add( S::Add( S::Mul( px, S::Splat( nx ) ), S::Mul( py, S::Splat( ny ) ) ),
                                          S::Mul( pz, S::Splat( nz ) ) );
            kept &= S::MaskBits( S::CompareGE( S::Add( dot, S::Splat( m_Lanes.D[plane] ) ), zero ) );
        }
        return kept;
    }
} // namespace Desert::Core
