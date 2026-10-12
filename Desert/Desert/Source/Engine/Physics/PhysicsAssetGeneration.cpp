#include "PhysicsAssetGeneration.hpp"

#include <Engine/Animation/Skeleton.hpp>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace Desert::Physics
{
    namespace
    {
        constexpr float kMinRadiusCm = 0.5f; // a capsule thinner than this cannot be grabbed or simulated sanely

        glm::quat DescaledRotation( const glm::mat4& m )
        {
            glm::mat3 r( m );
            for ( int c = 0; c < 3; ++c )
                r[c] = glm::normalize( r[c] );
            return glm::normalize( glm::quat_cast( r ) );
        }

        // The shortest turn taking unit @p from onto unit @p to.
        glm::quat TurnFromTo( const glm::vec3& from, const glm::vec3& to )
        {
            const float d = glm::dot( from, to );
            if ( d > 0.999999f )
                return glm::quat( 1.0f, 0.0f, 0.0f, 0.0f );
            if ( d < -0.999999f )
            {
                glm::vec3 axis = glm::cross( glm::vec3( 1.0f, 0.0f, 0.0f ), from );
                if ( glm::dot( axis, axis ) < 1e-6f )
                    axis = glm::cross( glm::vec3( 0.0f, 1.0f, 0.0f ), from );
                return glm::angleAxis( glm::pi<float>(), glm::normalize( axis ) );
            }
            const glm::vec3 c = glm::cross( from, to );
            return glm::normalize( glm::quat( 1.0f + d, c.x, c.y, c.z ) );
        }

        // Longest side of the box around @p points in the frame (origin, rotation).
        float LongestSide( const std::vector<glm::vec3>& points, const glm::vec3& origin,
                           const glm::quat& rotation )
        {
            if ( points.empty() )
                return 0.0f;
            const glm::quat toLocal = glm::conjugate( rotation );
            glm::vec3       lo( std::numeric_limits<float>::max() );
            glm::vec3       hi( -std::numeric_limits<float>::max() );
            for ( const glm::vec3& p : points )
            {
                const glm::vec3 l = toLocal * ( p - origin );
                lo                = glm::min( lo, l );
                hi                = glm::max( hi, l );
            }
            const glm::vec3 size = hi - lo;
            return std::max( size.x, std::max( size.y, size.z ) );
        }
    } // namespace

    Common::ResultStr<PhysicsAssetData> GeneratePhysicsAsset( const Animation::Skeleton&            skeleton,
                                                              std::span<const SkinnedVertex>        vertices,
                                                              const Common::Content::AssetGuid&     skeletonGuid,
                                                              const PhysicsAssetGenerationSettings& settings )
    {
        if ( !skeleton.GetStructureError().empty() )
            return Common::MakeFormattedError<PhysicsAssetData>( "the skeleton is malformed: {}",
                                                                 skeleton.GetStructureError() );
        const auto&    bones = skeleton.GetBones();
        const uint32_t count = static_cast<uint32_t>( bones.size() );
        if ( count == 0 )
            return Common::MakeFormattedError<PhysicsAssetData>( "the skeleton has no bones" );
        if ( vertices.empty() )
            return Common::MakeFormattedError<PhysicsAssetData>( "the mesh has no vertices to fit bodies to" );

        std::vector<glm::mat4> bind;
        skeleton.ResolveComponentSpace( [&]( uint32_t i ) { return bones[i].LocalBindTransform; }, bind );
        std::vector<glm::vec3> origin( count );
        std::vector<glm::quat> rotation( count );
        for ( uint32_t b = 0; b < count; ++b )
        {
            origin[b]   = glm::vec3( bind[b][3] );
            rotation[b] = DescaledRotation( bind[b] );
        }
        const auto parentOf = [&]( uint32_t bone ) -> std::optional<uint32_t>
        {
            const uint32_t parent = skeleton.ResolveParent( bone );
            return parent < count ? std::optional<uint32_t>( parent ) : std::nullopt;
        };

        // Dominant weight: each vertex to its heaviest influence.
        std::vector<std::vector<glm::vec3>> points( count );
        for ( const SkinnedVertex& v : vertices )
        {
            std::size_t heaviest = 0;
            for ( std::size_t i = 1; i < SkinnedVertex::MAX_BONE_INFLUENCES; ++i )
                if ( v.BoneWeights[i] > v.BoneWeights[heaviest] )
                    heaviest = i;
            if ( v.BoneWeights[heaviest] <= 0.0f || v.BoneIDs[heaviest] >= count )
                continue;
            points[v.BoneIDs[heaviest]].push_back( v.StaticVertex.Position );
        }

        // Children first: a too-small bone hands its vertices up; "carries" marks a bone with a body at or below.
        const auto&       order = skeleton.GetResolveOrder();
        std::vector<bool> hasBody( count, false );
        std::vector<bool> carries( count, false );
        for ( auto it = order.rbegin(); it != order.rend(); ++it )
        {
            const uint32_t bone = *it;
            const auto     up   = parentOf( bone );
            if ( !points[bone].empty() &&
                 LongestSide( points[bone], origin[bone], rotation[bone] ) >= settings.MinBoneSizeCm )
                hasBody[bone] = true;
            else if ( up )
            {
                auto& target = points[*up];
                target.insert( target.end(), points[bone].begin(), points[bone].end() );
                points[bone].clear();
            }
            carries[bone] = carries[bone] || hasBody[bone];
            if ( up && carries[bone] )
                carries[*up] = true;
        }

        PhysicsAssetData data;
        data.Skeleton = skeletonGuid;
        for ( const uint32_t bone : order )
        {
            if ( !hasBody[bone] )
                continue;
            const auto& pts = points[bone];

            // The axis: towards the first child that leads to a body, else towards the vertices.
            glm::vec3 direction( 0.0f );
            for ( uint32_t c = 0; c < count; ++c )
                if ( carries[c] && parentOf( c ) == bone )
                {
                    direction = origin[c] - origin[bone];
                    break;
                }
            if ( glm::dot( direction, direction ) < 1e-6f )
            {
                glm::vec3 centroid( 0.0f );
                for ( const glm::vec3& p : pts )
                    centroid += p;
                direction = centroid / static_cast<float>( pts.size() ) - origin[bone];
            }
            const glm::quat toBone    = glm::conjugate( rotation[bone] );
            const glm::vec3 axisLocal = glm::dot( direction, direction ) < 1e-6f
                                             ? glm::vec3( 0.0f, 0.0f, 1.0f )
                                             : glm::normalize( toBone * direction );

            const glm::quat        capsule   = TurnFromTo( glm::vec3( 0.0f, 0.0f, 1.0f ), axisLocal );
            const glm::quat        toCapsule = glm::conjugate( capsule );
            std::vector<glm::vec3> local;
            local.reserve( pts.size() );
            glm::vec2 meanXY( 0.0f );
            float     zMin = std::numeric_limits<float>::max();
            float     zMax = -std::numeric_limits<float>::max();
            for ( const glm::vec3& p : pts )
            {
                const glm::vec3 l = toCapsule * ( toBone * ( p - origin[bone] ) );
                local.push_back( l );
                meanXY += glm::vec2( l.x, l.y );
                zMin = std::min( zMin, l.z );
                zMax = std::max( zMax, l.z );
            }
            meanXY /= static_cast<float>( local.size() );
            float radius = kMinRadiusCm;
            for ( const glm::vec3& l : local )
                radius = std::max( radius, glm::length( glm::vec2( l.x, l.y ) - meanXY ) );

            PhysicsAssetBody body;
            body.Bone     = bones[bone].Name;
            body.Shape    = PhysicsBodyShape::Capsule;
            body.Rotation = capsule;
            body.Center   = capsule * glm::vec3( meanXY, 0.5f * ( zMin + zMax ) );
            body.Radius   = radius;
            body.Length   = std::max( 0.0f, ( zMax - zMin ) - 2.0f * radius );
            body.MassKg   = 0.0f; // from the volume and density, as an authored asset defaults
            data.Bodies.push_back( std::move( body ) );

            std::optional<uint32_t> ancestor = parentOf( bone );
            while ( ancestor && !hasBody[*ancestor] )
                ancestor = parentOf( *ancestor );
            if ( !ancestor )
                continue;
            PhysicsAssetConstraint joint;
            joint.ParentBone         = bones[*ancestor].Name;
            joint.ChildBone          = bones[bone].Name;
            joint.Position           = glm::vec3( 0.0f );
            joint.Rotation           = TurnFromTo( glm::vec3( 1.0f, 0.0f, 0.0f ), axisLocal );
            joint.Swing1LimitDegrees = settings.DefaultLimitDegrees;
            joint.Swing2LimitDegrees = settings.DefaultLimitDegrees;
            joint.TwistLimitDegrees  = settings.DefaultLimitDegrees;
            data.Constraints.push_back( std::move( joint ) );
        }
        if ( data.Bodies.empty() )
            return Common::MakeFormattedError<PhysicsAssetData>(
                 "no bone spans {} cm of weighted vertices; nothing to make a body of", settings.MinBoneSizeCm );
        return Common::MakeSuccess( std::move( data ) );
    }
} // namespace Desert::Physics
