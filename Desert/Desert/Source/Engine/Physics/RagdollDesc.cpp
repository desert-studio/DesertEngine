#include <Engine/Physics/RagdollDesc.hpp>

#include <Engine/Animation/Skeleton.hpp>

#include <glm/gtc/constants.hpp>

#include <cmath>
#include <format>
#include <optional>
#include <unordered_map>

namespace Desert::Physics
{
    namespace
    {
        constexpr float kMaxLimitDegrees = 180.0f;

        bool Positive( float v )
        {
            return std::isfinite( v ) && v > 0.0f;
        }

        std::string ConstraintName( const PhysicsAssetConstraint& c )
        {
            return std::format( "constraint '{}' -> '{}'", c.ParentBone, c.ChildBone );
        }

        /// Bone index -> index into asset.Bodies, for bones the skeleton has; duplicates keep the first.
        std::unordered_map<uint32_t, size_t> BodiesByBone( const PhysicsAssetData&    asset,
                                                           const Animation::Skeleton& skeleton )
        {
            std::unordered_map<uint32_t, size_t> byBone;
            for ( size_t i = 0; i < asset.Bodies.size(); ++i )
                if ( const auto bone = skeleton.FindBoneIndex( asset.Bodies[i].Bone ) )
                    byBone.emplace( *bone, i );
            return byBone;
        }

        /// The nearest ancestor of @p bone that carries a body, or nullopt.
        std::optional<uint32_t> ParentBodyBone( uint32_t bone, const Animation::Skeleton& skeleton,
                                                const std::unordered_map<uint32_t, size_t>& byBone )
        {
            const size_t boneCount = skeleton.GetBones().size();
            uint32_t     at        = skeleton.ResolveParent( bone );
            for ( size_t steps = 0; at != Animation::Skeleton::NO_PARENT && steps < boneCount; ++steps )
            {
                if ( byBone.contains( at ) )
                    return at;
                at = skeleton.ResolveParent( at );
            }
            return std::nullopt;
        }

        void CheckLimit( std::vector<std::string>& issues, const PhysicsAssetConstraint& c, const char* which,
                         float degrees )
        {
            if ( !std::isfinite( degrees ) || degrees < 0.0f || degrees > kMaxLimitDegrees )
                issues.push_back( std::format( "{}: {} limit {} degrees is outside [0, 180]", ConstraintName( c ),
                                               which, degrees ) );
        }

        void CheckBodyShape( std::vector<std::string>& issues, const PhysicsAssetBody& b )
        {
            switch ( b.Shape )
            {
                case PhysicsBodyShape::Sphere:
                    if ( !Positive( b.Radius ) )
                        issues.push_back( std::format( "body on bone '{}': sphere radius {} cm is not positive",
                                                       b.Bone, b.Radius ) );
                    break;
                case PhysicsBodyShape::Capsule:
                    if ( !Positive( b.Radius ) )
                        issues.push_back( std::format( "body on bone '{}': capsule radius {} cm is not positive",
                                                       b.Bone, b.Radius ) );
                    if ( !Positive( b.Length ) )
                        issues.push_back( std::format( "body on bone '{}': capsule length {} cm is not positive",
                                                       b.Bone, b.Length ) );
                    break;
                case PhysicsBodyShape::Box:
                    if ( !Positive( b.BoxExtents.x ) || !Positive( b.BoxExtents.y ) ||
                         !Positive( b.BoxExtents.z ) )
                        issues.push_back(
                             std::format( "body on bone '{}': box extents ({}, {}, {}) cm are not all positive",
                                          b.Bone, b.BoxExtents.x, b.BoxExtents.y, b.BoxExtents.z ) );
                    break;
            }
            if ( !std::isfinite( b.MassKg ) || b.MassKg < 0.0f )
                issues.push_back( std::format( "body on bone '{}': mass {} kg is negative", b.Bone, b.MassKg ) );
            else if ( b.MassKg == 0.0f && !Positive( b.DensityGramsPerCm3 ) )
                issues.push_back(
                     std::format( "body on bone '{}': density {} g/cm3 is not positive and no mass overrides it",
                                  b.Bone, b.DensityGramsPerCm3 ) );
        }

        glm::quat RotationOf( const glm::mat4& m )
        {
            // The bind chain may carry scale; the body's orientation is its basis with the scale divided out.
            const glm::mat3 basis( glm::normalize( glm::vec3( m[0] ) ), glm::normalize( glm::vec3( m[1] ) ),
                                   glm::normalize( glm::vec3( m[2] ) ) );
            return glm::normalize( glm::quat_cast( basis ) );
        }
    } // namespace

