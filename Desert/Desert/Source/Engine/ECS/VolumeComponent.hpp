#pragma once

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

#include <CoreReflection/ReflectionMacros.hpp>

namespace Desert::ECS
{
    // UE's AVolume classes a Modeling tool can make (UMeshToVolumeTool::NewVolumeType): what the volume IS for.
    // The kind picks the collision profile its body is created with (VolumeCollisionProfile below), exactly as
    // ATriggerVolume / ABlockingVolume set their brush component's preset in their constructors.
    enum class VolumeType
    {
        Trigger,  // ATriggerVolume: overlaps everything, blocks nothing; reports begin/end overlap
        Blocking, // ABlockingVolume: an invisible wall
    };

    // The project collision profile (Config/CollisionProfiles.json) a volume of @p type is simulated with.
    constexpr const char* VolumeCollisionProfile( VolumeType type )
    {
        return type == VolumeType::Trigger ? "Trigger" : "BlockAll";
    }

    // UE's AVolume + UBrushComponent: a CONVEX region that draws only as an outline. The shape is held by the
    // volume itself (UE keeps the brush's geometry in the actor, not in a mesh asset): the points the hull is
    // cooked from and the triangles the editor outlines, body-local, centimetres, the entity's scale on top.
    // The body is a Jolt ConvexHull built from these points (PhysicsECSSystem::GatherColliderMesh) on the
    // entity's RigidBody + Collider; a Trigger's profile answers Overlap, so its contacts are Jolt sensor
    // contacts and the bodies pass through, reported as BeginOverlap / EndOverlap (PhysicsEvents.hpp).
    struct VolumeData
    {
        REFLECT()

        PROPERTY( DisplayName( "Volume Type" ), Category( "Volume" ), Summary,
                  Tooltip( "What the volume is for (UE: the AVolume class). Trigger reports overlaps and blocks "
                           "nothing; Blocking is an invisible wall." ) )
        VolumeType Type = VolumeType::Trigger;

        // x, y, z per corner, flat: the reflection serializes a vector of scalars (a std::vector<glm::vec3> field
        // is not a container to it and would be silently dropped by save, undo and redo). Read through
        // VolumeCornerCount / VolumeCorner below.
        PROPERTY( DisplayName( "Hull Points" ), Category( "Volume" ), Advanced, Length,
                  Tooltip( "Body-local corners of the convex hull (cm), x y z per corner." ) )
        std::vector<float> HullPoints;

        PROPERTY( DisplayName( "Hull Triangles" ), Category( "Volume" ), Advanced,
                  Tooltip( "Three Hull Points indices per triangle: the outline the editor draws." ) )
        std::vector<uint32_t> HullIndices;
    };

    inline size_t VolumeCornerCount( const VolumeData& volume ) { return volume.HullPoints.size() / 3; }

    inline glm::vec3 VolumeCorner( const VolumeData& volume, size_t corner )
    {
        return { volume.HullPoints[corner * 3], volume.HullPoints[corner * 3 + 1], volume.HullPoints[corner * 3 + 2] };
    }

    inline void SetVolumeCorners( VolumeData& volume, const std::vector<glm::vec3>& corners )
    {
        volume.HullPoints.clear();
        volume.HullPoints.reserve( corners.size() * 3 );
        for ( const glm::vec3& c : corners )
            volume.HullPoints.insert( volume.HullPoints.end(), { c.x, c.y, c.z } );
    }

    struct VolumeComponent
    {
        COMPONENT( Key( "Volume" ), Block( Data ), Run( ActorsAndUI ) )
        VolumeData Data;
    };
} // namespace Desert::ECS
