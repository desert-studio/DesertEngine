#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace Desert::Core::Formats
{
    enum class SurfaceBlendMode : uint8_t; // ShaderProgramMeta.hpp
} // namespace Desert::Core::Formats

namespace Desert::Graphic
{
    // ── THE THREE AXES OF A MESH DRAW ───────────────────────────────────────────────────────────────
    //
    // A mesh draw is a PERMUTATION of three independent things, and the class hierarchy that stood here
    // was their cartesian product spelled out in C++ — `MaterialGlass` inherited from the static PBR class,
    // so a SHADING MODEL was a subclass of a (shading model x vertex path) pair.
    //
    //   1. the SHADING MODEL — PBR, glass, a node graph, unlit. What the fragment stage does with the
    //      surface. It belongs in a SHADER, named by the asset, with no privileged C++ type behind it.
    //   2. the VERTEX PATH — how the vertices are fetched and transformed before that surface is shaded.
    //      Static, skinned, hardware-instanced. A property of the GEOMETRY, chosen by the renderer, and
    //      the artist never names it. THIS ENUM.
    //   3. the SCENE-STATE BINDING — camera, lights, cascades, environment, cloud shadow. Legitimately
    //      C++, and ALREADY decoupled: `Graphic::PBRSceneFrame::ApplyTo` takes any `Material*` and writes
    //      by block NAME, so it reaches the PBR path, the skinned path, the graph path and unlit alike.
    //      It is a finished foundation and nothing here rebuilds it.
    //
    // Where each axis lives AFTER this change — the census, because the answer was smaller than the six
    // classes suggested:
    //
    //   | axis           | where it lives now                | how many C++ types |
    //   |----------------|-----------------------------------|--------------------|
    //   | shading model  | the shader NAME, from the `.demat`| 0 (it is data)     |
    //   | vertex path    | MeshVertexPath, a renderer argument| 0 (it is an enum) |
    //   | scene binding  | PBRSceneFrame + SceneLightingBinding| 1, already shared |
    //
    // `MaterialGlass` and `MaterialRSM` are gone entirely: they were a shader name and nothing else, so
    // they are cells of a template: glass is a translucent template's own cell, (Static, GBuffer) the RSM's.
    // And glass was never chosen by a class — `MeshRenderer::DrawStaticMeshes` splits it out by the template's
    // blend mode (ShaderProgramMeta::Blend), i.e. by DATA, which is what makes deleting the class safe.
    //
    // THERE IS NO C++ SPLIT LEFT. Every surface material — a `.demat` of any template, and the renderer's
    // own glass / RSM / instanced cells — is a DataDrivenMaterial of one cell: the shader of that cell, its
    // descriptor sets, and a row of the shared `Materials[]` storage buffer named by a push constant. What
    // a draw needs beyond that belongs to the vertex path (the skinned bone palette and offset,
    // Material::UploadSkinnedBones / SetSkinnedBoneOffset) or to the scene (PBRSceneFrame), never to a class.

