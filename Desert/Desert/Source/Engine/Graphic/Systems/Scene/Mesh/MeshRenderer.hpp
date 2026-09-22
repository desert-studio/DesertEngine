#pragma once

#include <Common/Core/Units.hpp>

#include <Engine/Graphic/Systems/RenderSystem.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/Materials/MaterialOverrides.hpp>
#include <Engine/Core/Camera.hpp>
#include <Engine/Graphic/Materials/Mesh/MaterialSilhouette.hpp>
#include <Engine/Graphic/Materials/Mesh/MaterialShadow.hpp>
#include <Common/Core/DevInstruments.hpp>
#if DESERT_DEV_INSTRUMENTS
#include <Engine/Graphic/Materials/Debug/MaterialDebugLine.hpp>
#include <Engine/Graphic/Materials/Debug/MaterialOverdraw.hpp>
#include <Engine/Graphic/Materials/Debug/MaterialOverdrawResolve.hpp>
#endif
#include <Engine/Graphic/Materials/Mesh/PBR/MaterialPBR.hpp>
#include <Engine/Graphic/Materials/Mesh/PBR/PBRSceneFrame.hpp>
#include <Engine/Graphic/Materials/DataDrivenMaterial.hpp>
#include <Engine/Graphic/Environment/SceneEnvironment.hpp>
#include <Engine/Graphic/RenderGraphBuilder.hpp>
#include <Engine/Graphic/ShadowCascades.hpp>

#include <Engine/Geometry/SkinnedMesh.hpp>
#include <Engine/Geometry/StaticMesh.hpp>

#include <memory>
#include <string>
#include <utility>
#include <unordered_map>

namespace Desert::Graphic::System
{
    struct MeshRenderData
    {
        class Mesh* Mesh;
        glm::mat4   Transform;

        // A CO-OWNED handle on the entity's slots — see Graphic::MaterialSlotBinding (A8-3). Still no
        // per-mesh slot-vector copy (that cost ~0.9 ms of CmdBuffer ExecuteAll for 256 meshes in Debug);
        // what travels the chain is a shared_ptr, so the slots and the instances in them cannot be freed
        // under a draw that is still in flight.
        MaterialSlotBindingPtr MaterialSlots;

        // optional
        std::vector<glm::mat4> BoneMatrices;

        bool     Outlined        = false;
        uint64_t HiddenSubmeshes = 0;  // bit i = submesh i hidden (static meshes)
        int      ForcedLOD       = -1; // -1 = auto (by distance); 0..N pins a LOD level
        int      LODBias         = 0;  // shifts the auto LOD (ignored when forced)
        bool     CastShadows     = true;
        bool     ReceiveShadows  = true;
    };

    class MeshRenderer final : public RenderSystem
    {
    public:
        struct StaticMeshRenderData
        {
            class Desert::StaticMesh* Mesh      = nullptr;
            glm::mat4                 Transform = glm::mat4( 1.0f );
            // Co-owned, and it must be: this queue is read by five passes, all of them AFTER the frame's
            // ECS systems have run (A8-3).
            MaterialSlotBindingPtr MaterialSlots;
            bool                   Outlined        = false;
            uint64_t               HiddenSubmeshes = 0;  // bit i = submesh i hidden
            int                    ForcedLOD       = -1; // -1 = auto (by distance)
            int                    LODBias         = 0;  // shifts the auto LOD (ignored when forced)
            bool                   CastShadows     = true;
            bool                   ReceiveShadows  = true;
        };

        struct SkinnedMeshRenderData
        {
            class Desert::SkinnedMesh* Mesh      = nullptr;
            glm::mat4                  Transform = glm::mat4( 1.0f );
            // The (surface x Skinned) material. It is SHARED with every other entity using the same
            // `.demat`, which is why nothing per-object may be stored on it: the pose below is packed
            // into a per-frame buffer and named by a push constant instead.
            class Graphic::MaterialPBR* Material = nullptr;
            // The binding the instance below was selected FROM, carried so that it keeps that instance
            // alive: the entity that authored it can be destroyed between the record and this queue being
            // drawn (A8-3). Holding the binding rather than a second shared_ptr to the instance keeps the
            // static and skinned queues answering the lifetime question the same way.
            MaterialSlotBindingPtr      MaterialSlots;
            MaterialInstance*           Instance = nullptr; // instance applied during Bind; owned by MaterialSlots
            std::vector<glm::mat4>      BoneMatrices;       // animated pose, or bind pose
            bool                        Outlined    = false;
            bool                        CastShadows = true;
        };

        // A UE-style Instanced Static Mesh: ONE mesh + ONE PBR material drawn N times. The material and the
        // transforms are CO-OWNED handles on what the component produced, not pointers into it (A8-3).
        // Rendered through the SAME instanced pipeline/SSBO as the auto-batched static meshes.
        struct InstancedMeshRenderData
        {
            // A Mesh, not a StaticMesh: a primitive ISM carries a DynamicMesh, and so does one the
            // Foliage tool builds. See SceneRenderer::SubmitInstancedMesh for the cast this replaced.
            class Desert::Mesh*                           Mesh = nullptr;
            MaterialInstancePtr                           Material;   // slot 0 (PBR)
            std::shared_ptr<const std::vector<glm::mat4>> Transforms; // snapshot of InstanceTransforms
            bool                                          CastShadows = true;
        };

