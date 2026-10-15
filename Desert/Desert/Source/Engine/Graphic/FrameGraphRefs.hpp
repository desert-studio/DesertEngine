#pragma once

#include <Engine/Graphic/RDG/RDGResources.hpp>
#include <Engine/Graphic/RDG/RDGSystemTextures.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/Shader.hpp>

#include <algorithm>
#include <array>
#include <optional>
#include <string_view>
#include <vector>

namespace Desert::Graphic
{
    // RDG-A2. The transients of THIS frame graph that one renderer's passes produce and a LATER renderer's passes
    // read (UE: FSceneTextures / the RDG texture fields passed between AddPass helpers). The producer's
    // AddFrame* creates the texture with Builder::CreateTexture (desc from this frame's view) and stores the ref
    // here; the consumer's AddFrame* declares a read of it and binds it with RDG::PassBindings. An invalid ref
    // means the producer did not run this frame (effect off, culled input): the consumer declares a read of
    // FrameTextures::System.Black, binds it at that slot and writes zero intensity in its uniform values - an
    // explicit choice at the call site (UE: FRDGSystemTextures::Black), never a stale image.
    // Only cross-renderer transients live here; a transient read only by its own renderer's passes (SMAA edges,
    // JFA ping-pong, SSR trace/tiles, cloud trace/guide, the exposure histogram) stays a local of that
    // AddFrame*. A history (read in a LATER frame) is never here: it is an external the renderer owns.
    // The struct is rebuilt with every graph and dies with it.
    // The shadow cascades a lit pass samples (u_ShadowMap0..3); SceneResources::kMaxCascades is asserted equal
    // where both are visible (MeshRendererInternal.hpp).
    inline constexpr uint32_t kSceneViewShadowCascades = 4;

    struct FrameTransients
    {
        RDG::TextureRef Bloom;          // BloomRenderer chain (mip 0 read)   -> Tonemap
        RDG::TextureRef LightShafts;    // LightShaftRenderer result          -> Tonemap
        RDG::TextureRef LensFlare;      // LensFlareRenderer result           -> Tonemap
        RDG::TextureRef SSAO;           // SSAO factor                        -> deferred lighting
        RDG::TextureRef GIResolve;      // RSM-GI resolve (raw, pre-temporal) -> GI temporal / deferred lighting
        RDG::TextureRef SceneColorCopy; // scene snapshot                     -> glass refraction
        RDG::TextureRef BackdropBlur;   // BackdropBlurRenderer pyramid (all mips) -> UI glass (Render2D)
        RDG::TextureRef HeightFog;      // HeightFogRenderer evaluation (RGBA16F) -> HeightFogApply
        // The clouds' reconstruction pair written this frame (VolumetricCloudRenderer::GetFrameResult, imported:
        // the ping-pong is the renderer's own); invalid when the frame has none   -> CloudComposite
        RDG::TextureRef CloudScatter;
        RDG::TextureRef CloudGuide;
        // The cloud layer's shadow map (VolumetricCloudRenderer::GetShadowMap, imported: the renderer owns it),
        // set once its shadow node was accepted; invalid when the frame has none   -> Deferred: Composite
        RDG::TextureRef CloudShadowMap;
        // The view's velocity (ViewRasterTargets.hpp CreateViewVelocity): one per view graph, the colour slot
        // every depth-writing node on the scene target / G-buffer writes, never on a light view   -> TAA
        RDG::TextureRef Velocity;
        // The view's responsive-AA mask (ViewRasterTargets.hpp CreateViewResponsiveAA): a colour slot of the scene
        // target only, written by ResponsiveAA translucency (glass, particles)   -> TAA
        RDG::TextureRef ResponsiveAA;
        // The scene/view inputs every lit pass samples (UE: the view uniform buffer's shadow / sky /
        // PreIntegratedGF textures), imported once per graph by SceneRenderer::ImportSceneViewTextures before any
        // node is added: cascade c (MeshRenderer's CSM map, invalid past the valid count), the scene environment's
        // two cubes (invalid when the scene has no baked sky) and the renderer's split-sum BRDF LUT. Never read
        // directly: a pass takes SceneViewInputsOf, which states the neutral texture for each one that is absent.
        std::array<RDG::TextureRef, kSceneViewShadowCascades> ShadowCascades;
        // The view's local-light shadow atlas (MeshRenderer::GetLocalShadowAtlasImage, imported: the renderer owns
        // it and keeps cached tiles in it across frames). Invalid while no light of the view has cast a shadow.
        RDG::TextureRef                                       LocalShadowAtlas;
        RDG::TextureRef                                       EnvIrradiance;
        RDG::TextureRef                                       EnvSpecular;
        RDG::TextureRef                                       BrdfLut;
        // The view's reflection captures (ReflectionCaptureRenderer::AddPasses, every graph): the cube array and
        // the capture list the lighting pass reads (Common/ReflectionEnvironment.glslh)   -> Deferred: Composite
        RDG::TextureRef ReflectionCaptureCubes;
        RDG::BufferRef  ReflectionCaptures;
        // The procedural sky's transmittance / sky-view LUTs (SkyboxRenderer, imported under the names its LUT
        // nodes use), set only when SkyboxRenderer::SkyPassSamplesLuts; invalid otherwise   -> SkyboxPass
        RDG::TextureRef SkyTransmittanceLut;
        RDG::TextureRef SkyViewLut;
    };