    float PhysicsBodyMassKg( const PhysicsAssetBody& body )
    {
        if ( body.MassKg > 0.0f )
            return body.MassKg;
        const float pi  = glm::pi<float>();
        const float r   = body.Radius;
        float       cm3 = 0.0f;
        switch ( body.Shape )
        {
            case PhysicsBodyShape::Sphere:
                cm3 = 4.0f / 3.0f * pi * r * r * r;
                break;
            case PhysicsBodyShape::Capsule:
                cm3 = pi * r * r * body.Length + 4.0f / 3.0f * pi * r * r * r;
                break;
            case PhysicsBodyShape::Box:
                cm3 = body.BoxExtents.x * body.BoxExtents.y * body.BoxExtents.z;
                break;
        }
        // grams -> kilograms
        return cm3 * body.DensityGramsPerCm3 / 1000.0f;
    }

    std::vector<std::string> ValidatePhysicsAsset( const PhysicsAssetData&    asset,
                                                   const Animation::Skeleton& skeleton )
    {
        std::vector<std::string> issues;
        if ( asset.Skeleton.IsNull() )
            issues.emplace_back( "the physics asset names no skeleton (null GUID)" );
        if ( asset.Bodies.empty() )
            issues.emplace_back( "the physics asset has no bodies" );

        std::unordered_map<std::string, size_t> seenBones;
        for ( const PhysicsAssetBody& b : asset.Bodies )
        {
            if ( !skeleton.FindBoneIndex( b.Bone ) )
                issues.push_back(
                     std::format( "body names bone '{}', which the skeleton does not have", b.Bone ) );
            if ( ++seenBones[b.Bone] == 2 )
                issues.push_back(
                     std::format( "bone '{}' carries more than one body; a bone carries at most one", b.Bone ) );
            CheckBodyShape( issues, b );
        }

        const auto                              byBone = BodiesByBone( asset, skeleton );
        std::unordered_map<std::string, size_t> seenChildren;
        for ( const PhysicsAssetConstraint& c : asset.Constraints )
        {
            const auto parent = skeleton.FindBoneIndex( c.ParentBone );
            const auto child  = skeleton.FindBoneIndex( c.ChildBone );
            if ( !parent )
                issues.push_back(
                     std::format( "{}: the skeleton has no bone '{}'", ConstraintName( c ), c.ParentBone ) );
            else if ( !byBone.contains( *parent ) )
                issues.push_back(
                     std::format( "{}: bone '{}' carries no body", ConstraintName( c ), c.ParentBone ) );
            if ( !child )
                issues.push_back(
                     std::format( "{}: the skeleton has no bone '{}'", ConstraintName( c ), c.ChildBone ) );
            else if ( !byBone.contains( *child ) )
                issues.push_back(
                     std::format( "{}: bone '{}' carries no body", ConstraintName( c ), c.ChildBone ) );

            if ( parent && child && byBone.contains( *parent ) && byBone.contains( *child ) )
            {
                const auto expected = ParentBodyBone( *child, skeleton, byBone );
                if ( !expected )
                    issues.push_back( std::format( "{}: the bodies are not parent and child in the skeleton ('{}' "
                                                   "has no ancestor with a body)",
                                                   ConstraintName( c ), c.ChildBone ) );
                else if ( *expected != *parent )
                    issues.push_back(
                         std::format( "{}: the bodies are not parent and child in the skeleton (the parent "
                                      "body of '{}' is '{}')",
                                      ConstraintName( c ), c.ChildBone, skeleton.GetBones()[*expected].Name ) );
            }
            if ( ++seenChildren[c.ChildBone] == 2 )
                issues.push_back(
                     std::format( "bone '{}' is the child of more than one constraint; a body has one "
                                  "joint to its parent",
                                  c.ChildBone ) );

            CheckLimit( issues, c, "swing1", c.Swing1LimitDegrees );
            CheckLimit( issues, c, "swing2", c.Swing2LimitDegrees );
            CheckLimit( issues, c, "twist", c.TwistLimitDegrees );
        }
        return issues;
    }