        // A static mesh drawn with a generic data-driven material. Two producers:
        //  - MaterialComponent (Shader Override) on the whole entity: ShaderName + Overrides drive a
        //    shader-keyed shared material (SlotMaterial == nullptr, all submeshes).
        //  - v3 per-slot materials: SlotMaterial points at the slot's own DataDrivenMaterial (asset
        //    params already applied) and VisibleSubmeshMask limits the draw to that slot's submeshes.
        struct GenericMeshRenderData
        {
            class Mesh*               Mesh      = nullptr;
            glm::mat4                 Transform = glm::mat4( 1.0f );
            std::string               ShaderName;
            Graphic::MaterialOverrides Overrides;
            bool                      Outlined = false; // selected -> JFA outline

            // Whether this record rasterizes into the shadow cascades. DEFAULT OFF, unlike the PBR
            // queue's flag: a generic draw is not necessarily a solid object. The text system submits
            // its SDF glyph quads through this same queue, and the shadow pass has no alpha test — a
            // default of true would hang an opaque rectangle in the cascade behind every 3D label.
            // The producers that ARE meshes opt in; ECS::Rules::RouteMeshShadowCaster decides which
            // single draw of an entity does so, because the shadow pass draws a mesh WHOLE.
            bool CastShadows = false;

            Graphic::Material*        SlotMaterial      = nullptr; // owned by MaterialService (stable)
            uint64_t                  VisibleSubmeshMask = ~0ull;  // bit i = submesh i drawn

            // A RUNTIME-owned texture bound straight to a sampler (bypasses the asset-handle
            // texture-override path). For procedural textures with no TextureAsset — e.g. the text
            // system's SDF font atlas. Non-owning: the producer keeps it alive for the frame.
            Graphic::Image2D* DirectTexture        = nullptr;
            std::string       DirectTextureSampler;
        };

        using RenderSystem::RenderSystem;

        // Gathers the scene's whole per-frame contribution (Graphic::PBRSceneFrame) from the scene
        // renderer + this renderer's own cascade state. One place that knows what "per-frame scene state"
        // IS; the snapshot itself lives beside the materials it is applied to, because it is their
        // payload and not this renderer's private business.
        PBRSceneFrame CaptureFrameState( const Core::Camera* camera ) const;

        // Cascaded shadow maps: the CEILING on directional-shadow cascades — how many the arrays below
        // hold and how many the ShadowUB block can carry. It is the block's own constant, so the cascades
        // this renderer can fit and the cascades a shader can read are one number and cannot drift apart.
        //
        // HOW MANY ARE ACTUALLY ALLOCATED IS NOT THIS, and the distinction is the whole of the preview
        // shadow budget: the count, the resolution and the distance come from the renderer's own
        // ShadowQuality (SceneRenderer::GetShadowQuality), read once in Initialize. See ShadowCascades.hpp.
        static constexpr uint32_t kMaxCascades = MaterialPBRBase::kMaxCascades;
        static_assert( kMaxCascades == kMaxShadowCascades,
                       "The ShadowUB block's cascade count and the fitter's array bound are the same "
                       "number seen from two sides; a renderer that fitted more than the block can carry "
                       "would write matrices no shader ever reads." );

        virtual Common::BoolResultStr Initialize() override;
        virtual void                  RegisterPasses( RenderGraphBuilder& builder ) override;

        // Silhouette mask of the currently outlined meshes (white on the framebuffer clear color).
        // Consumed by JumpFloodOutlineRenderer to build the outline.
        // Deferred: renders the static-mesh queue into the scene renderer's G-buffer via a MANUAL render pass
        // (outside the graph — see the note in RegisterPasses). No-op unless the deferred pipeline exists.
        // Called by SceneRenderer when RenderPath == Deferred, before the deferred lighting pass.
        void RenderGBufferManual();
        // Forward transparent (glass) pass: draws meshes with material Transmission > 0 over the composited
        // scene. sceneColor = a snapshot of the opaque scene the glass samples for refraction (may be null).
        void RenderGlassManual( const std::shared_ptr<Image2D>& sceneColor );
        // Deferred path: draws the generic (custom-shader) meshes FORWARD over the deferred
        // lighting composite in a LOAD render pass — they have no G-buffer variant, so without
        // this they simply vanish in Deferred. Forward path draws them inside MeshGeometryPass.
        void RenderGenericManual();
        // Deferred path: draws SKINNED meshes forward over the deferred lighting composite (they have no
        // G-buffer variant, so without this they only appear in the silhouette/outline pass — invisible
        // otherwise). Forward path draws them inside MeshGeometryPass.
        void RenderSkinnedManual();
        // Reflective Shadow Map: the G-buffer rasterized from the SUN instead of the camera, into the scene
        // renderer's RSM buffer. Every lit texel becomes a virtual point light for the RSM GI mode, which is
        // what lets off-screen geometry bounce light. No-op unless the deferred pipeline exists.
        void RenderRSMManual();
        // World -> RSM clip for the pass above — the GI resolve needs it to project fragments into the
        // sun's view. Valid after UpdateCascades(); identity before the first frame.
        glm::mat4 GetRSMViewProj() const
        {
            return m_RSMViewProj;
        }

