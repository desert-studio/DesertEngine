#pragma once

#include <Engine/Core/Formats/DefaultTexture.hpp>
#include <Engine/Core/Formats/Shader.hpp>

#include <glm/glm.hpp>

#include <optional>
#include <string>
#include <vector>

namespace Desert::Core::Formats
{
    // ---- Program-level metadata declared in a .shader file (data-driven materials) ----
    //
    // Parsed by the ShaderPreprocessor from `#pragma param` / `#pragma state` lines and attached to the
    // compiled Shader. The Graphic layer (pipeline cache + generic Material) consumes it; the editor
    // builds the Details UI from the param list. Kept backend-neutral so Core can produce it and Graphic
    // can interpret it without a layer-inversion.
    //
    // Syntax (in the .shader, alongside `#pragma use_stage`):
    //   #pragma param float   Roughness  "Roughness"  range(0,1)  default(0.5)
    //   #pragma param color   BaseColor  "Base Color"             default(1,1,1,1)
    //   #pragma param vec3    SunDir                              default(0,1,0)
    //   #pragma param texture2D AlbedoTex "Albedo"               default("white")
    //   #pragma state cull back
    //   #pragma state depth less write on
    //   #pragma state blend off
    //   #pragma state topology patches 4

    // How a param is presented/edited. The underlying storage type is ShaderValueType; UiHint refines it
    // (e.g. a vec4 shown as a color picker vs four drag floats).
    enum class ShaderParamWidget : uint8_t
    {
        Auto = 0, // pick from value type
        Color,    // vec3/vec4 as a color swatch
        Slider    // scalar/vector with a range
    };

    // WHEN AN EDIT TO THIS PARAMETER REACHES THE PICTURE. It is the one fact about a parameter that an
    // artist cannot discover by looking at the control, and it is the fact the cloud material's window was
    // hiding: about half of that material's values are inputs to a CPU BAKE of a 256x32x256 volume over
    // several thousand cloud bodies, so moving one of them re-runs that bake — measured between 3.3 s and
    // 14.1 s on this machine — and the sky goes on showing the PREVIOUS volume until the new one lands.
    // The other half are read per sample by the march and answer in the frame that is drawn next.
    //
    // Drawn side by side with no mark the two are indistinguishable, and the owner reported the layer as
    // "not updating" twice before anybody named the difference. It belongs to the PARAMETER, so it lives in
    // the schema beside the parameter's range and category rather than in a table elsewhere that would have
    // to be kept level with the shader by hand.
    //
    // NOT the same axis as ShaderStage (vertex/fragment/compute): that says which program stage compiles
    // the code, this says which side of the CPU/GPU boundary consumes the VALUE.
    enum class ShaderParamTiming : uint8_t
    {
        Unspecified = 0, // the shader made no claim; the editor shows none rather than inventing one
        Immediate,       // read by the shader per frame — an edit is on screen the next frame
        Rebake,          // an input to a CPU precomputation — an edit re-runs it and costs seconds
    };

    // The enum's own spelling, for the DSL, the editor and diagnostics. No `default:`, so a value added
    // above is a -Wswitch warning here rather than a row that reads "2".
    constexpr const char* ShaderParamTimingName( ShaderParamTiming timing )
    {
        switch ( timing )
        {
            case ShaderParamTiming::Unspecified:
                return "Unspecified";
            case ShaderParamTiming::Immediate:
                return "Immediate";
            case ShaderParamTiming::Rebake:
                return "Rebake";
        }
        return "Unspecified";
    }

    struct ShaderParam
    {
        std::string       Name;                                  // UB field / sampler name (the binding key)
        std::string       DisplayName;                           // editor label (defaults to Name)
        std::string       Category;                              // optional Details grouping
        std::string       Tooltip;                               // editor hover text (optional)
        ShaderValueType   Type   = ShaderValueType::Float;       // numeric storage type
        ShaderParamWidget Widget = ShaderParamWidget::Auto;
        bool              IsTexture = false;                     // sampler param (uses DefaultTexture)

