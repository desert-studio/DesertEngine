#pragma once

#include "../RenderCommand.hpp"
// The renderer itself: every Execute below calls a method on it, and RenderCommand.hpp only
// forward-declares the type (see the note there on the include cycle that cost).
#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Geometry/Mesh.hpp>
#include <Engine/Animation/Pose.hpp>

namespace Desert::Graphic::Render
{
    struct DrawSkinnedMeshCommand : RenderCommand
    {
        Desert::SkinnedMesh* Mesh;
        // CO-OWNED, not a copy of bare pointers: the copy this used to make kept the ARRAY safe and left
        // every MaterialInstance in it owned by an ECS component that Lua can destroy before this command
        // runs. See Graphic::MaterialSlotBinding (A8-3).
        Graphic::MaterialSlotBindingPtr MaterialSlot;
        glm::mat4                       Transform;
        std::vector<glm::mat4>          BoneMatrices;
        bool                            Outlined    = false;
        bool                            CastShadows = true;

        DrawSkinnedMeshCommand( Desert::SkinnedMesh* mesh, Graphic::MaterialSlotBindingPtr materialSlot,
                                const glm::mat4& transform, const std::vector<glm::mat4>& bones,
                                bool outlined = false, bool castShadows = true )
             : Mesh( mesh ), MaterialSlot( std::move( materialSlot ) ), Transform( transform ),
               BoneMatrices( bones ), Outlined( outlined ), CastShadows( castShadows )
        {
        }

        void Execute( SceneRenderer& renderer ) override
        {
            // CastShadows must survive this hop: the flag is consumed three links away (the cascade pass
            // skips !CastShadows on SkinnedMeshRenderData), and a default here would silently re-enable
            // the shadow for every skinned mesh whose component turned it off.
            renderer.SubmitMesh(
                 Mesh, MaterialSlot, Transform,
                 { .BoneMatrices = BoneMatrices, .Outlined = Outlined, .CastShadows = CastShadows } );
        }
    };
} // namespace Desert::Graphic::Render