        const std::shared_ptr<Framebuffer>& GetSilhouetteMaskFramebuffer() const
        {
            return m_SilhouetteMaskFramebuffer;
        }

#if DESERT_DEV_INSTRUMENTS
        // Overdraw debug view: re-rasterize every opaque mesh with additive blend (no depth) into a float
        // accumulation buffer, then heat-map the per-pixel overdraw count over the finished scene colour.
        // Path-independent (re-draws geometry; ignores the G-buffer), so it works in Forward and Deferred.
        void RenderOverdrawManual();

        const std::shared_ptr<Framebuffer>& GetOverdrawFramebuffer() const
        {
            return m_OverdrawFB;
        }
#endif // DESERT_DEV_INSTRUMENTS

        // True if any queued mesh is flagged for the selection outline this frame. The Jump Flood pass
        // uses this to skip its (log2(width)) full-screen ping-pong passes when nothing is selected.
        bool HasOutline() const
        {
            for ( const auto& d : m_StaticQueue )
                if ( d.Outlined )
                    return true;
            for ( const auto& d : m_GenericQueue )
                if ( d.Outlined )
                    return true;
            for ( const auto& d : m_SkinnedQueue )
                if ( d.Outlined )
                    return true;
            return false;
        }

        void SubmitMesh( const MeshRenderData& data );
        void SubmitGenericMesh( const GenericMeshRenderData& data );
        void SubmitInstancedMesh( const InstancedMeshRenderData& data );
        void ClearQueues();

        // THE BOUNDARY IS DRAWN INSIDE THIS HEADER, not at the call sites, and Common/Core/Profiler.hpp
        // draws its own the same way for the same reason: the API stays one shape in both
        // configurations, so every caller — including the next one somebody writes — compiles unchanged
        // and is cut automatically. Desert/Tests/Runtime/ShippingBoundary calls this form
        // `Gating::InItsOwnHeader` and calls it the stronger of the two. The alternative, an `#if`
        // around each call in SceneRenderer and around each branch in the draw loop, is four more
        // places to forget.

        // Debug wireframe toggle (DebugViewState::WireframeMode) — selects the line-polygon pipeline.
        // In a player's build the argument is accepted and dropped: there is no wireframe pipeline to
        // select, because nothing in a player can set the flag that would select it.
        void SetWireframe( [[maybe_unused]] bool enabled )
        {
#if DESERT_DEV_INSTRUMENTS
            m_Wireframe = enabled;
#endif
        }

        [[nodiscard]] bool WireframeView() const
        {
#if DESERT_DEV_INSTRUMENTS
            return m_Wireframe;
#else
            return false;
#endif
        }

        [[nodiscard]] GraphicsPipeline* WireframePipelineOr( GraphicsPipeline* fallback ) const
        {
#if DESERT_DEV_INSTRUMENTS
            return ( m_Wireframe && m_StaticWireframePipeline ) ? m_StaticWireframePipeline.get() : fallback;
#else
            return fallback;
#endif
        }

        // Distance-based mesh LOD (auto). LOD0 is byte-identical to the base geometry, so this only
        // affects meshes far from the camera. Toggle from the editor's Graphics menu.
        void SetLODEnabled( bool enabled )
        {
            m_LODEnabled = enabled;
        }
        bool IsLODEnabled() const
        {
            return m_LODEnabled;
        }

        // Which LOD level to draw for a mesh: a forced level (>= 0) wins; otherwise auto by screen
        // coverage (world bounding radius / camera distance), so big objects keep detail farther than
        // small ones. Returns 0 when LOD is off or there's no camera. Used by every mesh draw path.
        uint32_t ComputeLOD( const glm::mat4& transform, const class Desert::Mesh* mesh, int forcedLOD,
                             int lodBias = 0 ) const;

        // Cascaded shadow maps (R32F light-space depth, one framebuffer per cascade). Recompute the
        // per-cascade light matrices once per frame BEFORE the render graph records (intra-phase order is
        // nondeterministic). Called from SceneRenderer::OnUpdate.
        void UpdateCascades();

        // Cascade depth map (for the editor's CSM debug viewer). Null if out of range / not yet created.
        std::shared_ptr<Image2D> GetCascadeShadowImage( uint32_t cascade ) const
        {
            if ( cascade >= m_Shadow.CascadeCount || !m_CascadeFB[cascade] )
                return nullptr;
            return m_CascadeFB[cascade]->GetColorAttachmentImage();
        }

