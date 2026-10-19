#pragma once

// Desert Shader Language (DSL) — a single-file, ShaderLab-style shader format.
//
// A .shader file in this format holds everything a shader program needs: editor-facing
// properties, render state, and the GLSL for every stage. Example:
//
//     Shader "Unlit"
//     {
//         Domain Surface
//
//         // Binding(n) opts into auto-generation of the MaterialUB uniform block from the
//         // numeric properties; TextureBinding(m) assigns sequential bindings to the texture
//         // properties. Omit both to declare GLSL resources by hand (metadata-only mode).
//         Properties Binding(1) TextureBinding(2)
//         {
//             Color     Color       ("Color")                    = (0.8, 0.4, 0.1, 1)
//             Float     Tiling      ("Tiling", Range(0.25, 64))  = 4
//             Texture2D u_AlbedoTex ("Albedo")                   = "white"
//         }
//
//         State
//         {
//             Cull Back        // None | Front | Back | FrontAndBack
//             ZTest LEqual     // Never|Less|Equal|LEqual|Greater|NotEqual|GEqual|Always | Off
//             ZWrite On
//             Blend Off        // Off | On | Alpha (alias of On)
//             Topology Triangles   // Triangles | Lines | Points | Patches <n>
//         }
//
//         Include  { /* GLSL inserted into every stage (after the generated header) */ }
//
//         Vertex   { /* GLSL */ }
//         Fragment { /* GLSL */ }
//         // Also: Compute, TessControl, TessEval
//
//         // Additional named passes (e.g. a depth-only shadow variant). Each pass has its own
//         // stages and may override individual State settings; everything else (Properties,
//         // Include, the auto-generated resources) is shared with the whole file. A pass is a
//         // separate shader program, addressable as "<ShaderName>/<PassName>".
//         Pass "Shadow"
//         {
//             State  { Cull Front }
//             Vertex { /* GLSL */ }
//         }
//     }
//
// The parser emits ShaderProgramMeta + per-stage GLSL, the only shader form the engine reads
// (the legacy `#pragma` format is refused by ShaderPreprocess), so everything downstream — shaderc, SPIR-V
// reflection, DataDrivenMaterial, the material UI — works unchanged. Emitted GLSL contains
// #line directives so shaderc errors point at the original .shader lines.