    Common::ResultStr<RagdollDesc> BuildRagdollDesc( const PhysicsAssetData&    asset,
                                                     const Animation::Skeleton& skeleton )
    {
        if ( const auto issues = ValidatePhysicsAsset( asset, skeleton ); !issues.empty() )
        {
            std::string joined;
            for ( const std::string& issue : issues )
                joined += ( joined.empty() ? "" : "; " ) + issue;
            return Common::MakeFormattedError<RagdollDesc>( "the physics asset does not fit the skeleton: {}",
                                                            joined );
        }

        const auto&            bones = skeleton.GetBones();
        std::vector<glm::mat4> bind;
        skeleton.ResolveComponentSpace( [&]( uint32_t i ) { return bones[i].LocalBindTransform; }, bind );

        const auto                                                  byBone = BodiesByBone( asset, skeleton );
        std::unordered_map<uint32_t, const PhysicsAssetConstraint*> constraintByChild;
        for ( const PhysicsAssetConstraint& c : asset.Constraints )
            constraintByChild.emplace( *skeleton.FindBoneIndex( c.ChildBone ), &c );

        // UE's capsule runs along the body's Z, Jolt's along Y: this turn takes Jolt's +Y onto the body's +Z.
        const glm::quat capsuleZFromY = glm::angleAxis( glm::half_pi<float>(), glm::vec3( 1.0f, 0.0f, 0.0f ) );
        constexpr float k             = kJoltUnitsPerCentimetre;

        RagdollDesc                           desc;
        std::unordered_map<uint32_t, int32_t> partByBone;
        for ( const uint32_t bone : skeleton.GetResolveOrder() )
        {
            const auto found = byBone.find( bone );
            if ( found == byBone.end() )
                continue;
            const PhysicsAssetBody& b = asset.Bodies[found->second];

            RagdollPartDesc part;
            part.Bone         = b.Bone;
            part.SkeletonBone = bone;
            if ( const auto parentBone = ParentBodyBone( bone, skeleton, byBone ) )
                part.Parent = partByBone.at( *parentBone );
            part.Shape = b.Shape;
            switch ( b.Shape )
            {
                case PhysicsBodyShape::Sphere:
                    part.Radius        = b.Radius * k;
                    part.ShapeRotation = b.Rotation;
                    break;
                case PhysicsBodyShape::Capsule:
                    part.Radius        = b.Radius * k;
                    part.HalfHeight    = 0.5f * b.Length * k;
                    part.ShapeRotation = glm::normalize( b.Rotation * capsuleZFromY );
                    break;
                case PhysicsBodyShape::Box:
                    part.HalfExtents   = 0.5f * b.BoxExtents * k;
                    part.ShapeRotation = b.Rotation;
                    break;
            }
            part.ShapeOffset = b.Center * k;

            const glm::mat4& boneBind = bind[bone];
            const glm::quat  boneRot  = RotationOf( boneBind );
            part.Position             = glm::vec3( boneBind[3] ) * k;
            part.Rotation             = boneRot;
            part.MassKg               = PhysicsBodyMassKg( b );

            if ( const auto joint = constraintByChild.find( bone ); joint != constraintByChild.end() )
            {
                const PhysicsAssetConstraint& c        = *joint->second;
                const glm::quat               frameRot = glm::normalize( boneRot * c.Rotation );
                const glm::vec3               framePos = glm::vec3( boneBind * glm::vec4( c.Position, 1.0f ) ) * k;
                const glm::vec3               twistAxis = frameRot * glm::vec3( 1.0f, 0.0f, 0.0f );
                const glm::vec3               planeAxis = frameRot * glm::vec3( 0.0f, 1.0f, 0.0f );
                part.HasConstraint                      = true;
                part.ToParent.Position1                 = framePos;
                part.ToParent.Position2                 = framePos;
                part.ToParent.TwistAxis1                = twistAxis;
                part.ToParent.TwistAxis2                = twistAxis;
                part.ToParent.PlaneAxis1                = planeAxis;
                part.ToParent.PlaneAxis2                = planeAxis;
                part.ToParent.NormalHalfConeAngle       = glm::radians( c.Swing1LimitDegrees );
                part.ToParent.PlaneHalfConeAngle        = glm::radians( c.Swing2LimitDegrees );
                part.ToParent.TwistMinAngle             = -glm::radians( c.TwistLimitDegrees );
                part.ToParent.TwistMaxAngle             = glm::radians( c.TwistLimitDegrees );
            }

            partByBone.emplace( bone, static_cast<int32_t>( desc.Parts.size() ) );
            desc.Parts.push_back( std::move( part ) );
        }
        return Common::MakeSuccess( std::move( desc ) );
    }
} // namespace Desert::Physics