        // HOW MANY CASCADES THIS RENDERER ACTUALLY HAS — not the ceiling. It was `static constexpr`, and
        // that is precisely what made a per-renderer budget impossible: every consumer, including the
        // deferred composite's own shadow gather, asked the CLASS instead of the object and would have
        // read four for a renderer holding one.
        [[nodiscard]] uint32_t GetCascadeCount() const
        {
            return m_Shadow.CascadeCount;
        }

        // HOW MANY OF THEM CARRY A MATRIX FROM THIS FRAME'S FIT — the number every SHADER-facing consumer
        // wants, and a different question from the one above. `GetCascadeCount` answers "what does this
        // renderer own" (the budget: what to allocate, what to show in Scene Settings); this answers "what
        // may be sampled right now". They are equal in the ordinary frame and diverge exactly when
        // UpdateCascades does not run to completion — no main camera, no directional light, a degenerate
        // fit — which is when publishing the budget hands the shader matrices nothing ever wrote.
        //
        // The min() is belt and braces against the two ever being written from different places again.
        [[nodiscard]] uint32_t GetValidCascadeCount() const
        {
            return m_FittedCascades < m_Shadow.CascadeCount ? m_FittedCascades : m_Shadow.CascadeCount;
        }

        // The budget this renderer was created with. Fixed after Initialize — the framebuffers are
        // allocated from it once.
        [[nodiscard]] const ShadowQuality& GetShadowQuality() const
        {
            return m_Shadow;
        }

        void SetShadows( bool enabled, float bias, int debugMode, float splitLambda )
        {
            m_ShadowsEnabled  = enabled;
            m_ShadowBias      = bias;
            m_ShadowDebugMode = debugMode;
            m_SplitLambda     = splitLambda;
        }
        bool  AreShadowsEnabled() const { return m_ShadowsEnabled; }
        float GetShadowBias() const     { return m_ShadowBias; }
        // CSM data the deferred lighting pass needs to shadow the sun (same source the forward material uses).
        const glm::mat4* GetCascadeViewProj() const        { return m_CascadeVP; }
        const glm::vec4& GetCascadeWorldPerTexel() const   { return m_CascadeWorldPerTexel; }

        // Debug visualizations. TWO KINDS, and the boundary runs between them: `showNormals` and
        // `lightingDebug` are BRANCHES IN THE PBR SHADER, so they travel with the program and cost no
        // pipeline; the AABB wireframes are drawn by a pipeline of their own, so a player's build has
        // neither the pipeline nor the fields, and the three arguments are accepted and dropped.
        void SetDebugView( bool showNormals, [[maybe_unused]] bool showBoundingBoxes,
                           [[maybe_unused]] const glm::vec3& bbColor, [[maybe_unused]] float bbLineWidth,
                           bool lightingDebug = false )
        {
            m_ShowNormals   = showNormals;
            m_LightingDebug = lightingDebug;
#if DESERT_DEV_INSTRUMENTS
            m_ShowBoundingBoxes    = showBoundingBoxes;
            m_BoundingBoxColor     = bbColor;
            m_BoundingBoxLineWidth = bbLineWidth;
#endif
        }

    private:
        bool SetupGeometryPass();
        bool SetupGBufferPass(); // deferred: static-mesh G-buffer write pipeline
        bool SetupGlassPass();   // forward transparent: static-mesh glass pipeline (blend, composites over scene)
        bool SetupSkinnedGeometryPass();
        bool SetupSilhouettePass();
        bool SetupShadowPass();
        // The one place the shadow budget is said out loud. Called from both arms of SetupShadowPass —
        // the allocating one and the zero-budget one — because a renderer that spends nothing on the sun
        // is exactly as worth reading in a log as one that spends 320 MiB, and a line printed on only one
        // path is a line whose absence means two different things.
        void LogShadowBudget( double allocMs ) const;

        void DrawStaticMeshes();
        void DrawSkinnedMeshes( bool useLoadPass = false );
        void DrawGenericMeshes( bool useLoadPass = false ); // per-object data-driven materials (v3 slots + overrides)
        void RegisterSilhouettePass( RenderGraphBuilder& builder );
        void RegisterShadowPass( RenderGraphBuilder& builder );
#if DESERT_DEV_INSTRUMENTS
        bool SetupDebugLinePass();
        bool SetupOverdrawPass(); // overdraw accumulation pipeline + FB + fullscreen heat resolve
        void RegisterDebugPass( RenderGraphBuilder& builder );
#endif // DESERT_DEV_INSTRUMENTS

    private:
        // Static
        std::shared_ptr<GraphicsPipeline> m_StaticPipeline;
#if DESERT_DEV_INSTRUMENTS
        std::shared_ptr<GraphicsPipeline> m_StaticWireframePipeline; // same spec, PolygonMode::Wireframe
#endif
        std::shared_ptr<GraphicsPipeline> m_StaticInstancedPipeline; // reads per-instance transform from SSBO
#if DESERT_DEV_INSTRUMENTS
        bool m_Wireframe = false;
#endif
        bool                              m_LODEnabled = true;

