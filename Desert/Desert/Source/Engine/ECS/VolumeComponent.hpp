#pragma once

#include <glm/glm.hpp>

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

        PROPERTY( DisplayName( "Hull Points" ), Category( "Volume" ), Advanced, Length,
                  Tooltip( "Body-local points (cm) whose convex hull is the volume." ) )
        std::vector<glm::vec3> HullPoints;

        PROPERTY( DisplayName( "Hull Triangles" ), Category( "Volume" ), Advanced,
                  Tooltip( "Three Hull Points indices per triangle: the outline the editor draws." ) )
        std::vector<uint32_t> HullIndices;
    };

    struct VolumeComponent
    {
        COMPONENT( Key( "Volume" ), Block( Data ), Run( ActorsAndUI ) )
        VolumeData Data;
    };
} // namespace Desert::ECS