        // When an edit here reaches the picture — see ShaderParamTiming. Unspecified is the shipped state
        // of every shader that has not been asked the question; the editor draws no claim for it, which is
        // different from claiming "immediate".
        ShaderParamTiming Timing = ShaderParamTiming::Unspecified;

        // Non-texture ASSET reference (e.g. "CloudTypeAsset", "CloudLayoutAsset"). Empty for ordinary
        // params. Such a parameter is CPU-side only: it never becomes a GLSL declaration, so the parser
        // refuses it inside a Properties block that opted into Binding()/TextureBinding() — a reference
        // the row layout silently skipped would shift every field after it. The value lives in
        // MaterialData::Textures (name -> handle), which is a name->uint64 map and not texture-specific.
        std::string AssetKind;

        // A `TextureCube` property (IsTexture is also true). Part of the SCHEMA, not a parser detail:
        // the editor's material window offers a cube slot an HDR skybox asset can be dropped on where a
        // 2D slot takes a TextureAsset, and the cubemap preview finds the cube it must show by this flag.
        // It used to live only in a parser-local array parallel to Params ("Extras"), which is exactly
        // the two-lists-must-agree shape this codebase keeps paying for — now the param carries its own
        // dimensionality and the parser's copy is gone.
        bool IsCubeTexture = false;

        std::optional<float> Min;                                // present => slider/clamped
        std::optional<float> Max;

        glm::vec4 Default = glm::vec4( 0.0f ); // numeric default (xyzw as needed)

        // What this sampler shows when the material binds NOTHING to it — the DSL `= "white"` on a
        // Texture2D property. Read by Graphic::Material::BindSchemaDefaultTexture, which is what makes
        // "clear this slot" a thing a material can express at all: the alternative is a slot the file
        // says is empty and the descriptor still points at the last texture somebody assigned.
        // Meaningless (and left at White) for a non-texture param; see DefaultTexture.hpp for why the
        // implicit value reproduces the old picture rather than choosing a new one.
        DefaultTextureKind DefaultTexture = DefaultTextureKind::White;

        bool IsAssetRef() const
        {
            return !AssetKind.empty();
        }

        // Field-wise, so the shader map's round-trip test can ask "is the cached meta the parsed meta".
        bool operator==( const ShaderParam& ) const = default;
    };

    // ---- Render state (maps to GraphicsPipelineSpecification in the Graphic layer's pipeline cache) ----
    // std::nullopt = "unspecified" -> the renderer/pipeline-cache falls back to its default.

    enum class StateCull : uint8_t  { None, Front, Back, FrontAndBack };
    enum class StateCompare : uint8_t { Never, Less, Equal, LessOrEqual, Greater, NotEqual, GreaterOrEqual, Always };
    enum class StateTopology : uint8_t { Triangles, Lines, Points, Patches };
    enum class StateBlendFactor : uint8_t
    {
        Zero, One, SrcColor, OneMinusSrcColor, DstColor, OneMinusDstColor,
        SrcAlpha, OneMinusSrcAlpha, DstAlpha, OneMinusDstAlpha
    };
    enum class StateStencilOp : uint8_t
    {
        Keep, Zero, Replace, IncrementClamp, DecrementClamp, Invert, IncrementWrap, DecrementWrap
    };

    struct ShaderRenderState
    {
        std::optional<StateCull>     Cull;
        std::optional<bool>          DepthTest;
        std::optional<bool>          DepthWrite;
        std::optional<StateCompare>  DepthCompare;
        std::optional<bool>          Blend;
        std::optional<StateTopology> Topology;
        std::optional<uint32_t>      PatchControlPoints; // only meaningful for Patches topology

        // Custom blend factors (only meaningful when Blend is on). Both set => custom; otherwise the
        // renderer's standard src-alpha / one-minus-src-alpha blend. DSL: `Blend SrcAlpha OneMinusSrcAlpha`.
        std::optional<StateBlendFactor> BlendSrc;
        std::optional<StateBlendFactor> BlendDst;