        // Deferred G-buffer geometry pipeline (static): writes Albedo+Metallic / Normal+Roughness into the
        // scene renderer's MRT G-buffer instead of shading. Same vertex layout + material bindings as the
        // forward static pipeline, so the same StaticMaterialPBR data binds. Null if the shader is missing.
        std::shared_ptr<Shader>           m_StaticGBufferShader;
        std::shared_ptr<GraphicsPipeline> m_StaticGBufferPipeline;
        // (Instanced x GBuffer). The G-buffer pass used to have no instanced cell at all, and the ISM
        // queue -- which has no per-object path to fall back to -- was therefore dropped entirely in the
        // deferred path. Null if the shader is missing, and DrawStaticMeshes says so rather than dropping
        // the queue again.
        std::shared_ptr<Shader>           m_InstancedGBufferShader;
        std::shared_ptr<GraphicsPipeline> m_InstancedGBufferPipeline;
        bool                              m_DeferredGeometry = false; // set true only while drawing the G-buffer pass
        std::shared_ptr<Shader>           m_StaticGlassShader;
        std::shared_ptr<GraphicsPipeline> m_StaticGlassPipeline;
        // `bool m_GlassPass` stood here, described as "set true only while drawing the transparent glass
        // pass". No line in the engine ever set it, so its two readers were a transparency test that could
        // only ever mean "skip glass" and a pipeline branch nothing could reach — and the unreachable
        // branch was the one that would have bound a FORWARD material against the glass pipeline, which is
        // the only reason StaticMeshGlass.shader was padded to the forward layout. Both are gone.
        // DEDICATED glass material (never drawn by the opaque passes) so its per-frame UB ring is written ONCE
        // per frame in the glass pass — sharing an opaque material across two passes/frame hangs the GPU.
        // It is (Static x Glass) rather than its own class: what made it different from the opaque
        // material was always the shader, and the shader is what the pair names.
        std::shared_ptr<MaterialPBR> m_GlassMaterial;
        MaterialInstancePtr          m_GlassInstance;

        // Reflective Shadow Map (G-buffer from the sun) — the off-screen bounce source for the RSM GI mode.
        // Its camera UB carries the SUN's matrices, so like glass it needs its OWN material: sharing one with
        // the opaque passes would write the same per-frame UB twice in a frame. m_RSMViewProj/m_RSMEye come
        // from cascade 1 in UpdateCascades(), so the pass draws through a STANDARD-Z matrix and needs its
        // own pipeline (m_RSMPipeline) rather than the reversed-Z G-buffer one — see SetupDeferredPass.
        // (Static x GBuffer) — the RSM is literally a G-buffer rasterized from the sun.
        std::shared_ptr<MaterialPBR>       m_RSMMaterial;
        MaterialInstancePtr                m_RSMInstance;
        std::shared_ptr<GraphicsPipeline>  m_RSMPipeline;
        glm::mat4                          m_RSMViewProj = glm::mat4( 1.0f );
        glm::vec3                          m_RSMEye      = glm::vec3( 0.0f );

        // (Static x GBuffer) for meshes whose FORWARD material is not one MaterialService owns — in
        // practice only MeshECSSystem's default material, which stands in for a mesh whose slot did not
        // resolve. It has no `.demat`, so it has no sibling in any other pass, and the deferred pass
        // dropped those meshes entirely until this existed. See SetupGBufferPass for why ONE is enough.
        std::shared_ptr<MaterialPBR> m_GBufferUnownedMaterial;

        std::shared_ptr<Shader>   m_GeometryShader;
        std::shared_ptr<Shader>   m_InstancedGeometryShader;

        // Auto-batching SPARE: the material an instanced batch is recorded with when the batch's own
        // material came from no `.demat` at all — in practice MeshECSSystem's default, standing in for a
        // mesh whose slot did not resolve.
        //
        // IT USED TO RECORD EVERY BATCH, AND THAT WAS THE DEFECT. A material built here has never been
        // through MaterialFactory, so every 2D sampler it declares holds the shader schema's 1x1 white.
        // Recording an asset-backed group with it deleted that surface's whole texture channel while
        // leaving its colours, tiling-independent, intact — see InstancedRecorder.hpp for the numbers and
        // for why rebinding this one material per batch cannot be the fix. A batch now finds its own
        // (Instanced x pass) sibling through MaterialService; this stays for the one group that has none.
        std::shared_ptr<Graphic::MaterialPBR> m_StaticInstancedMaterial;
        MaterialInstancePtr                   m_StaticInstancedInstance;
        // The same pair for the G-buffer pass. A material is one shader's descriptor sets plus a payload,
        // so the pass that binds the (Instanced x GBuffer) pipeline has to bind sets allocated from that
        // cell's own reflection -- the same reason m_RSMMaterial and m_GBufferUnownedMaterial exist for
        // the static path.
        std::shared_ptr<Graphic::MaterialPBR> m_InstancedGBufferMaterial;
        MaterialInstancePtr                   m_InstancedGBufferInstance;