    // THE TRANSPORT USED TO BE TWO, AND THE SECOND ONE IS GONE. `Properties Binding(n)` generated a
    // per-material `uniform MaterialUB` block. Measured 2026-09-04 in Debug, reading the mesh pass's own
    // GPU-timestamp line and taking the minimum of interleaved runs: on 1024 cubes sharing one material,
    // collapsing the batched draw into one draw per object moves MeshGeometryPass from 0.756 ms to
    // 17.121 ms of CPU (22.6x) and the whole frame from 11.477 ms to 27.973 ms (2.4x, 87 -> 35 FPS). The
    // GPU line does not move at all — the entire cost is submission. On the 101-mesh
    // MAT_ProbeCascadeSeam the pass moves 0.259 -> 1.855 ms and the frame does NOT, because at that size
    // the extra CPU still fits in the wait on the GPU; the frame-level number only appears once it does
    // not, which is why the stress scene exists.
    //
    // And the correctness argument pointed the same way, which was the stronger half. A per-material block
    // IS the parameters, so one material held exactly one set of values, and
    // MeshRenderer::DrawGenericMeshes keys ONE DataDrivenMaterial per shader for MaterialComponent
    // overrides. Resources/Assets/Scenes/MAT_ProbeSharedBlock.desce is three spheres whose only difference
    // is the graph parameter `Blend` (0.0 / 0.5 / 1.0) and all three rendered RED, the Blend = 0 colour;
    // MAT_ProbeSharedBlockSingle.desce is the same entity alone at Blend = 1.0 and rendered BLUE. So the
    // override path worked and the sharing was what broke it — the same defect shape as the bone matrices
    // that lived on a material and made two skinned meshes render one pose. The storage-buffer transport
    // cannot fail that way: a push constant is snapshotted per draw.
    //
    // Where the surviving transport is now written down: Engine/Core/Formats/MaterialParamRow.hpp (the
    // layout rule and the push offset both halves share), DShaderParser::BuildAutoDeclarations (the GLSL),
    // DataDrivenMaterial (the bytes) and MeshRenderer::DrawGenericMeshes plus TerrainRenderer (the packing
    // and the index). The census that was left here to notice when it landed is
    // Desert/Tests/Engine/ShippedShaderPasses, and it now expects ZERO.
    //
    // UE calls axis 2 a *vertex factory* and compiles (material x vertex factory x pass) into a shader
    // permutation at draw time; Unity's SRP reaches the same shape with per-pass shader variants and
    // `#pragma multi_compile` keywords; Godot's spatial shaders take a `skeleton` input the same way.
    // None of the three lets a material belong to a vertex path, and none of the three has a C++ class
    // per shading model.
    //
    // This engine had both. A static, a skinned and an instanced PBR class
    // were three C++ CLASSES for one surface model, one per path, and `MaterialService` resolved a
    // `.demat` into exactly one of them. The consequences were all one defect wearing different clothes:
    //
    //   1. an imported character with its own materials did not draw AT ALL — MeshRenderer looked for a
    //      slot whose parent was the skinned PBR class, and MaterialService could not build one from an
    //      asset under any circumstances (it answered the static class even for a `.demat` naming the
    //      skinned shader);
    //   2. a skinned mesh cast NO SHADOW — the cascade pass walked the static queue by name;
    //   3. a skinned mesh ignored per-instance material overrides — its Bind built the GPU material from
    //      the parent's data and never looked at the instance;
    //   4. two skinned meshes sharing one material rendered with ONE pose — the bone matrices lived on
    //      the material, so the last writer in the frame won for every draw recorded before it.
    //
    // The axis below is the fix: the surface stays in the asset, the path becomes an explicit enum the
    // renderer supplies, and a runtime material is identified by the PAIR. `MaterialService::Get` and
    // `CreateRuntimeInstance` therefore take a path, and one `.demat` legitimately yields one material
    // per path with the same parameters — it cannot drift, because there is one asset behind all of them.
    //
    // ── WHY THE DESCRIPTOR LAYOUT LIVES ON THE CELL AND NOT ON THE MATERIAL ──────────────────────────
    //
    // A `Graphic::Material` in this engine is "one shader's descriptor sets plus a parameter payload"
    // (Material.cpp: the constructor takes a shader NAME and builds a MaterialExecutor from its
    // reflection). So the descriptor layout was never a property of the surface — it is a property of the
    // shader, and the shader is chosen by (path x pass). That is exactly what `MeshShaderFor` below says.
    //
    // It also explains the strangest lines this renderer ever carried, and why they are gone.
    // `StaticMeshGBuffer.shader` used to multiply three environment samples, four cascade maps, two light
    // SSBOs, ShadowUB, the lights-metadata and directional-light blocks and the cloud-shadow pair by 1e-20
    // — fourteen descriptors it never read — purely so that its reflected layout stayed identical to
    // `StaticMeshPBR`'s. It had to: the G-buffer pass had NO MATERIAL, so MeshRenderer bound the
    // (Static x Forward) material's sets against the (Static x GBuffer) pipeline layout, and Vulkan
    // demands the two be compatible.
    //
    // `Runtime::MaterialService` keys a runtime material by (asset x path x PASS) now, so the G-buffer
    // pass binds sets allocated from its own shader and declares only what a G-buffer write reads. The
    // relation is asserted rather than commented, in two places for two reasons:
    // `Desert/Tests/Engine/MeshVertexPath` holds the G-buffer cell to being a PROPER subset of its path's
    // forward cell (padding coming back fails there), and `Desert/Tests/Engine/ShaderCacheKey` pins the
    // exact five descriptors it does declare.
    //
    // ── THE RELATION THIS TABLE EXISTS TO MAKE CHECKABLE ─────────────────────────────────────────────
    //
    // Every forward variant shades the SAME surface, so every forward variant must declare the same
    // surface bindings, differing only by what its own vertex stage adds (`MeshPathOwnBinding` below).
    // `Desert/Tests/Engine/MeshVertexPath` asserts that against the reflected SPIR-V rather than against
    // a list somebody maintains. A shader added to the table without matching the surface is caught
    // there, and not by a black character in somebody's scene.
    enum class MeshVertexPath : uint8_t
    {
        Static    = 0, // one model matrix per draw, from the push constant
        Skinned   = 1, // bone matrices from the Bones SSBO, offset per draw
        Instanced = 2, // one model matrix per INSTANCE, from the InstanceTransforms SSBO
    };