#include <Engine/Core/Formats/Shader.hpp>
#include <Engine/Core/Formats/MaterialLayout.hpp>
#include <Engine/Core/Formats/ShaderProgramMeta.hpp>
#include <Engine/Core/ShaderCompiler/ShadingModels/ShadingModelManifest.hpp>
#include <Common/Core/ResultStr.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Desert::Core::Preprocess
{
    struct DShaderPass
    {
        std::string                                                 Name;  // "" for the default pass
        Core::Formats::ShaderRenderState                            State; // file-level State + pass overrides
        std::unordered_map<Core::Formats::ShaderStage, std::string> Stages;
    };

    // ─── Surface templates ──────────────────────────────────────────────────────────────────────────
    // A `Domain Surface` template may carry a `Surface { SurfaceOutput EvaluateSurface( SurfaceInput ) }`
    // block instead of stage blocks. The parser expands it into one CELL per (vertex path × pass), named
    // `<Path>.<Pass>`, each a named pass of the program: the engine's vertex header for the path, the
    // engine's pass header for the pass and the author's surface function between them. These tables are
    // the one home of the cell set and of the headers a cell is built from — the parser writes the cells
    // from them and ComputeShaderMapKey hashes the same headers, so the two cannot disagree.
    inline constexpr std::array<std::string_view, 3> kSurfaceVertexPaths = { "Static", "Instanced", "Skinned" };
    inline constexpr std::array<std::string_view, 4> kSurfaceCellPasses   = { "Forward", "GBuffer", "ShadowDepth",
                                                                              "VirtualShadowDepth" };
    inline constexpr std::string_view                kSurfaceTypesInclude = "Mesh/Surface/SurfaceTypes.glslh";
    // THE PARALLAX PERMUTATION (UE: the static permutation of a material whose ParallaxOcclusionMapping node and
    // Pixel Depth Offset pin are wired). A template that declares `Height <Texture2D> ... Scale(<Float>)` gets every mesh cell a
    // second time, named `<Path>.<Pass>.Parallax`, after all its other cells; only those define
    // kSurfaceParallaxDefine (Mesh/Surface/SurfaceParallax.glslh: the march, the depth write, a caster that
    // evaluates the surface). A material draws them only when it assigns the named texture
    // (Graphic::ParallaxCellOf), so one without a height map pays nothing.
    inline constexpr std::string_view kSurfaceParallaxCellSuffix = ".Parallax";
    inline constexpr std::string_view kSurfaceParallaxDefine     = "DESERT_SURFACE_PARALLAX";
    inline constexpr std::string_view kSurfaceParallaxInclude    = "Mesh/Surface/SurfaceParallax.glslh";
    // THE PARTICLE SPRITE USAGE (UE: a material's bUsedWithParticleSprites). A template that declares
    // `Usage ParticleSprites` gets ONE more cell, `ParticleSprite.Forward`, after the mesh cells: the sprite
    // vertex path (Vertex_ParticleSprite.glslh, the particle's colour / relative time / random into
    // SurfaceInput.Particle) and the sprite pass header (depth-tested, unlit, Depth Fade over the scene depth). No
    // other pass: a sprite is drawn by the particle pass alone. Both stages of the cell get
    // kSurfaceParticleSpriteDefine.
    inline constexpr std::string_view kSurfaceParticleSpritePath        = "ParticleSprite";
    inline constexpr std::string_view kSurfaceParticleSpritePass        = "Forward";
    inline constexpr std::string_view kSurfaceParticleSpritePassInclude = "Mesh/Surface/Pass_ParticleSprite.glslh";
    inline constexpr std::string_view kSurfaceParticleSpriteDefine      = "DESERT_SURFACE_PARTICLE_SPRITE";
    // THE RAY TRACING HIT CELL (UE: the material's hit group). Every Opaque / Masked template whose bindings are
    // all row slots gets `RayTracing.Hit` after its mesh cells: ClosestHit (the surface into the ray's payload)
    // and, Masked, AnyHit (the OpacityMask test). Its stages define DESERT_SURFACE_PASS_Hit; their material row
    // comes from the hit group's shader record (kSurfaceRayHitRecordInclude), the geometry from the same record.
    inline constexpr std::string_view kSurfaceRayHitPath          = "RayTracing";
    inline constexpr std::string_view kSurfaceRayHitPass          = "Hit";
    inline constexpr std::string_view kSurfaceRayHitPassInclude   = "Mesh/Surface/Pass_RayHit.glslh";
    inline constexpr std::string_view kSurfaceRayHitRecordInclude = "Common/RayTracingHitRecord.glslh";
    // THE GRASS BLADE USAGE (GRASS-BLADE; UE has no such vertex factory). A template that declares
    // `Usage GrassBlades` gets the mesh passes once more on the blade path, `GrassBlade.<Pass>` for every pass of
    // kSurfaceCellPasses its blend draws, after the mesh (and sprite) cells: the vertex stage is
    // Vertex_GrassBlade.glslh (a procedural blade placed by the instance's matrix and shaped by the variety's
    // GrassBladeShape), the pass headers are the meshes'.
    inline constexpr std::string_view kSurfaceGrassBladePath = "GrassBlade";
    // THE TRANSLUCENT FRONT LAYER CELL (UE: EMeshPass::FrontLayerTranslucencyGBuffer, FrontLayerTranslucency.usf
    // MainPS). Every `BlendMode Translucent` template gets `Static.FrontLayer` after its Forward cells: the static
    // vertex path and Mesh/Surface/Pass_FrontLayer.glslh, which writes the surface's world normal + roughness and
    // its motion, depth-tested and depth-written, so the nearest translucent surface of each pixel is left in the
    // front layer's G-buffer for the reflection trace (Reflections.TranslucentFrontLayer). Additive templates add
    // light and reflect nothing: no cell.
    inline constexpr std::string_view kSurfaceFrontLayerPath        = "Static";
    inline constexpr std::string_view kSurfaceFrontLayerPass        = "FrontLayer";
    inline constexpr std::string_view kSurfaceFrontLayerPassInclude = "Mesh/Surface/Pass_FrontLayer.glslh";
    // Defined in both stages of every cell of a `BlendMode Translucent` template (the sprite pass blends on it).
    inline constexpr std::string_view kSurfaceTranslucentDefine = "DESERT_SURFACE_TRANSLUCENT";
    // Defined in every cell of a `ResponsiveAA` template (UE: the material's bResponsiveAA): the translucent pass
    // headers then write the coverage they composite into the scene target's responsive-AA slot (location 2,
    // Graphic::kSceneTargetResponsiveAASlot), and the temporal resolve trusts the current frame more there.
    inline constexpr std::string_view kSurfaceResponsiveAADefine = "DESERT_SURFACE_RESPONSIVE_AA";
    // A template has no program of its own besides its cells, so its DEFAULT program (the empty pass name,
    // what the boot content compiles and what a lookup by shader name returns) is this one cell, by name.
    inline constexpr std::string_view kSurfaceDefaultCell = "Static.Forward";
    // THE DEPTH-NORMAL ENTRY OF A TEMPLATE WITHOUT A SURFACE BLOCK (Graphic::MeshPass::DepthNormal). Such a
    // template is hand-written, so the parser cannot derive the pass from a surface function: an OPAQUE
    // mesh-domain one declares it as a named pass of this name, writing reversed-Z depth and the world normal
    // into the G-buffer's normal slot (Mesh/Generic/DepthNormal.glslh; every other colour slot unwritten, so
    // its write mask is 0 - VulkanShaderReflection BuildColorBlendAttachments). A `BlendMode Masked` template
    // discards there exactly as its default program does. The deferred path draws it in "Deferred: Forward
    // Depth/Normal" so the G-buffer's screen-space readers see the forward-shaded mesh (UE: the depth prepass).
    inline constexpr std::string_view kGenericDepthNormalPass = "DepthNormal";
    // THE DEFAULT PROGRAM AND THE DEFAULT CELL ARE ONE PROGRAM (UE: one FMaterialShaderMap entry per cell, not a
    // copy for the lookup by name). Every consumer that enumerates a template's cells — ShaderService's
    // registration, the boot content's shader maps, the package cook, the shader map key — compiles the default
    // program once and answers "<Template>/<kSurfaceDefaultCell>" with it. @p isSurfaceTemplate is the caller's
    // knowledge of the program (a parse's Surface.Cells, or MayDeclareSurface on its text).
    constexpr bool IsSurfaceDefaultCell( const bool isSurfaceTemplate, const std::string_view passName )
    {
        return isSurfaceTemplate && passName == kSurfaceDefaultCell;
    }
    // The same rule from a parse's metadata, for EVERY program: a surface template's default cell and the first
    // named pass of a shader without top-level stages (ShaderProgramMeta::DefaultPassName). Registration, the boot
    // shader maps, the package cook and the pass metadata use this one; IsSurfaceDefaultCell remains for the
    // callers that hold only the program's text (the shader map key).
    inline bool IsDefaultProgramPass( const Formats::ShaderProgramMeta& meta, const std::string_view passName )
    {
        return !meta.DefaultPassName.empty() && passName == meta.DefaultPassName;
    }
    // The pass whose opaque cells never evaluate the surface: their fragment stage is the pass header alone (no
    // surface function, no material row, no push block), so the cell's layout is the shadow shader's.
    inline constexpr std::string_view kSurfaceDepthPass = "ShadowDepth";
    // The virtual shadow map's caster pass (UE: the VSM page raster, a non-Nanite mesh's shadow-depth shader
    // writing the physical pool): the same depth-only cell, its fragment stage writing the pool by an atomic max
    // instead of a depth attachment (Mesh/Surface/Pass_VirtualShadowDepth.glslh).
    inline constexpr std::string_view kSurfaceVirtualDepthPass = "VirtualShadowDepth";
    constexpr bool                    IsSurfaceDepthOnlyPass( const std::string_view pass )
    {
        return pass == kSurfaceDepthPass || pass == kSurfaceVirtualDepthPass;
    }

    // The template's blend mode lives on the program metadata (Core/Formats/ShaderProgramMeta.hpp), where the
    // renderer reads it. Masked: the pass headers discard below u_Material.<kSurfaceMaskClipParam>. Translucent:
    // the template has Forward cells only, built on the translucency pass header (kSurfaceTranslucentPassInclude).
    using SurfaceBlendMode = Formats::SurfaceBlendMode;

    // The shading model of a template, as UE's MaterialShadingModel: a property of the TEMPLATE, named by
    // `ShadingModel <Name>` (a file ShadingModels/<Name>.shadingmodel) and carried as the model's Guid; a template
    // without the directive is DefaultLit. Every model is lit by the same pass headers: each cell's both stages
    // get `#define DESERT_SHADING_MODEL_INDEX SHADING_MODEL_INDEX_<NAME>` (as UE's MATERIAL_SHADINGMODEL_*), and
    // the headers hand it to the generated dispatch (ShadingModels/ShadingModelRegistry.hpp). A template that does
    // not assign one of its model's Inputs in the surface function is refused, naming the field.
    inline constexpr std::string_view kSurfaceShadingModelDefine = "DESERT_SHADING_MODEL_INDEX";
    // Defined in the fragment stage of a cell that evaluates the surface (not in an opaque depth cell, never in a
    // vertex stage): Mesh/Surface/SurfaceTypes.glslh then declares the material fetch, which carries the view's
    // mip bias (CameraUB MaterialMipBias).
    inline constexpr std::string_view kSurfaceSamplesMaterialDefine = "DESERT_SURFACE_SAMPLES_MATERIAL";

    // The threshold of a Masked template is a material PARAMETER (per material, like UE's Opacity Mask Clip
    // Value); a Masked template that does not declare it is refused.
    inline constexpr std::string_view kSurfaceMaskClipParam = "OpacityMaskClipValue";

    std::string SurfaceCellName( std::string_view path, std::string_view pass );
    std::string SurfaceVertexInclude( std::string_view path );
    std::string SurfacePassInclude( std::string_view pass, SurfaceBlendMode blend );
    // The translucency pass header: a Translucent template's Forward cells are lit and composited by it.
    inline constexpr std::string_view kSurfaceTranslucentPassInclude =
         "Mesh/Surface/Pass_Forward_Translucent.glslh";
    // Whether a template of this blend mode has a cell in @p pass: a Translucent surface is drawn by the
    // translucency pass alone (UE: no depth or base-pass G-buffer write), so it has Forward cells only.
    bool SurfaceBlendHasPass( SurfaceBlendMode blend, std::string_view pass );
    // Every engine header any cell of a surface template compiles: the key hashes all of them.
    std::vector<std::string> SurfaceTemplateIncludes();

    // What the `Surface` block declared; empty Cells = not a surface template.
    struct SurfaceTemplateInfo
    {
        bool                     TwoSided = false;
        SurfaceBlendMode         Blend    = SurfaceBlendMode::Opaque;
        bool UsedWithParticleSprites      = false; // `Usage ParticleSprites`: + ParticleSprite.Forward
        bool                     UsedWithGrassBlades = false;          // `Usage GrassBlades`: + GrassBlade.<Pass>
        // `ResponsiveAA` (UE bResponsiveAA): a Translucent / Additive template only — an opaque surface draws into
        // the G-buffer, which has no responsive slot, and writes the velocity TAA reprojects it with.
        bool ResponsiveAA = false;
        Common::UUID             ShadingModel{ ShadingModels::kDefaultLitGuid };
        std::vector<std::string> Cells; // `<Path>.<Pass>`, in kSurfaceVertexPaths × kSurfaceCellPasses order
        // `RayTracing.Hit` when the template has its hit cell (also in Cells); otherwise empty, and RayHitRefusal
        // says why for an Opaque / Masked template (empty for a translucent one, which has no hit group by
        // design).
        std::string RayHitCell;
        std::string RayHitRefusal;
    };

    struct DShaderParseResult
    {
        std::string                                                 Name;
        Core::Formats::ShaderProgramMeta                            Meta;   // default pass meta (+ PassNames)
        std::unordered_map<Core::Formats::ShaderStage, std::string> Stages; // default pass GLSL
        std::vector<DShaderPass>                                    Passes; // all passes; [0] is the default
        // The row and texture layout the generated GLSL was written from (Core/Formats/MaterialLayout.hpp);
        // its push fields stay empty until ReconcileMaterialLayout reads them off a compiled cell.
        Core::Formats::MaterialLayout Layout;
        SurfaceTemplateInfo           Surface; // the expanded `Surface` block; Cells empty for any other shader

        // nullptr when the pass doesn't exist. Empty name = the default pass; for a surface template that is
        // the cell named kSurfaceDefaultCell (the parser refuses a template without it) — by name, never "the
        // first pass", so the default program and its lookup are the same cell by construction.
        const DShaderPass* FindPass( const std::string& name ) const
        {
            // Meta.DefaultPassName names the pass the default program IS (a surface template's default cell, or
            // the first named pass of a shader with no top-level stages); empty when the default program is the
            // shader's own top-level stage set.
            const std::string_view wanted = !name.empty() ? std::string_view( name )
                                            : !Meta.DefaultPassName.empty()
                                                 ? std::string_view( Meta.DefaultPassName )
                                            : !Surface.Cells.empty() ? kSurfaceDefaultCell
                                                                     : std::string_view( name );
            for ( const auto& p : Passes )
                if ( p.Name == wanted )
                    return &p;
            return nullptr;
        }
    };

    // Headers the parser writes into a stage that the authored text never names. A key computed over the
    // AUTHORED text (the shader map's, see ShaderCacheKey.hpp) must hash these as well, or editing one would
    // leave every material program on its old binary.
    inline constexpr std::array<std::string_view, 2> kParserInjectedIncludes = { "Common/MaterialTransport.glslh",
                                                                                 "Common/BindlessTextures.glslh" };

    class DShaderParser
    {
    public:
        // True when the source is written in the DSL (first meaningful token is `Shader`); false for
        // anything else, which ShaderPreprocess refuses as "not a DSL shader".
        static bool IsDShader( const std::string& source );

        // Errors carry the source line: `DShader parse error (line N): message`.
        static Common::ResultStr<DShaderParseResult> Parse( const std::string& source );

        // Translates the Desert layout sugar (In/Out/Uniform/Buffer/PushConstant/LocalSize) to plain GLSL,
        // line-for-line. Used both when assembling stage code AND by the #include resolver, so shared
        // `.glslh` headers can use the same sugar. A no-op on source that has none.
        static std::string TranslateSugar( const std::string& source );

        // False only when Parse could not produce a Medium block: the block keyword is matched without
        // regard to case, so a source that does not contain "medium" in any case has none. A true is
        // "parse to find out". ShaderService asks this of every shader it registers, and the answer is
        // what lets a warm start register a program without parsing its text at all.
        static bool MayDeclareMedium( std::string_view source );

        // False only when the text certainly declares no `Domain Particle` (any case, any whitespace between
        // the two words) — and so no Particle fragment, which is legal only under that line. Boot skips a
        // fragment from the program prebuild on this answer; a false positive only builds a map later.
        static bool MayDeclareParticle( std::string_view source );

        // Cheap precheck for the shader map key, which must not parse: false = the text certainly declares
        // no `Surface { ... }` block. A false positive only hashes the surface headers needlessly.
        static bool MayDeclareSurface( std::string_view source );
    };
} // namespace Desert::Core::Preprocess