        // ONE MaterialInstance per instanced material the service hands out, kept because an instance is
        // allocated per material and a batch needs one to Bind with — not per frame and not per object.
        //
        // Keyed by the raw material and dropped WHOLE whenever MaterialService's invalidation stamp
        // moves: a reloaded `.demat` graveyards every runtime material it built, so an entry surviving
        // that bump is a MaterialInstance holding a parent pointer into a material about to be destroyed.
        // The same stamp MeshECSSystem already rebuilds its cached slots on.
        std::unordered_map<const Graphic::MaterialPBR*, MaterialInstancePtr> m_InstancedVariantInstances;
        uint32_t                                                             m_InstancedVariantStamp = 0;

        // Skinned
        std::shared_ptr<GraphicsPipeline> m_SkinnedPipeline;
        std::shared_ptr<Shader>   m_SkinnedShader;

        // Silhouette (mask for the Jump Flood outline)
        std::shared_ptr<GraphicsPipeline>   m_SilhouettePipeline;
        std::shared_ptr<Shader>             m_SilhouetteShader;
        std::unique_ptr<MaterialSilhouette> m_SilhouetteMaterial;
        std::shared_ptr<Framebuffer>        m_SilhouetteMaskFramebuffer;
        // Skinned silhouette (selected skinned meshes -> outline). Skins by the Bones SSBO so the mask
        // matches the posed/animated mesh. Optional — null if the Silhouette_Skinned shader is missing.
        std::shared_ptr<GraphicsPipeline>          m_SilhouetteSkinnedPipeline;
        std::shared_ptr<Shader>                    m_SilhouetteSkinnedShader;
        std::unique_ptr<MaterialSilhouetteSkinned> m_SilhouetteSkinnedMaterial;

        // Cascaded directional shadow maps: one framebuffer + one MaterialShadow (its own light-matrix UB,
        // so the 4 cascade passes don't alias a shared UBO) per cascade. m_CascadeVP is recomputed each
        // frame by UpdateCascades().
        std::shared_ptr<GraphicsPipeline> m_ShadowPipeline;
        std::shared_ptr<Shader>           m_ShadowShader;
        std::unique_ptr<MaterialShadow>   m_ShadowMaterial[kMaxCascades];
        std::shared_ptr<Framebuffer>      m_CascadeFB[kMaxCascades];

        // Instanced shadow caster: one pipeline + per-cascade instanced material (each owns the cascade's
        // light matrix UBO + an InstanceTransforms SSBO). Batched casters of one mesh collapse to a single
        // instanced draw per cascade. Optional — null if the Shadow_Instanced shader is missing.
        std::shared_ptr<GraphicsPipeline>        m_ShadowInstancedPipeline;
        std::shared_ptr<Shader>                  m_ShadowInstancedShader;
        std::unique_ptr<MaterialShadowInstanced> m_ShadowInstancedMaterial[kMaxCascades];

        // Skinned shadow caster: the (Skinned x ShadowDepth) cell, which did not exist — the cascade pass
        // walked the static queue by name and a character cast nothing. One material per cascade, exactly
        // like the two above, and every skinned caster's pose packed into its single Bones buffer.
        // Optional — null if the Shadow_Skinned shader is missing, and then skinned shadows are simply off.
        std::shared_ptr<GraphicsPipeline>      m_ShadowSkinnedPipeline;
        std::shared_ptr<Shader>                m_ShadowSkinnedShader;
        std::unique_ptr<MaterialShadowSkinned> m_ShadowSkinnedMaterial[kMaxCascades];

        // THE BUDGET, copied from the owning SceneRenderer in Initialize() and never written again. Held
        // by value rather than read through m_SceneRenderer on every use, because the three numbers decide
        // what was ALLOCATED: reading them live would let a later write leave the fitter and the
        // framebuffers disagreeing, which is the one failure this cannot be allowed to have.
        ShadowQuality m_Shadow;

        // The live-total accounting for the cascade framebuffers, held BESIDE them so it is released
        // exactly when they are — see Graphic::ShadowAttachmentLease for why this is RAII and not a pair
        // of hand-written add/remove calls.
        ShadowAttachmentLease m_ShadowAttachments;

        // How many of m_CascadeVP below were written by THIS frame's fit (0 until UpdateCascades runs).
        // See GetValidCascadeCount for why this is not the same number as m_Shadow.CascadeCount.
        uint32_t m_FittedCascades = 0;

