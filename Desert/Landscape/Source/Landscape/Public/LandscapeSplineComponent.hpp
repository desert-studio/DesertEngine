#pragma once

#include <entt/entt.hpp>

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

#include <Engine/World/Landscape/LandscapeSplines.hpp>

namespace Desert::ECS
{
    // ONE LANDSCAPE SPLINE (UE 5.5: ALandscapeSplineActor + its ULandscapeSplinesComponent - in a World Partition
    // world every spline is its own spatially loaded actor, ULandscapeInfo::MoveSplinesToLevel /
    // CreateSplineActor). The control points and segments are WORLD cm and they write the root's Splines edit
    // layer (RasterizeLandscapeSplines over every loaded spline of the root, combined); the bent meshes are
    // derived from them (LandscapeSplineMeshDraw, and the cell HLOD's LandscapeSpline builder).
    //
    // `Landscape` names the root BY ID, as LandscapeTileComponent does and for the same reason: as an
    // OBSERVATION (WorldPartitionRules.hpp, kEntityReferences) the spline is its own composite, placed by the box
    // around its band (EntityDescriptor::LandscapeSpline) - so a long road goes up to the grid level whose cell
    // holds it - and never drags the root, or the root it. Nothing reads this entity's transform.
    struct LandscapeSplineComponent
    {
        Common::UUID                       Landscape = Common::UUID::Null(); // the root entity
        World::Landscape::LandscapeSplines Splines;
    };
} // namespace Desert::ECS
