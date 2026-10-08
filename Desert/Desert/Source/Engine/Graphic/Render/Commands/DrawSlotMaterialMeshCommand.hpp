#pragma once

#include "../RenderCommand.hpp"
// The renderer itself: every Execute below calls a method on it, and RenderCommand.hpp only
// forward-declares the type (see the note there on the include cycle that cost).
#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Geometry/Mesh.hpp>

#include <glm/mat4x4.hpp>

namespace Desert::Graphic
{
    class Material;
}

namespace Desert::Graphic::Render
{
    // v3 per-slot custom shaders: draws the submeshes of ONE material slot with the slot's own
    // runtime material (a MaterialService-owned DataDrivenMaterial). The rest of the mesh stays
    // on the batched lit path — the two paths split the submesh set via masks.
    struct DrawSlotMaterialMeshCommand : RenderCommand
    {
        // The entity that owns the draw (entt id incl. version): the view's MotionHistory keys this draw's
        // previous transform by it (SceneViewState MotionKey), so the velocity of a moving object is its own.
        uint32_t Entity;
        // A stable part of the entity (MotionRecord::Part; a fracture piece's node + 1), 0 = the entity itself:
        // the part the same piece's DrawStaticMeshCommand carries, so the piece's velocity is its own on both
        // paths.
        uint32_t           MotionPart = 0;
        Desert::Mesh*      Mesh;
        glm::mat4          Transform;
        Graphic::Material* SlotMaterial;
        uint64_t           VisibleSubmeshMask;
        bool               Outlined = false;

        // Set on AT MOST ONE of an entity's slot draws, and only when no lit draw was emitted for it —
        // the shadow pass draws the mesh whole, so a second caster would be the same silhouette twice.
        bool CastShadows = false;

        DrawSlotMaterialMeshCommand( uint32_t entity, Desert::Mesh* mesh, const glm::mat4& transform,
                                     Graphic::Material* material, uint64_t visibleSubmeshMask, bool outlined,
                                     bool castShadows = false, uint32_t motionPart = 0 )
             : Entity( entity ), MotionPart( motionPart ), Mesh( mesh ), Transform( transform ),
               SlotMaterial( material ), VisibleSubmeshMask( visibleSubmeshMask ), Outlined( outlined ),
               CastShadows( castShadows )
        {
        }

        void Execute( SceneRenderer& renderer ) override
        {
            renderer.SubmitSlotMaterialMesh( Entity, Mesh, Transform, SlotMaterial, VisibleSubmeshMask, Outlined,
                                             CastShadows, MotionPart );
        }
    };
} // namespace Desert::Graphic::Render
