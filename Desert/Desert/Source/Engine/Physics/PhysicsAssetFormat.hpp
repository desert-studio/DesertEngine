#pragma once

// THE `.dephysasset` FILE — UE's UPhysicsAsset: the rigid bodies and joints a skeleton falls apart into when it
// becomes a ragdoll. One body per bone (UE USkeletalBodySetup with one FKAggregateGeom element: a sphere, a box
// or a capsule) and one swing-twist joint between a body and its parent body (UE UPhysicsConstraintTemplate with
// Swing1 / Swing2 cone limits and a Twist limit).
//
// The file is the asset envelope (Common/Content/AssetEnvelope.hpp, Kind PhysicsAsset, one subsystem version
// under 'DPHA'), the way a fracture sits in its envelope (Destruction/FractureFormat.hpp). The envelope carries
// the identity (GUID); the payload carries everything else, little-endian, no padding:
//
//   skeleton GUID (Hi, Lo u64), body count, per body: bone name (u32 length + bytes), shape (u8), centre (3 f32),
//   rotation (x, y, z, w f32), radius, length, box extents (3 f32), mass kg, density g/cm3; constraint count, per
//   constraint: parent bone name, child bone name, position (3 f32), rotation (x, y, z, w f32), swing1, swing2,
//   twist limits (f32 degrees).
//
// Every length is in CENTIMETRES (the engine's unit) and every angle in DEGREES (what an author types, as in
// UE's Details panel); the conversion to what Jolt reads is Physics/RagdollDesc.hpp's and is made there once.
// A reader of another version, a truncated payload, an unknown shape or trailing bytes is refused by name.
//
// This header is the DATA and its bytes only. Whether the data fits a skeleton — bone names, parent/child
// joints, positive extents — is Physics/RagdollDesc.hpp's validation, because it needs the skeleton.

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Core/ResultStr.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

namespace Desert::Physics
{
    inline constexpr uint32_t    kPhysicsAssetSubsystemTag  = Common::Content::FourCC( "DPHA" );
    inline constexpr uint32_t    kPhysicsAssetFormatVersion = 1;
    inline constexpr const char* kPhysicsAssetExtension     = ".dephysasset";

    /// UE FKAggregateGeom's element kinds a ragdoll body is made of. The numbers are the file's.
    enum class PhysicsBodyShape : uint8_t
    {
        Sphere  = 0, ///< UE FKSphereElem: Radius.
        Box     = 1, ///< UE FKBoxElem: BoxExtents (full sizes along the body's X, Y, Z).
        Capsule = 2, ///< UE FKSphylElem: Radius and Length (between the cap centres), along the body's local Z.
    };

    [[nodiscard]] const char* PhysicsBodyShapeName( PhysicsBodyShape shape );

    /// UE USkeletalBodySetup with one element: the body that follows @c Bone.
    struct PhysicsAssetBody
    {
        std::string      Bone;
        PhysicsBodyShape Shape = PhysicsBodyShape::Capsule;
        // The element's placement in the BONE's space (UE FKSphylElem::Center / Rotation).
        glm::vec3 Center   = glm::vec3( 0.0f ); ///< cm
        glm::quat Rotation = glm::quat( 1.0f, 0.0f, 0.0f, 0.0f );
        float     Radius   = 5.0f; ///< cm; Sphere and Capsule
        float Length = 20.0f; ///< cm; Capsule: the cylinder between the two cap centres (UE FKSphylElem::Length)
        glm::vec3 BoxExtents = glm::vec3( 10.0f ); ///< cm; Box: full sizes (UE FKBoxElem X, Y, Z)
        // UE FBodyInstance: MassInKgOverride when positive; otherwise the mass is the shape's volume times
        // DensityGramsPerCm3 (UE UPhysicalMaterial::Density, in the same unit).
        float MassKg             = 0.0f;
        float DensityGramsPerCm3 = 1.0f;

        bool operator==( const PhysicsAssetBody& ) const = default;
    };

    /// UE UPhysicsConstraintTemplate (FConstraintInstance) between a body and its parent body. The joint frame is
    /// placed in the CHILD bone's space (UE's default: the frame sits at the child bone); its +X is the twist
    /// axis, Swing1 turns about its +Z and Swing2 about its +Y — UE's ConeLimit / TwistLimit convention.
    struct PhysicsAssetConstraint
    {
        std::string ParentBone;
        std::string ChildBone;
        glm::vec3   Position           = glm::vec3( 0.0f ); ///< cm, child bone space
        glm::quat   Rotation           = glm::quat( 1.0f, 0.0f, 0.0f, 0.0f );
        float       Swing1LimitDegrees = 45.0f; ///< half-angle of the cone about the frame's +Z, [0, 180]
        float       Swing2LimitDegrees = 45.0f; ///< half-angle of the cone about the frame's +Y, [0, 180]
        float       TwistLimitDegrees  = 45.0f; ///< symmetric twist about the frame's +X, [0, 180]

        bool operator==( const PhysicsAssetConstraint& ) const = default;
    };

    struct PhysicsAssetData
    {
        Common::Content::AssetGuid          Guid;     ///< the envelope's identity
        Common::Content::AssetGuid          Skeleton; ///< the `.skeleton` whose bones the bodies name
        std::vector<PhysicsAssetBody>       Bodies;
        std::vector<PhysicsAssetConstraint> Constraints;

        bool operator==( const PhysicsAssetData& ) const = default;
    };

    /// The payload bytes alone (no envelope, no GUID).
    [[nodiscard]] std::vector<unsigned char> EncodePhysicsAssetPayload( const PhysicsAssetData& data );
    [[nodiscard]] Common::ResultStr<PhysicsAssetData>
    DecodePhysicsAssetPayload( const std::vector<unsigned char>& bytes );

    /// The whole `.dephysasset` file. Refuses a null GUID: the file IS the asset's identity.
    [[nodiscard]] Common::ResultStr<std::vector<unsigned char>> EncodePhysicsAsset( const PhysicsAssetData& data );
    [[nodiscard]] Common::ResultStr<PhysicsAssetData> DecodePhysicsAsset( const std::vector<unsigned char>& file );

    /// The GUID from the envelope header alone.
    [[nodiscard]] Common::ResultStr<Common::Content::AssetGuid> ReadPhysicsAssetGuid( std::istream& in );
} // namespace Desert::Physics
