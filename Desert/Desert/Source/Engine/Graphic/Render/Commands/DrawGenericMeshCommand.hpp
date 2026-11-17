#pragma once

#include "../RenderCommand.hpp"
// The renderer itself: every Execute below calls a method on it, and RenderCommand.hpp only
// forward-declares the type (see the note there on the include cycle that cost).
#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Geometry/Mesh.hpp>
#include <Engine/Graphic/Materials/MaterialOverrides.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingProgramHit.hpp>

#include <glm/mat4x4.hpp>

#include <atomic>
#include <cstdint>
#include <string>
#include <utility>
#include <Engine/Graphic/GpuDriven/GpuSceneRegistry.hpp>

namespace Desert::Graphic::Render
{
    // The next GenericMeshDrawParams::Revision: process-unique, never 0, never reused (a params object built at a
    // freed one's address is another revision).
    [[nodiscard]] inline uint64_t NextGenericMeshDrawParamsRevision() noexcept
    {
        static std::atomic<uint64_t> next{ 1u };
        return next.fetch_add( 1u, std::memory_order_relaxed );
    }

    // A DRAW'S PER-OBJECT VALUES KEPT BY ITS PRODUCER (UE: a proxy's material relevance built on change, not per
    // frame): the material overrides and the program hit a producer builds when its colour/emission/atlas moves
    // and hands every frame by pointer, so a steady frame copies no vector and no string (TextECSSystem, and
    // MeshECSSystem's Shader Override meshes, built per MaterialComponent revision).
    struct GenericMeshDrawParams
    {
        Graphic::MaterialOverrides                Overrides;
        Graphic::RayTracing::RayTracingProgramHit RayHit;
        // Non-empty: the draw's shader, read instead of the command's ShaderName (a MaterialComponent shader name
        // can pass the small-string buffer, so a per-frame copy into the command would allocate). Empty: the
        // command's ShaderName (text passes a short literal).
        std::string ShaderName;
        // THE VALUES' REVISION (UE: a proxy's material relevance is rebuilt on a material change event, not
        // re-hashed per frame): stamped once when the producer builds the object. A producer never edits a
        // published object, it builds a new one (and retires the old: ECS::RetireRenderCache), so equal
        // revisions are equal Overrides, RayHit and ShaderName. The renderer keys the record's content and its
        // texture-set key by it (MeshRenderer::GenericMeshRenderData::ValuesRevision) instead of hashing the
        // strings and every override per record per frame. Const: a copy keeps the values it was stamped for.
        const uint64_t Revision = NextGenericMeshDrawParamsRevision();
    };

    // A static mesh drawn with a generic data-driven material (a MaterialComponent assigning a non-lit
    // shader). Per-object path — does NOT go through the batched lit SSBO.
    struct DrawGenericMeshCommand : RenderCommand
    {
        // The entity that owns the draw (entt id incl. version): GpuSceneRegistry keys this draw's
        // GpuScene primitive by it (MotionKey), so the velocity of a moving object is its own.
        uint32_t                   Entity;
        Desert::Mesh*              Mesh;
        glm::mat4                  Transform;
        std::string                ShaderName;
        Graphic::MaterialOverrides Overrides;
        bool                       Outlined = false;

        // Optional runtime-owned texture (no asset handle) bound to DirectTextureSampler — the text
        // system feeds its SDF atlas here. Non-owning: the producer keeps it alive for the frame.
        Graphic::Image2D* DirectTexture = nullptr;
        std::string       DirectTextureSampler;

        // Off by default: the text system draws its SDF glyph quads through this command too, and the
        // shadow pass has no alpha test. Mesh producers opt in.
        bool CastShadows = false;

        // The owning component's MaxDrawDistance (cm, 0 = no limit), asked of the executing view's lens.
        float MaxDrawDistance = 0.0f;

        // Off by default for the reason CastShadows is (UE bVisibleInRayTracing): mesh producers opt in.
        bool VisibleInRayTracing = false;
        // The draw's own hit group (RayTracingProgramHit.hpp), for a program that is no surface template: the text
        // system's glyph quads (TextRayTracing.hpp TextRayHit). Empty: the default surface shades a hit.
        Graphic::RayTracing::RayTracingProgramHit RayHit;

        // Set: Overrides and RayHit above stay empty and the draw reads this producer-kept pair instead. The
        // producer keeps it in a shared_ptr it retires on change (ECS::RetireRenderCache), as it does the mesh.
        const GenericMeshDrawParams* Params = nullptr;

        DrawGenericMeshCommand( uint32_t entity, Desert::Mesh* mesh, const glm::mat4& transform,
                                std::string shaderName, Graphic::MaterialOverrides overrides, bool outlined,
                                Graphic::Image2D* directTexture = nullptr, std::string directTextureSampler = {},
                                bool castShadows = false, float maxDrawDistance = 0.0f,
                                bool                                      visibleInRayTracing = false,
                                Graphic::RayTracing::RayTracingProgramHit rayHit              = {} )
             : Entity( entity ), Mesh( mesh ), Transform( transform ), ShaderName( std::move( shaderName ) ),
               Overrides( std::move( overrides ) ), Outlined( outlined ), DirectTexture( directTexture ),
               DirectTextureSampler( std::move( directTextureSampler ) ), CastShadows( castShadows ),
               MaxDrawDistance( maxDrawDistance ), VisibleInRayTracing( visibleInRayTracing ),
               RayHit( std::move( rayHit ) )
        {
        }

        // The producer-kept form: @p params outlives the frames in flight (see Params).
        DrawGenericMeshCommand( uint32_t entity, Desert::Mesh* mesh, const glm::mat4& transform,
                                std::string shaderName, const GenericMeshDrawParams& params, bool outlined,
                                Graphic::Image2D* directTexture, std::string directTextureSampler,
                                bool castShadows, float maxDrawDistance, bool visibleInRayTracing )
             : Entity( entity ), Mesh( mesh ), Transform( transform ), ShaderName( std::move( shaderName ) ),
               Outlined( outlined ), DirectTexture( directTexture ),
               DirectTextureSampler( std::move( directTextureSampler ) ), CastShadows( castShadows ),
               MaxDrawDistance( maxDrawDistance ), VisibleInRayTracing( visibleInRayTracing ), Params( &params )
        {
        }

        void Prepare() override
        {
            if ( Mesh != nullptr )
                SceneRenderer::PrepareMesh( *Mesh );
        }

        void Execute( SceneRenderer& renderer ) override
        {
            if ( !renderer.IsWithinMaxDrawDistance( Mesh, Transform, MaxDrawDistance ) )
                return;
            const Graphic::MaterialOverrides*                overrides  = &Overrides;
            const Graphic::RayTracing::RayTracingProgramHit* rayHit     = &RayHit;
            const std::string*                               shaderName = &ShaderName;
            // 0: the values are this command's own (hashed by the renderer); else the params' revision.
            SceneRenderer::GenericValuesRevision revision{};
            if ( Params != nullptr )
            {
                overrides         = &Params->Overrides;
                rayHit            = &Params->RayHit;
                revision.Revision = Params->Revision;
                if ( !Params->ShaderName.empty() )
                {
                    shaderName             = &Params->ShaderName;
                    revision.HasShaderName = true;
                }
            }
            renderer.SubmitGenericMesh( Entity, Mesh, Transform, *shaderName, *overrides, Outlined, DirectTexture,
                                        DirectTextureSampler, CastShadows, VisibleInRayTracing, *rayHit,
                                        revision );
        }
    };
} // namespace Desert::Graphic::Render