    // RDG-A2 (owner decision 2, 2026-10-05). What EVERY node of the frame graph is handed, whatever registered
    // it (an AddFrame* lambda, a system's SystemRasterPass, a system's ComputeNodeDeclaration, an editor
    // ExtensionPass): this frame's cross-renderer transients and the engine's system textures, as
    // refs of THIS graph. Its declaration callback gets it to name what it reads (RenderPassDeclaration::Read of
    // a TextureRef), its body gets it with the node's RDG::PassContext to bind those refs by shader name
    // (RDG::PassBindings). The value is taken when the node is added, so a node sees every transient a node
    // added before it produced; the refs are handles of this graph and die with it. No Image2D* of a graph
    // resource crosses into a node body.
    struct FrameGraphRefs
    {
        FrameTransients     Transients;
        RDG::SystemTextures System;
    };

    // What a cloud-shadow receiver samples as u_CloudShadowMap this frame: the cloud layer's map, or
    // System.White (the sun is not occluded) when the frame has none. The ONE choice every receiver pass
    // (deferred composite, forward meshes, glass, terrain) declares and binds.
    [[nodiscard]] inline RDG::TextureRef CloudShadowMapOrWhite( const FrameGraphRefs& refs )
    {
        return refs.Transients.CloudShadowMap.IsValid() ? refs.Transients.CloudShadowMap : refs.System.White;
    }

    // MESH-PB1. THE scene/view inputs of a lit pass (UE: the view uniform buffer's textures - shadow maps, sky
    // cubes, PreIntegratedGF - which are pass parameters and never material parameters): every ref valid, an
    // absent producer stated as the engine's neutral texture (UE GSystemTextures). Forward meshes, glass and the
    // deferred composite take this ONE value, declare its reads (Refs) and bind it (BindSceneViewInputs).
    struct SceneViewInputs
    {
        std::array<RDG::TextureRef, kSceneViewShadowCascades> ShadowMaps;    // cascade c, or System.White
        RDG::TextureRef                                       EnvIrradiance; // diffuse cube, or System.BlackCube
        RDG::TextureRef                                       EnvSpecular; // prefiltered cube, or System.BlackCube
        RDG::TextureRef                                       BrdfLut;     // split-sum LUT, or System.Black
        RDG::TextureRef                                       CloudShadowMap; // CloudShadowMapOrWhite
        // The point/spot shadow atlas, or System.White (depth 1 everywhere = "no occluder"; no light then names a
        // view of it, so it is never read).
        RDG::TextureRef LocalShadowAtlas;
        // The reflection captures: their cube array (System.BlackCube is a cube, not an array - the view always
        // publishes its own, a one-cube black array when it has no captures) and their list.
        RDG::TextureRef ReflectionCaptureCubes;
        RDG::BufferRef  ReflectionCaptures;

