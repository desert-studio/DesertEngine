#pragma once

#include <Common/Core/Entt.hpp>

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/UUID.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

#include <CoreReflection/ReflectionMacros.hpp>

#include <Engine/Core/Serialize/StreamingSourceTargetBehavior.hpp>

namespace Desert::ECS
{
    // UE's UWorldPartitionStreamingSourceComponent: what makes an entity a point a partitioned world loads
    // around in Play and in the game. Residency follows the union of every enabled source (Core::WorldStreamer).
    // The player streams from where it LOOKS without one (Core::PlayerStreamingViewOf, PlayerStreamingView.hpp -
    // UE's player controller at GetPlayerViewPoint); a pawn is a source only when its prefab authors this
    // component, as UE's character carries its own (Pawn_Player.deprefab: the ground under the body while the view
    // is away). No other camera is a source by itself. Its entity loads Global (WorldPartitionRules), so a source
    // never streams itself out.
    struct StreamingSourceData
    {
        REFLECT()

        PROPERTY( DisplayName( "Enabled" ), Category( "Streaming Source" ),
                  Tooltip( "Off = the world does not load around this entity." ) )
        bool Enabled = true;

        PROPERTY( DisplayName( "Override Loading Range" ), Category( "Streaming Source" ),
                  Tooltip( "Off = the World Partition grid's Loading Range." ) )
        bool OverrideLoadingRange = false;

        PROPERTY( DisplayName( "Loading Range" ), Category( "Streaming Source" ), Units( "cm" ),
                  Range( 0.0f, 1000000.0f ), EditCondition( "OverrideLoadingRange" ),
                  Tooltip( "Cells within this distance of the entity load (cm)." ) )
        float LoadingRange = 0.0f;

        PROPERTY( DisplayName( "Priority" ), Category( "Streaming Source" ),
                  Tooltip( "Higher loads first when several sources want cells; it never changes which cells "
                           "load." ) )
        int Priority = 0;

        // The shape the world loads within (UE's FStreamingSourceShape): a circle of the range, or a sector of
        // it, placed in the entity's frame (Rules::StreamingSourceShape).
        PROPERTY( DisplayName( "Sector" ), Category( "Streaming Source" ),
                  Tooltip( "On = only a slice of the circle, Sector Angle wide around the entity's forward "
                           "(turned by Shape Yaw), loads." ) )
        bool IsSector = false;

        PROPERTY( DisplayName( "Sector Angle" ), Category( "Streaming Source" ), Units( "deg" ),
                  Range( 0.0f, 360.0f ), EditCondition( "IsSector" ),
                  Tooltip( "How wide the slice is; 360 is the whole circle." ) )
        float SectorAngle = 360.0f;

        // A VIEW AS A SOURCE: a camera that streams what it looks at (a narrow lens reaching far) states the
        // range and priority; the angle follows the lens as gameplay changes the camera's Field of View.
        PROPERTY( DisplayName( "Angle From Camera" ), Category( "Streaming Source" ), EditCondition( "IsSector" ),
                  Tooltip( "On = the slice is as wide as this entity's Camera sees across: its Field of View "
                           "widened by the viewport's width over height, following every change of it. Sector "
                           "Angle is not used. The entity must have a Camera." ) )
        bool SectorAngleFromCamera = false;

        PROPERTY( DisplayName( "Shape Offset" ), Category( "Streaming Source" ), Length,
                  Tooltip( "Where the shape's centre sits from the entity, in its own axes (cm): X right, Z "
                           "backward. Height is ignored." ) )
        glm::vec3 ShapeOffset = glm::vec3( 0.0f );

        PROPERTY( DisplayName( "Shape Yaw" ), Category( "Streaming Source" ), Units( "deg" ),
                  Range( -180.0f, 180.0f ),
                  Tooltip( "How far the shape's axis is turned from the entity's forward, about its up axis." ) )
        float ShapeYaw = 0.0f;

        PROPERTY( DisplayName( "Use Velocity For Cell Sorting" ), Category( "Streaming Source" ),
                  Tooltip( "On = cells the entity is moving toward load first (UE's "
                           "bUseVelocityContributionToCellsSorting); it never changes which cells load." ) )
        bool UseVelocityForSorting = false;

        // UE's FWorldPartitionStreamingSource::TargetGrids: a camera streaming only the near grid's small props,
        // a cinematic only the main grid. A name the world has no grid of streams nothing and is an Issue of the
        // world's plan (Rules::PlanWorldPartition, Entities[i].StreamingSource.TargetGrids). Details draws it as
        // a pick of the world's grid names (ComponentEditorRegistrations.cpp, MakeStreamingSourceEntry).
        PROPERTY( DisplayName( "Target Grids" ), Category( "Streaming Source" ),
                  Tooltip( "The World Partition grids this source streams, by name; empty = every grid." ),
                  Hidden )
        std::vector<std::string> TargetGrids;

        // UE's FWorldPartitionStreamingSource::TargetHLODLayers: the HLOD layers this source streams, by
        // HLODLayerSerialized::Name, read under Target Behavior like Target Grids (Rules::TargetsHLODLayer). An
        // HLOD layer standing for a world grid the source streams is streamed whatever this says (Rules::
        // HLODSources). A name the world has no layer of is an Issue of the world's plan (Entities[i].
        // StreamingSource.TargetHLODLayers). Details draws it as a pick of the world's HLOD layer names
        // (ComponentEditorRegistrations.cpp, MakeStreamingSourceEntry).
        PROPERTY( DisplayName( "Target HLOD Layers" ), Category( "Streaming Source" ),
                  Tooltip( "The HLOD layers this source streams, by name, read with Target Behavior." ), Hidden )
        std::vector<std::string> TargetHLODLayers;

        // UE's FWorldPartitionStreamingSource::TargetBehavior (Rules::TargetsGridNamed).
        PROPERTY( DisplayName( "Target Behavior" ), Category( "Streaming Source" ),
                  Tooltip( "Include = only the Target Grids and Target HLOD Layers stream; Exclude = every grid "
                           "but those. With both lists empty every grid streams either way." ) )
        Core::Rules::StreamingSourceTargetBehavior TargetBehavior =
             Core::Rules::StreamingSourceTargetBehavior::Include;

        // THE LOOK-AHEAD (Rules::StreamingSource): a plane or a car streams the cells it is about to reach, not
        // only those around it. UE has no such setting; there a game adds a predicted source by hand.
        PROPERTY( DisplayName( "Look Ahead Time" ), Category( "Streaming Source" ), Units( "s" ),
                  Range( 0.0f, 120.0f ),
                  Tooltip( "Cells along the entity's path load ahead of time: the loading shape reaches as far "
                           "as the entity travels in this many seconds at its present speed (on the ground). "
                           "0 = only around where it is. About how long a cell takes to load." ) )
        float LookAheadTime = 0.0f;

        PROPERTY( DisplayName( "Max Look Ahead" ), Category( "Streaming Source" ), Length,
                  Range( 0.0f, 10000000.0f ),
                  Tooltip( "The farthest ahead the loading shape reaches however fast the entity moves (cm), "
                           "so a teleport or a burst of speed does not request half the world." ) )
        float MaxLookAhead = 100000.0f;
    };

    struct StreamingSourceComponent
    {
        COMPONENT( Key( "StreamingSource" ), Block( Data ), Run( ActorsAndUI ) )
        StreamingSourceData Data;
    };
} // namespace Desert::ECS
