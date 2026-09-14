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
    // on the batched PBR path — the two paths split the submesh set via masks.
    struct DrawSlotMaterialMeshCommand : RenderCommand
    {
        Desert::Mesh*      Mesh;
        glm::mat4          Transform;
        Graphic::Material* SlotMaterial;
        uint64_t           VisibleSubmeshMask;
        bool               Outlined = false;

        // Set on AT MOST ONE of an entity's slot draws, and only when no PBR draw was emitted for it —
        // the shadow pass draws the mesh whole, so a second caster would be the same silhouette twice.
        bool CastShadows = false;

        DrawSlotMaterialMeshCommand( Desert::Mesh* mesh, const glm::mat4& transform, Graphic::Material* material,
                                     uint64_t visibleSubmeshMask, bool outlined, bool castShadows = false )
             : Mesh( mesh ), Transform( transform ), SlotMaterial( material ),
               VisibleSubmeshMask( visibleSubmeshMask ), Outlined( outlined ), CastShadows( castShadows )
        {
        }

        void Execute( SceneRenderer& renderer ) override
        {
            renderer.SubmitSlotMaterialMesh( Mesh, Transform, SlotMaterial, VisibleSubmeshMask, Outlined,
                                             CastShadows );
        }
    };
} // namespace Desert::Graphic::Render