        // Each distinct texture once (System.White fills several slots): what the node declares as read.
        [[nodiscard]] std::vector<RDG::TextureRef> Refs() const
        {
            std::vector<RDG::TextureRef> refs;
            const auto                   add = [&refs]( RDG::TextureRef ref )
            {
                if ( ref.IsValid() && std::find( refs.begin(), refs.end(), ref ) == refs.end() )
                    refs.push_back( ref );
            };
            for ( const RDG::TextureRef map : ShadowMaps )
                add( map );
            for ( const RDG::TextureRef ref : { EnvIrradiance, EnvSpecular, BrdfLut, CloudShadowMap, LocalShadowAtlas,
                                                ReflectionCaptureCubes } )
                add( ref );
            return refs;
        }
    };

    // The shader names the inputs are bound under (Mesh/CascadedShadow.glslh, Mesh/AmbientIBL.glslh, the cloud
    // shadow receiver). Every one is a separate texture read through a shared sampler state of the pass (the slot
    // names below); the cascades are one `texture2D u_ShadowMaps[4]`, a slot per element.
    inline constexpr std::array<std::string_view, kSceneViewShadowCascades> kSceneViewShadowMapElementNames = {
         "u_ShadowMaps[0]", "u_ShadowMaps[1]", "u_ShadowMaps[2]", "u_ShadowMaps[3]" };
    inline constexpr std::string_view kSceneViewEnvIrradianceName  = "u_EnvIrradianceTex";
    inline constexpr std::string_view kSceneViewEnvSpecularName    = "u_EnvSpecularTex";
    inline constexpr std::string_view kSceneViewBrdfLutName        = "u_BRDFLUTTexture";
    inline constexpr std::string_view kSceneViewCloudShadowMapName = "u_CloudShadowMap";
    inline constexpr std::string_view kSceneViewLocalShadowAtlasName = "u_LocalShadowAtlas";
    inline constexpr std::string_view kSceneViewReflectionCubesName = "u_ReflectionCaptureCubes";
    inline constexpr std::string_view kSceneViewReflectionListName  = "ReflectionCapturesUB";

    [[nodiscard]] inline SceneViewInputs SceneViewInputsOf( const FrameGraphRefs& refs )
    {
        const FrameTransients& t = refs.Transients;
        SceneViewInputs        inputs;
        for ( uint32_t c = 0; c < kSceneViewShadowCascades; ++c )
            inputs.ShadowMaps[c] = t.ShadowCascades[c].IsValid() ? t.ShadowCascades[c] : refs.System.White;
        inputs.EnvIrradiance  = t.EnvIrradiance.IsValid() ? t.EnvIrradiance : refs.System.BlackCube;
        inputs.EnvSpecular    = t.EnvSpecular.IsValid() ? t.EnvSpecular : refs.System.BlackCube;
        inputs.BrdfLut        = t.BrdfLut.IsValid() ? t.BrdfLut : refs.System.Black;
        inputs.CloudShadowMap = CloudShadowMapOrWhite( refs );
        inputs.LocalShadowAtlas = t.LocalShadowAtlas.IsValid() ? t.LocalShadowAtlas : refs.System.White;
        inputs.ReflectionCaptureCubes = t.ReflectionCaptureCubes;
        inputs.ReflectionCaptures     = t.ReflectionCaptures;
        return inputs;
    }

    namespace SceneViewDetail
    {
        // Whether the layout a pass's SETUP declares against has a separate-texture slot of that name.
        [[nodiscard]] inline bool LayoutSamples( const RDG::ShaderBindingLayout& layout, std::string_view name )
        {
            return std::any_of( layout.Slots.begin(), layout.Slots.end(),
                                [name]( const RDG::ShaderSlot& slot )
                                { return slot.Name == name && slot.Kind == RDG::ShaderResourceKind::Texture; } );
        }