        // Stencil test. DSL: `Stencil <compare> <ref> [<fail> <pass> <depthFail>]`
        // (default ops Keep/Replace/Keep; read/write masks 0xFF). std::nullopt = no stencil (default).
        std::optional<bool>           StencilTest;
        std::optional<StateCompare>   StencilCompare;
        std::optional<uint32_t>       StencilRef;
        std::optional<StateStencilOp> StencilFail;
        std::optional<StateStencilOp> StencilPass;
        std::optional<StateStencilOp> StencilDepthFail;

        bool operator==( const ShaderRenderState& ) const = default;
    };

    // Where a shader may be used (mirrors UE's Material Domain). Drives the editor's material shader
    // picker: only "assignable" domains (Surface, Terrain) are offered for a MaterialComponent; engine
    // shaders (skybox, post-process, shadow…) stay out of the list. A shader with no `#pragma domain` is
    // Unspecified -> treated as internal (not offered).
    enum class ShaderDomain : uint8_t
    {
        Unspecified = 0, // no #pragma domain -> internal, not user-assignable
        Surface,         // lit/unlit surface materials on meshes
        Terrain,         // tessellated terrain materials
        Skybox,
        PostProcess,
        Volume, // participating media marched by a compute pass (the volumetric cloud layer)
        UI      // the fill of a 2D UI element, rasterized by the Render2D batcher
    };

    // The enum's own spelling, for diagnostics. It lives beside the enum so a domain added above cannot
    // leave a log line reading "3": this switch has no default, so growing the enum is a -Wswitch warning
    // and the build is warning-clean.
    //
    // NOT the same function as the editor's DomainName() (MaterialEditorPanel.cpp), which is deliberately
    // separate artist-facing copy -- "engine-internal" reads better than "Unspecified" beside a material's
    // name in a window, whereas a log wants the token the `.shader` file actually writes so the message can
    // be grepped straight back to the file that caused it.
    constexpr const char* ShaderDomainName( ShaderDomain domain )
    {
        switch ( domain )
        {
            case ShaderDomain::Unspecified:
                return "Unspecified";
            case ShaderDomain::Surface:
                return "Surface";
            case ShaderDomain::Terrain:
                return "Terrain";
            case ShaderDomain::Skybox:
                return "Skybox";
            case ShaderDomain::PostProcess:
                return "PostProcess";
            case ShaderDomain::Volume:
                return "Volume";
            case ShaderDomain::UI:
                return "UI";
        }
        return "Unspecified";
    }

    // ---- Which draw path may execute a domain ----
    //
    // These two are the domain's actual MEANING, and they are separate on purpose. A domain is not a label
    // a material carries around; it is the name of the one renderer that knows how to feed that shader.
    //
    // THE TRAP THEY EXIST TO CLOSE. IsUserAssignable() below is their UNION, and a draw path that asks the
    // union instead of its own half accepts a material it cannot execute. That shipped: a `.demat` naming
    // the `Terrain` shader, placed in a StaticMeshComponent slot, drew with a pipeline belonging to a
    // different renderer and produced nothing at all -- no log, no refusal, no validation error -- because
    // the geometry and the uniform blocks the mesh path supplies are not the ones a terrain shader reads.
    // MeshRenderer::DrawGenericMeshes asks DrawnByMeshPath() and refuses by name.
    //
    // So: when a new domain is added, it gets a predicate here and a path that answers to it, or it is not
    // user-assignable. There is no third option in which a user may pick it and nothing draws it.

    // The domain each path rasterizes, named rather than spelled inside the predicates: a refusal has to
    // print the domain it drew AND the domain it wanted, and those two strings must come from the same
    // place the comparison does or the message can describe a rule the code is not applying.
    inline constexpr ShaderDomain kMeshPathDomain    = ShaderDomain::Surface;
    inline constexpr ShaderDomain kTerrainPathDomain = ShaderDomain::Terrain;
    inline constexpr ShaderDomain kVolumePathDomain  = ShaderDomain::Volume;
    inline constexpr ShaderDomain kUIPathDomain      = ShaderDomain::UI;