        // EVERY element identity, spelled element by element. `= { glm::mat4( 1.0f ) }` reads as "all
        // identity" and is not: it initializes element 0 and VALUE-initializes the rest, so cascades 1..3
        // began life as ZERO matrices, whose w is 0 and whose perspective divide is a division by zero in
        // the sampling shader. Nothing should ever read past m_FittedCascades — this is what the array
        // holds if something does.
        glm::mat4 m_CascadeVP[kMaxCascades] = { glm::mat4( 1.0f ), glm::mat4( 1.0f ), glm::mat4( 1.0f ),
                                                glm::mat4( 1.0f ) };
        static_assert( kMaxCascades == 4, "m_CascadeVP's initializer lists one identity per cascade" );
        // World-space size of one shadow-map texel per cascade (2*radius/res) — drives a cascade-correct
        // normal-offset/bias in the PBR shader instead of the old fixed world-unit constants.
        glm::vec4                         m_CascadeWorldPerTexel    = glm::vec4( 1.0f );
        bool                              m_ShadowsEnabled  = true;
        float                             m_ShadowBias      = 0.005f;
        int                               m_ShadowDebugMode = 0;     // ShadowDebugMode (Off/ShadowFactor/Cascades)
        float                             m_SplitLambda     = 0.6f;  // cascade split uniform<->log blend

        // Debug visualization (Scene Settings -> Debug)
        bool      m_ShowNormals          = false; // per-pixel normal color (PBR shader branch)
        bool      m_LightingDebug        = false; // per-light colored "where light lands" (PBR shader branch)
#if DESERT_DEV_INSTRUMENTS
        bool      m_ShowBoundingBoxes    = false; // AABB wireframes via the debug line renderer below
        glm::vec3 m_BoundingBoxColor     = glm::vec3( 0.25f, 0.95f, 0.35f );
        float     m_BoundingBoxLineWidth = 1.5f;

        // Debug line renderer (AABB wireframes): Lines-topology pipeline + storage-buffer line verts.
        std::shared_ptr<GraphicsPipeline>  m_DebugLinePipeline;
        std::shared_ptr<Shader>            m_DebugLineShader;
        std::unique_ptr<MaterialDebugLine> m_DebugLineMaterial;

        // Overdraw view: geometry accumulation (additive, no depth) into m_OverdrawFB, then a fullscreen
        // resolve that heat-maps the count over the scene colour. Static/generic meshes only (skinned skipped).
        std::shared_ptr<GraphicsPipeline>        m_OverdrawPipeline;
        std::shared_ptr<Shader>                  m_OverdrawShader;
        std::unique_ptr<MaterialOverdraw>        m_OverdrawMaterial;
        std::shared_ptr<Framebuffer>             m_OverdrawFB;
        std::shared_ptr<GraphicsPipeline>        m_OverdrawResolvePipeline;
        std::shared_ptr<Shader>                  m_OverdrawResolveShader;
        std::unique_ptr<MaterialOverdrawResolve> m_OverdrawResolveMaterial;
#endif // DESERT_DEV_INSTRUMENTS

        std::vector<StaticMeshRenderData>  m_StaticQueue;
        std::vector<SkinnedMeshRenderData> m_SkinnedQueue;

        // Generic (data-driven) static meshes + a cache of the materials that record them. Per-object data
        // is the transform push constant plus a ROW of the material's `Materials[]` buffer, named by a
        // second push constant — so any number of objects share one material and still carry their own
        // parameter values.
        //
        // KEYED BY SHADER **AND TEXTURE SET** (see GenericTextureKey). It was keyed by shader alone, which
        // was the whole defect while parameters lived in the material too; textures still live there, and a
        // key that ignored them would batch two entities into one descriptor set and give both the last
        // one's textures.
        std::vector<GenericMeshRenderData>                              m_GenericQueue;
        std::unordered_map<std::string, std::unique_ptr<DataDrivenMaterial>> m_GenericMaterials;

        // UE-style Instanced Static Meshes (one entity = N instances). Folded into the shared instanced
        // pipeline/SSBO alongside the auto-batched static meshes (geometry + shadow passes).
        std::vector<InstancedMeshRenderData> m_InstancedQueue;

    private:
        // m_StaticMaterialFallback and m_SkinnedMaterialFallback stood here. The first was constructed
        // every Initialize() and read by nothing; the second was never even constructed. A mesh with no
        // resolvable slot gets its fallback from MeshECSSystem, which is the one place that knows a slot
        // failed to resolve — two fallbacks meant two answers to one question and only one of them ran.

        // ── Per-frame scratch (memory discipline: reuse, don't reallocate) ──────────────────
        // Cleared each use; capacity persists across frames so the steady state allocates nothing.

        // One draw-ready record per object: the effective material is built ONCE per object per
        // frame and reused for the glass split, the batch entry and the per-object SSBO.
        struct ObjDraw
        {
            const StaticMeshRenderData* Obj  = nullptr;
            MaterialInstance*           Inst = nullptr;
            PBRGpuMaterial              Gm{};
            bool                        HasOverrides = false;
        };
        struct InstancedDraw
        {
            // Fed by BOTH the auto-batched statics (a StaticMesh) and the ISM queue (any Mesh), so it
            // is the wider of the two — a draw call only ever needs what Mesh already carries.
            Desert::Mesh*       Mesh          = nullptr;
            uint32_t            InstanceCount = 0;
            uint32_t            FirstInstance = 0;
            uint32_t            MaterialIndex = 0;
            // A draw call carries ONE index range, so a batch carries ONE level. Before this field the
            // batched path drew at the default 0 while the per-object path next to it computed a level
            // and passed it — the same object at two detail levels depending on whether it batched.
            uint32_t LodLevel = 0;
        };

