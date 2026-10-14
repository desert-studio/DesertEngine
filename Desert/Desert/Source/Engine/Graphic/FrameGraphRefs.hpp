#pragma once

#include <Engine/Graphic/RDG/RDGResources.hpp>
#include <Engine/Graphic/RDG/RDGSystemTextures.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/Shader.hpp>

#include <algorithm>
#include <array>
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
    // The most shadow cascades a lit pass samples (the layers of u_ShadowCascades); SceneResources::kMaxCascades
    // is asserted equal where both are visible (MeshRendererInternal.hpp).
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
        // node is added: the sun's cascade array (MeshRenderer::AcquireCascadeMaps, layer c = cascade c; a cleared
        // one-layer array when the view has no cascades - the shader's sampler2DArray takes no 2D stand-in), the
        // scene environment's
        // two cubes (invalid when the scene has no baked sky) and the renderer's split-sum BRDF LUT. Never read
        // directly: a pass takes SceneViewInputsOf, which states the neutral texture for each one that is absent.
        RDG::TextureRef ShadowCascades;
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
        RDG::TextureRef ShadowCascades; // the cascade array (always one: ImportSceneViewTextures)
        RDG::TextureRef                                       EnvIrradiance; // diffuse cube, or System.BlackCube
        RDG::TextureRef                                       EnvSpecular; // prefiltered cube, or System.BlackCube
        RDG::TextureRef                                       BrdfLut;     // split-sum LUT, or System.Black
        RDG::TextureRef                                       CloudShadowMap; // CloudShadowMapOrWhite
        // The reflection captures: their cube array (System.BlackCube is a cube, not an array - the view always
        // publishes its own, a one-cube black array when it has no captures) and their list.
        RDG::TextureRef ReflectionCaptureCubes;
        RDG::BufferRef  ReflectionCaptures;
        // The point/spot shadow atlas, or System.White (depth 1 everywhere = "no occluder"; no light then names a
        // view of it, so it is never read).
        RDG::TextureRef LocalShadowAtlas;

        // Each distinct texture once (System.White fills several slots): what the node declares as read.
        [[nodiscard]] std::vector<RDG::TextureRef> Refs() const
        {
            std::vector<RDG::TextureRef> refs;
            const auto                   add = [&refs]( RDG::TextureRef ref )
            {
                if ( ref.IsValid() && std::find( refs.begin(), refs.end(), ref ) == refs.end() )
                    refs.push_back( ref );
            };
            add( ShadowCascades );
            for ( const RDG::TextureRef ref :
                  { EnvIrradiance, EnvSpecular, BrdfLut, CloudShadowMap, ReflectionCaptureCubes,
                    LocalShadowAtlas } )
                add( ref );
            return refs;
        }
    };

    // The shader names the inputs are bound under (Mesh/CascadedShadow.glslh, Mesh/AmbientIBL.glslh, the cloud
    // shadow receiver).
    inline constexpr std::string_view kSceneViewShadowCascadesName   = "u_ShadowCascades";
    inline constexpr std::string_view kSceneViewEnvIrradianceName  = "u_EnvIrradianceTex";
    inline constexpr std::string_view kSceneViewEnvSpecularName    = "u_EnvSpecularTex";
    inline constexpr std::string_view kSceneViewBrdfLutName        = "u_BRDFLUTTexture";
    inline constexpr std::string_view kSceneViewCloudShadowMapName = "u_CloudShadowMap";
    inline constexpr std::string_view kSceneViewReflectionCubesName = "u_ReflectionCaptureCubes";
    inline constexpr std::string_view kSceneViewReflectionListName  = "ReflectionCapturesUB";
    inline constexpr std::string_view kSceneViewLocalShadowAtlasName = "u_LocalShadowAtlas";

    [[nodiscard]] inline SceneViewInputs SceneViewInputsOf( const FrameGraphRefs& refs )
    {
        const FrameTransients& t = refs.Transients;
        SceneViewInputs        inputs;
        inputs.ShadowCascades         = t.ShadowCascades;
        inputs.EnvIrradiance  = t.EnvIrradiance.IsValid() ? t.EnvIrradiance : refs.System.BlackCube;
        inputs.EnvSpecular    = t.EnvSpecular.IsValid() ? t.EnvSpecular : refs.System.BlackCube;
        inputs.BrdfLut        = t.BrdfLut.IsValid() ? t.BrdfLut : refs.System.Black;
        inputs.CloudShadowMap = CloudShadowMapOrWhite( refs );
        inputs.ReflectionCaptureCubes = t.ReflectionCaptureCubes;
        inputs.ReflectionCaptures     = t.ReflectionCaptures;
        inputs.LocalShadowAtlas = t.LocalShadowAtlas.IsValid() ? t.LocalShadowAtlas : refs.System.White;
        return inputs;
    }

    namespace SceneViewDetail
    {
        [[nodiscard]] inline bool Reflects2D( const Shader& shader, std::string_view name )
        {
            const auto samplers = shader.GetUniformImage2DModels();
            return std::any_of( samplers.begin(), samplers.end(),
                                [name]( const auto& sampler ) { return sampler.Name == name; } );
        }
        [[nodiscard]] inline bool ReflectsCube( const Shader& shader, std::string_view name )
        {
            const auto samplers = shader.GetUniformImageCubeModels();
            return std::any_of( samplers.begin(), samplers.end(),
                                [name]( const auto& sampler ) { return sampler.Name == name; } );
        }
        // A sampled-texture slot of that name in the layout a pass's SETUP declares against (a 2D sampler and a
        // cube are both ShaderResourceKind::SampledTexture there).
        [[nodiscard]] inline bool LayoutSamples( const RDG::ShaderBindingLayout& layout, std::string_view name )
        {
            return std::any_of(
                 layout.Slots.begin(), layout.Slots.end(), [name]( const RDG::ShaderSlot& slot )
                 { return slot.Name == name && slot.Kind == RDG::ShaderResourceKind::SampledTexture; } );
        }

        // THE one list of scene/view inputs with their samplers: binds each one @p samples( name ) admits. The
        // cascades and the cloud map with the sampler the material route used (the image's own: linear, REPEAT);
        // the cubes and the LUT clamped (a LUT edge must not wrap into the opposite one).
        template <typename Block, typename Samples>
        void BindWhere( Block& block, const SceneViewInputs& inputs, const Samples& samples )
        {
            const auto bind = [&]( std::string_view name, RDG::TextureRef texture, RDG::SamplerDesc sampler )
            {
                if ( samples( name ) )
                    block.Sampled( name, texture, RDG::Access::SampledGraphics, RDG::SubresourceRange::All(),
                                   sampler );
            };
            if ( inputs.ShadowCascades.IsValid() )
                bind( kSceneViewShadowCascadesName, inputs.ShadowCascades, RDG::SamplerDesc::LinearRepeat() );
            bind( kSceneViewEnvIrradianceName, inputs.EnvIrradiance, RDG::SamplerDesc::LinearClamp() );
            bind( kSceneViewEnvSpecularName, inputs.EnvSpecular, RDG::SamplerDesc::LinearClamp() );
            bind( kSceneViewBrdfLutName, inputs.BrdfLut, RDG::SamplerDesc::LinearClamp() );
            bind( kSceneViewCloudShadowMapName, inputs.CloudShadowMap, RDG::SamplerDesc::LinearRepeat() );
            if ( inputs.ReflectionCaptureCubes.IsValid() )
                bind( kSceneViewReflectionCubesName, inputs.ReflectionCaptureCubes,
                      RDG::SamplerDesc::LinearClamp() );
            // Read with texelFetch (Mesh/LocalLightShadow.glslh filters by hand inside each tile).
            bind( kSceneViewLocalShadowAtlasName, inputs.LocalShadowAtlas, RDG::SamplerDesc::PointClamp() );
        }

        template <typename Samples>
        [[nodiscard]] bool SamplesAny( const Samples& samples )
        {
            return samples( kSceneViewShadowCascadesName ) || samples( kSceneViewEnvIrradianceName ) ||
                   samples( kSceneViewEnvSpecularName ) || samples( kSceneViewBrdfLutName ) ||
                   samples( kSceneViewCloudShadowMapName ) || samples( kSceneViewLocalShadowAtlasName );
        }
    } // namespace SceneViewDetail

    // Whether @p layout samples any scene/view input: the lit programs do; a G-buffer, unlit or custom program
    // without the receivers does not. Read from the shader's REFLECTED resources, never from material properties.
    [[nodiscard]] inline bool SamplesSceneViewInputs( const RDG::ShaderBindingLayout& layout )
    {
        return SceneViewDetail::SamplesAny( [&layout]( std::string_view name )
                                            { return SceneViewDetail::LayoutSamples( layout, name ); } );
    }
    [[nodiscard]] inline bool SamplesSceneViewInputs( const Shader& shader )
    {
        return SceneViewDetail::SamplesAny(
             [&shader]( std::string_view name ) {
                 return SceneViewDetail::Reflects2D( shader, name ) ||
                        SceneViewDetail::ReflectsCube( shader, name );
             } );
    }

    // SETUP: declares on @p block (RenderPassDeclaration::BlockDeclaration or RDG::BindingBlockBuilder) every
    // input
    // @p layout has a sampled slot for, so a shader that samples no cascade (the glass) is never handed one - the
    // block would fault the pass in ValidatePassBindings ("'u_ShadowCascades' is not a resource of shader ...").
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
