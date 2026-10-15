#pragma once

#include "../RenderCommand.hpp"
// The renderer itself: every Execute below calls a method on it, and RenderCommand.hpp only
// forward-declares the type (see the note there on the include cycle that cost).
#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Geometry/Mesh.hpp>
#include <Engine/Graphic/Materials/MaterialOverrides.hpp>

#include <glm/mat4x4.hpp>

#include <string>
#include <utility>

namespace Desert::Graphic::Render
{
    // A static mesh drawn with a generic data-driven material (a MaterialComponent assigning a non-lit
    // shader). Per-object path — does NOT go through the batched lit SSBO.
    struct DrawGenericMeshCommand : RenderCommand
    {
        // The entity that owns the draw (entt id incl. version): the view's MotionHistory keys this draw's
        // previous transform by it (SceneViewState MotionKey), so the velocity of a moving object is its own.
        uint32_t                   Entity;
        Desert::Mesh*            Mesh;
        glm::mat4                Transform;
        std::string              ShaderName;
        Graphic::MaterialOverrides Overrides;
        bool                     Outlined = false;

        // Optional runtime-owned texture (no asset handle) bound to DirectTextureSampler — the text
        // system feeds its SDF atlas here. Non-owning: the producer keeps it alive for the frame.
        Graphic::Image2D* DirectTexture = nullptr;
        std::string       DirectTextureSampler;

        // Off by default: the text system draws its SDF glyph quads through this command too, and the
        // shadow pass has no alpha test. Mesh producers opt in.
        bool CastShadows = false;

        // The owning component's MaxDrawDistance (cm, 0 = no limit), asked of the executing view's lens.
        float MaxDrawDistance = 0.0f;

        // The owning StaticMeshComponent's Cast Contact Shadow (on for a producer that is not a mesh component).
        bool CastContactShadow = true;

        DrawGenericMeshCommand( uint32_t entity, Desert::Mesh* mesh, const glm::mat4& transform,
                                std::string shaderName, Graphic::MaterialOverrides overrides, bool outlined,
                                Graphic::Image2D* directTexture = nullptr, std::string directTextureSampler = {},
                                bool castShadows = false, float maxDrawDistance = 0.0f,
                                bool castContactShadow = true )
             : Entity( entity ), Mesh( mesh ), Transform( transform ), ShaderName( std::move( shaderName ) ),
               Overrides( std::move( overrides ) ), Outlined( outlined ), DirectTexture( directTexture ),
               DirectTextureSampler( std::move( directTextureSampler ) ), CastShadows( castShadows ),
               MaxDrawDistance( maxDrawDistance ), CastContactShadow( castContactShadow )
        {
        }

        void Execute( SceneRenderer& renderer ) override
        {
            if ( !renderer.IsWithinMaxDrawDistance( Mesh, Transform, MaxDrawDistance ) )
                return;
            renderer.SubmitGenericMesh( Entity, Mesh, Transform, ShaderName, Overrides, Outlined, DirectTexture,
                                        DirectTextureSampler, CastShadows, CastContactShadow );
        }
    };
} // namespace Desert::Graphic::Render