    constexpr bool DrawnByMeshPath( ShaderDomain domain )
    {
        return domain == kMeshPathDomain;
    }

    constexpr bool DrawnByTerrainPath( ShaderDomain domain )
    {
        return domain == kTerrainPathDomain;
    }

    // The volume path is VolumetricCloudRenderer: a compute march, not a rasterized draw. Its material
    // slot lives on VolumetricCloudComponent (ECS), never on a mesh — MeshRenderer asks DrawnByMeshPath
    // and refuses a Volume material by name, exactly as it refuses a Terrain one.
    constexpr bool DrawnByVolumePath( ShaderDomain domain )
    {
        return domain == kVolumePathDomain;
    }

    // The UI path is Graphic::Render2D — a screen-space quad in the 2D batcher, fed the pixel->clip
    // projection and this element's row of `Materials[]`, and nothing else. It receives no camera, no
    // world position and no normal, so a Surface shader placed in a UI slot would read uniform blocks
    // that path never binds: the UI slot asks THIS predicate and draws the error material by name when
    // it is false, which is the refusal UE never wrote (its own source still carries
    // `//TODO UMG Check if the material can be used with the UI`, and a wrong-domain UMG material
    // renders silently nothing).
    constexpr bool DrawnByUIPath( ShaderDomain domain )
    {
        return domain == kUIPathDomain;
    }

    struct ShaderProgramMeta
    {
        std::vector<ShaderParam> Params;
        ShaderRenderState        State;
        ShaderDomain             Domain = ShaderDomain::Unspecified;

        // Additional named passes declared by the shader (DSL `Pass "Name" { ... }` blocks).
        // Each is a separate program registered in the ShaderService as "<Shader>/<Pass>";
        // empty for legacy single-program shaders.
        std::vector<std::string> PassNames;

        // THE BODY OF A `Medium { ... }` BLOCK — a PROGRAM FRAGMENT, not a stage.
        //
        // A Volume-domain shader may declare no stages at all and carry only this: the authored cloud
        // medium, which is compiled INTO four other programs as the substitution for one of their includes
        // (Engine/Core/ShaderCompiler/ShaderVariant.hpp). It is a `.shader` rather than a loose `.glslh`
        // for one reason and it is the deciding one: a `.shader` is an ASSET with a handle, so a material
        // can point at it, the content browser can show it, and hot reload already watches it. A `.dgraph`
        // could not — it has no identity at all — and inventing an asset type for the fragment would have
        // meant a second document editor for something the graph already produces.
        //
        // Empty for every ordinary program, which is what makes IsMediumProgram() a fact about the file
        // rather than a convention.
        std::string MediumSource;

        // A program fragment and nothing else: no stages of its own, so it compiles to no modules and is
        // never used to build a pipeline. ShaderService registers it by name without complaining that it
        // has no compiled stages — the complaint is right for every other shader and wrong for this one.
        bool IsMediumProgram() const
        {
            return !MediumSource.empty();
        }

        bool HasParams() const
        {
            return !Params.empty();
        }

        // Can a user assign this shader to a renderable via a MaterialComponent?
        //
        // DERIVED from the per-path predicates rather than restating the list, so the two cannot drift:
        // "a user may assign it" means exactly "some path draws it". Restated by hand, this is the line
        // that would keep offering a domain after its renderer stopped answering for it -- and a draw path
        // must still ask its OWN predicate, never this one (see the note above DrawnByMeshPath).
        bool IsUserAssignable() const
        {
            return DrawnByMeshPath( Domain ) || DrawnByTerrainPath( Domain ) || DrawnByVolumePath( Domain ) ||
                   DrawnByUIPath( Domain );
        }

        bool operator==( const ShaderProgramMeta& ) const = default;
    };

} // namespace Desert::Core::Formats