    inline constexpr uint32_t kMeshVertexPathCount = 3;

    // What the fragment stage of the draw WRITES. Orthogonal to the path above: the same geometry can be
    // rasterized into the lit scene, into the deferred G-buffer, into the transparent composite or into a
    // shadow cascade, and which one it is has nothing to do with how its vertices were fetched.
    //
    // A PASS, not a shading model — the distinction matters and `Glass` is where it is easiest to blur.
    // Glass is drawn in a blended pass over the composite; which objects go there is decided by the BLEND MODE
    // of the template their material draws with (ShaderProgramMeta::Blend, `BlendMode Translucent`), exactly as
    // UE routes a Translucent material into the translucency pass. Nothing about the shading model is encoded here.
    enum class MeshPass : uint8_t
    {
        Forward     = 0, // lit colour, the forward path and the over-composite draws
        GBuffer     = 1, // deferred albedo/normal MRT (also the Reflective Shadow Map, from the sun)
        Glass       = 2, // transparent, samples the composited scene for refraction
        ShadowDepth = 3, // depth only, into a cascade
    };

    inline constexpr uint32_t kMeshPassCount = 4;

    // (path x pass) -> the surface CELL that draws it, "<Path>.<Pass>" (DShaderParser's SurfaceCellName), or
    // nullptr where the pass is not drawn through a surface template's cell (Glass, which is its own program).
    //
    // THE TABLE NAMES NO TEMPLATE. It is the axis every surface template expands into (UE's
    // FMeshMaterialShaderMap is taken from the MATERIAL: which template, those permutations), so the engine
    // knows no concrete material's name; MeshShaderFor below joins a cell to the template the material names.
    //
    // THE HOLES ARE THE DIAGNOSIS. Before this table the two axes were implicit, so nobody could see
    // that (Skinned x ShadowDepth) did not exist — the cascade pass simply never mentioned skinned
    // meshes and a character stood in the sun casting nothing. The cell is filled now. The remaining
    // holes are deliberate and each has a reason:
    //
    //   (Skinned  x GBuffer) is NOT a hole any more: Forward and GBuffer are cells of every surface
    //                          template, which has every path. The skinned mesh is still drawn forward
    //                          (MeshRenderer::RenderSkinnedManual); the cell exists unused.
    //   (* x Glass)          — no cell of its own: the translucency pass draws a translucent template's
    //                          (Static x Forward) cell (MeshShaderFor); no skinned or instanced glass exists.
    //
    // (Instanced x GBuffer) WAS A HOLE AND WAS NOT ONE. Its stated reason — "instancing is disabled in
    // the G-buffer pass, every static takes the per-object path there" — was true of the AUTO-BATCHED
    // statics, which fall back to per-object draws when instancing is off, and FALSE of an
    // InstancedStaticMesh entity, which is one entity carrying N transforms and has no per-object path
    // to fall back to. So the G-buffer pass dropped the whole ISM queue with no line in the log, in the
    // render path 81 of the repository's 88 scenes state. The cell is filled (Г26); the comment is kept
    // because a hole justified by a half-true sentence is the failure this table exists to make visible.
    const char* MeshCellFor( MeshVertexPath path, MeshPass pass );

    // THE SHADER a (path x pass) of a material built on the surface template @p templateName draws with:
    // "<Template>/<MeshCellFor(path, pass)>" — the program ShaderService registers for every template that
    // declares a Surface block — for (Static x Glass) the template's own Static.Forward cell, each translucent
    // template drawing with its own shader in the translucency pass. Empty = a hole (or no template
    // named), and the caller must SAY so rather than silently drawing something else. The template name comes
    // from the material (or, for the renderer's own draws, from the template declaring `Default Surface`,
    // found by that role — MaterialService::DefaultSurfaceTemplate), never from a literal here.
    std::optional<std::string> MeshShaderFor( std::string_view templateName, MeshVertexPath path, MeshPass pass );

