#pragma once

#include "../RenderCommand.hpp"
// The renderer itself: every Execute below calls a method on it, and RenderCommand.hpp only
// forward-declares the type (see the note there on the include cycle that cost).
#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Geometry/Mesh.hpp>
#include <glm/mat4x4.hpp>

namespace Desert::Graphic::Render
{
    struct DrawStaticMeshCommand : RenderCommand
    {
        Desert::Mesh* Mesh;
        // A CO-OWNED handle on the entity's material slots, not a pointer into the component that authored
        // them. A command is recorded by MeshECSSystem, survives every later system — ScriptSystem among
        // them, which runs user Lua that can add or destroy entities — and is only read afterwards. See
        // Graphic::MaterialSlotBinding for the mechanism this replaces (A8-3). Copying it is one atomic
        // increment; the per-entity slot-vector copy the old raw pointer avoided is still avoided.
        Graphic::MaterialSlotBindingPtr                MaterialSlots;
        glm::mat4                                      Transform;
        bool                                           Outlined        = false;
        uint64_t                                       HiddenSubmeshes = 0;  // bit i = submesh i hidden
        int                                            ForcedLOD       = -1; // -1 = auto (by distance)
        int                                            LODBias         = 0;  // shifts the auto LOD (ignored when forced)
        bool                                           CastShadows     = true;
        bool                                           ReceiveShadows  = true;

        DrawStaticMeshCommand( Desert::Mesh* mesh, Graphic::MaterialSlotBindingPtr materialSlots,
                               const glm::mat4& transform, bool outlined = false, uint64_t hiddenSubmeshes = 0,
                               int forcedLOD = -1, int lodBias = 0, bool castShadows = true,
                               bool receiveShadows = true )
             : Mesh( mesh ), MaterialSlots( std::move( materialSlots ) ), Transform( transform ),
               Outlined( outlined ), HiddenSubmeshes( hiddenSubmeshes ), ForcedLOD( forcedLOD ),
               LODBias( lodBias ), CastShadows( castShadows ), ReceiveShadows( receiveShadows )
        {
        }

        void Execute( SceneRenderer& renderer ) override
        {
            if ( MaterialSlots )
                renderer.SubmitMesh( Mesh, MaterialSlots, Transform,
                                     { .Outlined        = Outlined,
                                       .HiddenSubmeshes = HiddenSubmeshes,
                                       .ForcedLOD       = ForcedLOD,
                                       .LODBias         = LODBias,
                                       .CastShadows     = CastShadows,
                                       .ReceiveShadows  = ReceiveShadows } );
        }
    };

    // UE-style Instanced Static Mesh: one mesh + one material drawn for every transform. The material
    // instance and the transform array are both CO-OWNED — both are owned by the ECS component that
    // recorded this command, and the entity can be destroyed by Lua before the command is executed (A8-3).
    struct DrawInstancedStaticMeshCommand : RenderCommand
    {
        Desert::Mesh*                                 Mesh;
        Graphic::MaterialInstancePtr                  Material;
        std::shared_ptr<const std::vector<glm::mat4>> Transforms;
        bool                                          CastShadows;

        DrawInstancedStaticMeshCommand( Desert::Mesh* mesh, Graphic::MaterialInstancePtr material,
                                        std::shared_ptr<const std::vector<glm::mat4>> transforms,
                                        bool                                          castShadows )
             : Mesh( mesh ), Material( std::move( material ) ), Transforms( std::move( transforms ) ),
               CastShadows( castShadows )
        {
        }

        void Execute( SceneRenderer& renderer ) override
        {
            if ( Mesh && Material && Transforms )
                renderer.SubmitInstancedMesh( Mesh, Material, Transforms, CastShadows );
        }
    };
} // namespace Desert::Graphic::Render