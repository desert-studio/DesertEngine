#pragma once

#include <Engine/Graphic/InstanceCullDistance.hpp>
#include <Engine/Graphic/InstanceWind.hpp>
#include <Engine/Graphic/Materials/MaterialInstance.hpp>

#include <glm/mat4x4.hpp>

#include <cstdint>
#include <memory>
#include <vector>

namespace Desert
{
    class Mesh;
}

namespace Desert::Graphic::Render
{
    // ONE SECTION GROUP OF AN INSTANCED STATIC MESH, AS THE RETAINED ISM LIST HOLDS IT (UE: an ISM proxy's
    // cached mesh draw command, built when the component, its instances or its material change, never per
    // frame). The material instances and the transform snapshot are CO-OWNED (A8-3): the entity can be
    // destroyed by Lua between recording and replay. A view's queue entry is refilled from the row
    // (MeshRenderer::SubmitInstancedRow) without copying a handle that already names the same object.
    struct InstancedSceneRow
    {
        uint32_t                                      Entity = 0; // keys each instance's GPU-scene row
        Desert::Mesh*                                 Mesh   = nullptr;
        MaterialInstancePtr                           Material;            // the section group's slot instance
        MaterialInstancePtr                           DisplacedMaterial;   // its displaced sibling, or null
        uint64_t                                      HiddenSubmeshes = 0; // the sections of the other slots
        std::shared_ptr<const std::vector<glm::mat4>> Transforms;          // the component's instance snapshot
        bool                                          CastShadows       = true;
        bool                                          CastContactShadow = true; // UE bCastContactShadow
    };

    // THE RETAINED ISM LIST, one immutable version of it (MeshECSSystem publishes a new one only when a row
    // changed: an instance revision, a material instance, a mesh, a flag). A still frame hands the SAME
    // snapshot again: one shared_ptr copy for every ISM of the scene. A replaced version is parked through
    // ECS::RetireRenderCache, so a view still replaying it never sees its rows freed.
    struct InstancedSceneSnapshot
    {
        std::vector<InstancedSceneRow> Rows;
        uint64_t                       Revision = 0; // bumped per published change
        // The Mesh::GetStaleEpoch its rows' meshes were last prepared at (DrawInstancedSceneCommand::Prepare),
        // as StaticSceneSnapshot's: game thread only, reset by the publisher for every new version.
        mutable uint64_t PreparedStaleEpoch = ~0ull;
    };

    // What changes per frame for a row and so is not part of the retained list: the foliage type's fade
    // (FO-5, read while the type loads) clipped to the component's max draw distance, and its wind (FO-7)
    // at this frame's gameplay time.
    struct InstancedSceneRowFrame
    {
        InstanceCullDistance CullDistance;
        InstanceWind         Wind;
    };

    // One frame's InstancedSceneRowFrame per row of the snapshot recorded with it (index for index). Plain
    // values, pooled by the producer (a spare no command holds any more is refilled), so a steady frame
    // allocates nothing.
    struct InstancedSceneFrame
    {
        std::vector<InstancedSceneRowFrame> Rows;
    };
} // namespace Desert::Graphic::Render