        // THE one list of scene/view inputs with their sampler states: binds each one @p samples( name ) admits as
        // a separate texture, with the shared sampler slot it is read through (the receivers' shaders name these
        // slots: Mesh/Surface/Pass_Forward.glslh, Programs/Deferred/DeferredLighting.shader, ...). The cascades
        // and the cloud map linear, REPEAT (the state the images carried as their own); the cubes and the LUT
        // clamped (a LUT edge must not wrap into the opposite one); the local shadow atlas point-clamped (it is
        // read with texelFetch and filtered by hand inside each tile, Mesh/LocalLightShadow.glslh). A shared slot
        // named by several inputs is one entry (BindingBlockBuilder::Sampler).
        template <typename Block, typename Samples>
        void BindWhere( Block& block, const SceneViewInputs& inputs, const Samples& samples )
        {
            const auto bind = [&]( std::string_view name, RDG::TextureRef texture, std::string_view samplerSlot,
                                   RDG::SamplerDesc sampler )
            {
                if ( !samples( name ) )
                    return;
                block.Texture( name, texture, RDG::Access::SampledGraphics, RDG::SubresourceRange::All() )
                     .Sampler( samplerSlot, sampler );
            };
            constexpr std::string_view kRepeat = "u_LinearRepeatSampler";
            constexpr std::string_view kClamp  = "u_LinearClampSampler";
            constexpr std::string_view kPoint  = "u_PointClampSampler";
            for ( uint32_t c = 0; c < kSceneViewShadowCascades; ++c )
                bind( kSceneViewShadowMapElementNames[c], inputs.ShadowMaps[c], kRepeat,
                      RDG::SamplerDesc::LinearRepeat() );
            bind( kSceneViewEnvIrradianceName, inputs.EnvIrradiance, kClamp, RDG::SamplerDesc::LinearClamp() );
            bind( kSceneViewEnvSpecularName, inputs.EnvSpecular, kClamp, RDG::SamplerDesc::LinearClamp() );
            bind( kSceneViewBrdfLutName, inputs.BrdfLut, kClamp, RDG::SamplerDesc::LinearClamp() );
            bind( kSceneViewCloudShadowMapName, inputs.CloudShadowMap, kRepeat, RDG::SamplerDesc::LinearRepeat() );
            bind( kSceneViewLocalShadowAtlasName, inputs.LocalShadowAtlas, kPoint, RDG::SamplerDesc::PointClamp() );
            if ( inputs.ReflectionCaptureCubes.IsValid() )
                bind( kSceneViewReflectionCubesName, inputs.ReflectionCaptureCubes, kClamp,
                      RDG::SamplerDesc::LinearClamp() );
        }
    } // namespace SceneViewDetail

    // Whether @p layout samples any scene/view input: the lit programs do; a G-buffer, unlit or custom program
    // without the receivers does not. Read from the shader's REFLECTED resources, never from material properties.
    [[nodiscard]] inline bool SamplesSceneViewInputs( const RDG::ShaderBindingLayout& layout )
    {
        for ( std::string_view name : kSceneViewShadowMapElementNames )
            if ( SceneViewDetail::LayoutSamples( layout, name ) )
                return true;
        for ( std::string_view name : { kSceneViewEnvIrradianceName, kSceneViewEnvSpecularName, kSceneViewBrdfLutName,
                                        kSceneViewCloudShadowMapName, kSceneViewLocalShadowAtlasName } )
            if ( SceneViewDetail::LayoutSamples( layout, name ) )
                return true;
        return false;
    }

    // SETUP: declares on @p block (RenderPassDeclaration::BlockDeclaration or RDG::BindingBlockBuilder) every
    // input
    // @p layout has a sampled slot for, so a shader that samples no cascade (the glass) is never handed one - the
    // block would fault the pass in ValidatePassBindings ("'u_ShadowMap0' is not a resource of shader ...").
    // A block entry IS the read: the node does not also declare inputs.Refs().
    template <typename Block>
    void BindSceneViewInputs( Block& block, const SceneViewInputs& inputs, const RDG::ShaderBindingLayout& layout )
    {
        SceneViewDetail::BindWhere( block, inputs, [&layout]( std::string_view name )
                                    { return SceneViewDetail::LayoutSamples( layout, name ); } );
        // The capture list is a storage buffer, bound where the layout has that block.
        if ( inputs.ReflectionCaptures.IsValid() &&
             std::any_of( layout.Slots.begin(), layout.Slots.end(),
                          []( const RDG::ShaderSlot& slot ) {
                              return slot.Name == kSceneViewReflectionListName &&
                                     slot.Kind == RDG::ShaderResourceKind::StorageBuffer;
                          } ) )
            block.Storage( kSceneViewReflectionListName, inputs.ReflectionCaptures, RDG::Access::StorageRead );
    }
} // namespace Desert::Graphic
