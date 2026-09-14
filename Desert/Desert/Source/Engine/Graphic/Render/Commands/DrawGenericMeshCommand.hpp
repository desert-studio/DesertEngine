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
    // A static mesh drawn with a generic data-driven material (a MaterialComponent assigning a non-PBR
    // shader). Per-object path — does NOT go through the batched PBR SSBO.
    struct DrawGenericMeshCommand : RenderCommand
    {
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

        DrawGenericMeshCommand( Desert::Mesh* mesh, const glm::mat4& transform, std::string shaderName,
                                Graphic::MaterialOverrides overrides, bool outlined,
                                Graphic::Image2D* directTexture = nullptr, std::string directTextureSampler = {},
                                bool castShadows = false )
             : Mesh( mesh ), Transform( transform ), ShaderName( std::move( shaderName ) ),
               Overrides( std::move( overrides ) ), Outlined( outlined ), DirectTexture( directTexture ),
               DirectTextureSampler( std::move( directTextureSampler ) ), CastShadows( castShadows )
        {
        }

        void Execute( SceneRenderer& renderer ) override
        {
            renderer.SubmitGenericMesh( Mesh, Transform, ShaderName, Overrides, Outlined, DirectTexture,
                                        DirectTextureSampler, CastShadows );
        }
    };
} // namespace Desert::Graphic::Render