        // EVERY INSTANCED DRAW THAT ONE MATERIAL RECORDS, plus the two buffers those draws read.
        //
        // There is one of these per distinct recording material in the pass, and that count is the
        // change: the accumulation used to be a single triple shared by every batch of every material,
        // which is only expressible because every batch was recorded by the same renderer-owned material
        // — the thing that cost every batched surface its textures. A set's Materials[] rows are named by
        // a push constant (snapshotted per draw), so many batches of ONE material still share one buffer
        // and one upload; two MATERIALS cannot, because a descriptor set written twice in a frame keeps
        // only the last write (VulkanMaterialBackend::ApplyTexture2D reports the swallowed rebind).
        struct InstancedBatchSet
        {
            Graphic::MaterialPBR*       Mat  = nullptr;
            MaterialInstance*           Inst = nullptr;
            std::vector<glm::mat4>      Transforms;
            std::vector<PBRGpuMaterial> Materials;
            std::vector<InstancedDraw>  Draws;
        };

        // One recorded generic draw, resolved in DrawGenericMeshes' first pass and executed in its third.
        // The passes are separate because every row has to be uploaded at FINAL size before the first draw
        // is recorded — growing a storage buffer reallocates the VkBuffer under a draw already recorded
        // against the old one — and only the first pass knows how many rows there will be.
        struct GenericDraw
        {
            const GenericMeshRenderData*      Data     = nullptr;
            DataDrivenMaterial*               Material = nullptr;
            std::shared_ptr<GraphicsPipeline> Pipeline;
            uint32_t                          Row = 0;
        };
        // One material's rows, end to end: Count rows of (parameter slots per row) vec4s.
        struct MaterialRows
        {
            std::vector<glm::vec4> Bytes;
            uint32_t               Count = 0;
        };
        struct ShadowBatch
        {
            Desert::Mesh*       Mesh     = nullptr; ///< see InstancedDraw::Mesh — the same two producers
            uint32_t            Count = 0;
            uint32_t            First = 0;
            uint32_t            LodLevel = 0; ///< see InstancedDraw::LodLevel — same omission, same fix
        };

        // Every skinned pose drawn in one pass, packed end to end; each draw names its slice with a
        // BoneOffset push constant. ONE buffer per material per pass instead of one upload per draw,
        // which is what makes a shared skinned material correct — see MaterialPBR.hpp.
        std::vector<glm::mat4>                   m_ScratchBones;
        std::vector<glm::mat4>      m_ScratchInstTransforms; // geometry + shadow instanced SSBOs
        // Per-batch LOD levels and the surviving ISM transforms. Members rather than locals for the
        // reason every accumulator in this file is one: their capacity persists, so a frame that culls
        // 49 000 instances out of 49 152 allocates nothing.
        std::vector<uint32_t>                    m_ScratchLodLevels;
        std::vector<glm::mat4>                   m_ScratchIsmVisible;
        // The instanced batches of ONE geometry pass, one entry per recording material. Reused BY INDEX
        // rather than cleared, so the inner vectors keep their capacity across frames — clearing the
        // outer vector would destroy them and hand the steady state an allocation per material per frame.
        // `m_ScratchInstSetCount` is how many of them this frame is using.
        //
        // INDIRECT, AND THAT IS THE POINT AND NOT AN OVERSIGHT. The accumulation hands out a POINTER to
        // one set and then keeps filling it while later groups may ask for sets of their own; held by
        // value, the first `emplace_back` that grows this vector would move every set and leave that
        // pointer dangling — a use-after-free that no frame would show, because the freed memory is the
        // vector this thread just wrote. The control flow happens not to interleave the two today, which
        // is exactly the kind of "safe for now" that the next edit in DrawStaticMeshes turns into a
        // corrupted draw list. A unique_ptr costs one indirection per access and makes the address
        // stable by construction.
        std::vector<std::unique_ptr<InstancedBatchSet>> m_ScratchInstSets;
        std::size_t                                     m_ScratchInstSetCount = 0;
        std::vector<PBRGpuMaterial> m_ScratchGpuMaterials; // per-object Materials[] SSBO
        std::vector<ObjDraw>        m_ScratchSingles;
        std::vector<ShadowBatch>    m_ScratchShadowBatches;
        std::vector<const StaticMeshRenderData*> m_ScratchShadowSingles;
        std::vector<GenericDraw>                 m_ScratchGenericDraws;
        std::vector<std::pair<DataDrivenMaterial*, MaterialRows>> m_ScratchGenericRows;
    };
} // namespace Desert::Graphic::System