    // THE ONE RULE of which (path x pass) cells a material on the template @p templateName HAS, and the shader
    // that draws each. A template with a Surface block registers every cell of the table ("<Template>/<Cell>"),
    // and the cell is MeshShaderFor. A template with no Surface block registers no cells: it is drawn by its own
    // default program on the generic path, which is its (Static x Forward) and nothing else — so every other cell
    // is a hole the caller must refuse. @p isRegistered answers whether a program of that name is registered
    // (ShaderService at run time); MaterialService builds by this and answers CellOf by it, so "can this draw
    // here" and "what draws here" cannot disagree. Empty = no cell.
    std::optional<std::string> TemplateCellShader( std::string_view templateName, MeshVertexPath path, MeshPass pass,
                                                   const std::function<bool( std::string_view )>& isRegistered );

    // The inverse of MeshShaderFor: which vertex path a compiled cell shader ("<Template>/<Cell>", any
    // template) belongs to, or nothing for a shader that is no cell of the table (a template without a
    // Surface block, drawn by its own default program on the generic path). This is the vertex
    // factory question the mesh renderer asks of a material — "can you be drawn on the batched static /
    // skinned / instanced path" — answered by the SHADER the material was allocated from, never by its C++
    // class or its template's name.
    std::optional<MeshVertexPath> MeshCellPath( std::string_view shaderName );

    // The one binding a path adds to the surface's own set, or nothing for a path that adds none.
    // Set 0 binding 1 is the skinned path's `Bones`, binding 17 the instanced path's
    // `InstanceTransforms`; both are free in every other variant, which is what makes "same surface,
    // different path" expressible as ONE descriptor-layout relation.
    //
    // OPTIONAL and not "0 means none": binding 0 is the shared camera block in every mesh shader in the
    // engine, so a sentinel of 0 is a live slot wearing a sentinel's clothes. The first draft of this
    // returned a bare uint32_t and its own test caught it — the static path's "own binding" of 0 deleted
    // CameraUB from the comparison and made two shaders that agree look like two that do not.
    std::optional<uint32_t> MeshPathOwnBinding( MeshVertexPath path );

    // Human-readable, for logs and test failure text. Never parsed.
    const char* MeshVertexPathName( MeshVertexPath path );
    const char* MeshPassName( MeshPass pass );

    // THE PIPELINE A MESH DRAW BINDS (UE: a PSO is the MATERIAL's shader for the vertex factory x the pass). The pass
    // fixes the state — vertex layout, target, depth, polygon mode, load/clear — and names it by the pass's own
    // pipeline (@p PassState); the DRAWING material fixes the program — its template's (path x pass) cell
    // (@p CellShader, a MeshShaderFor name). The descriptor sets a draw binds come from that same cell, so a
    // pipeline chosen by the pass alone (the default template's cell) disagrees with every other template's
    // sets: the object vanishes and the validation layer counts the difference. The default surface is one entry
    // of this key like any other template, not a case of it.
    struct MeshCellPipelineKey
    {
        const void* PassState = nullptr;
        std::string CellShader;

        bool operator==( const MeshCellPipelineKey& ) const = default;
    };
    struct MeshCellPipelineKeyHash
    {
        std::size_t operator()( const MeshCellPipelineKey& key ) const;
    };

    // WHICH PROGRAM DRAWS A CASTER INTO THE SHADOW CASCADES, decided by the material's BLEND MODE (UE: the
    // shadow-depth pass takes an opaque material's position-only shader and a Masked material's own
    // FShadowDepthPS, which evaluates the OpacityMask and clips). An opaque caster's depth is its position
    // and nothing else, so every opaque caster shares ONE program — the default surface template's
    // ShadowDepth cell — and batches by mesh. A Masked caster's depth has HOLES, and only its own template's
    // ShadowDepth cell (DESERT_SURFACE_MASKED -> EvaluateSurface + DESERT_SURFACE_CLIP) knows where: drawn
    // through the shared program, a leaf card casts its whole quad. Translucent keeps the shared caster, as
    // before this existed (no translucent-shadow model yet).
    enum class ShadowCasterCell : uint8_t
    {
        Shared, // the default surface template's (path x ShadowDepth) cell, one pipeline for all casters
        Own,    // the MATERIAL'S template's (path x ShadowDepth) cell, with its textures and its clip threshold
    };
    ShadowCasterCell ShadowCasterCellFor( Core::Formats::SurfaceBlendMode blend );

    // The caster shader of a material on @p materialTemplate with @p blend, drawn on @p path:
    // MeshShaderFor on the material's template when ShadowCasterCellFor says Own, on @p defaultTemplate (the
    // template declaring `Default Surface`) when it says Shared. Empty where MeshShaderFor is.
    std::optional<std::string> ShadowCasterShaderFor( std::string_view                materialTemplate,
                                                      std::string_view                defaultTemplate,
                                                      Core::Formats::SurfaceBlendMode blend, MeshVertexPath path );
} // namespace Desert::Graphic
