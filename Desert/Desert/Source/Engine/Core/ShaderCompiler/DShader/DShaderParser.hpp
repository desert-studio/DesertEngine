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
// The parser emits the same structures the legacy `#pragma` format produces
// (ShaderProgramMeta + per-stage GLSL), so everything downstream — shaderc, SPIR-V
// reflection, DataDrivenMaterial, the material UI — works unchanged. Emitted GLSL contains
// #line directives so shaderc errors point at the original .shader lines.

#include <Engine/Core/Formats/Shader.hpp>
#include <Engine/Core/Formats/MaterialLayout.hpp>
#include <Engine/Core/Formats/ShaderProgramMeta.hpp>
#include <Common/Core/ResultStr.hpp>

#include <array>
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
    inline constexpr std::array<std::string_view, 3> kSurfaceCellPasses  = { "Forward", "GBuffer", "ShadowDepth" };
    inline constexpr std::string_view                kSurfaceTypesInclude = "Mesh/Surface/SurfaceTypes.glslh";
    // A template has no program of its own besides its cells, so its DEFAULT program (the empty pass name,
    // what the boot content compiles and what a lookup by shader name returns) is this one cell, by name.
    inline constexpr std::string_view kSurfaceDefaultCell = "Static.Forward";
    // The pass whose opaque cells never evaluate the surface: their fragment stage is the pass header alone (no
    // surface function, no material row, no push block), so the cell's layout is the shadow shader's.
    inline constexpr std::string_view kSurfaceDepthPass = "ShadowDepth";

    enum class SurfaceBlendMode
    {
        Opaque,
        Masked, // the pass headers discard below u_Material.<kSurfaceMaskClipParam>
    };

    // The threshold of a Masked template is a material PARAMETER (per material, like UE's Opacity Mask Clip
    // Value); a Masked template that does not declare it is refused.
    inline constexpr std::string_view kSurfaceMaskClipParam = "OpacityMaskClipValue";

    std::string SurfaceCellName( std::string_view path, std::string_view pass );
    std::string SurfaceVertexInclude( std::string_view path );
    std::string SurfacePassInclude( std::string_view pass );
    // Every engine header any cell of a surface template compiles: the key hashes all of them.
    std::vector<std::string> SurfaceTemplateIncludes();

    // What the `Surface` block declared; empty Cells = not a surface template.
    struct SurfaceTemplateInfo
    {
        bool                     TwoSided = false;
        SurfaceBlendMode         Blend    = SurfaceBlendMode::Opaque;
        std::vector<std::string> Cells; // `<Path>.<Pass>`, in kSurfaceVertexPaths × kSurfaceCellPasses order
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

        // nullptr when the pass doesn't exist. Empty name = the default pass.
        const DShaderPass* FindPass( const std::string& name ) const
        {
            if ( name.empty() && !Surface.Cells.empty() )
                return FindPass( std::string( kSurfaceDefaultCell ) );
            for ( const auto& p : Passes )
                if ( p.Name == name )
                    return &p;
            return nullptr;
        }
    };

    // Headers the parser writes into a stage that the authored text never names. A key computed over the
    // AUTHORED text (the shader map's, see ShaderCacheKey.hpp) must hash these as well, or editing one would
    // leave every material program on its old binary.
    inline constexpr std::array<std::string_view, 1> kParserInjectedIncludes = {
         "Common/MaterialTransport.glslh" };

    class DShaderParser
    {
    public:
        // True when the source is written in the DSL (first meaningful token is `Shader`);
        // false for the legacy `#pragma use_stage` format.
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

        // Cheap precheck for the shader map key, which must not parse: false = the text certainly declares
        // no `Surface { ... }` block. A false positive only hashes the surface headers needlessly.
        static bool MayDeclareSurface( std::string_view source );
    };
} // namespace Desert::Core::Preprocess
