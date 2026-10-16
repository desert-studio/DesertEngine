#pragma once

#include <Engine/Graphic/RDG/RDGResources.hpp>
#include <Engine/Graphic/RDG/RDGSystemTextures.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/Shader.hpp>

#include <glm/glm.hpp>

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

    // The view's dynamic lights as buffers of THIS frame's graph (SceneRenderer::AddFrameLightGrid, before any lit
    // node): the point and spot rows, the local shadow views their ShadowView indices name, and the view's light
    // grid (Graphic/LightGrid.hpp, UE's ForwardLightData) — its parameters (uploaded) and its cells (written by
    // the "LightGrid" compute node). Every lit pass reads the grid: the deferred composite, the forward meshes.
    struct ViewLightBuffers
    {
        RDG::BufferRef Point;       // "Upload: Lights.Point"
        RDG::BufferRef Spot;        // "Upload: Lights.Spot"
        RDG::BufferRef ShadowViews; // "Upload: Lights.ShadowViews"
        RDG::BufferRef GridParams;  // "Upload: Lights.GridParams"
        RDG::BufferRef GridCells;   // written by "LightGrid"
        // The world layout of the same lights (LightGridLayout::WorldClipmaps: the radiance cache's clipmap cells,
        // for GI hits off screen). Invalid unless the frame has a reader of it
        // (SceneRenderer::WorldLightGridRead).
        RDG::BufferRef WorldGridParams; // "Upload: Lights.WorldGridParams"
        RDG::BufferRef WorldGridCells;  // written by "LightGridWorld"
    };

    struct FrameTransients
    {
        RDG::TextureRef Bloom;          // BloomRenderer chain (mip 0 read)   -> Tonemap
        RDG::TextureRef LightShafts;    // LightShaftRenderer result          -> Tonemap
        RDG::TextureRef LensFlare;      // LensFlareRenderer result           -> Tonemap
        RDG::TextureRef SSAO;           // SSAO factor                        -> deferred lighting
        RDG::TextureRef SceneColorCopy; // scene snapshot                     -> glass refraction
        // The opaque scene's device depth (the G-buffer depth, reversed-Z), set by the deferred composite; invalid
        // on the forward path, which has no depth before its meshes draw   -> contact shadows of the forward
        // meshes and the glass (SceneViewInputs::SceneDepth)
        RDG::TextureRef SceneDepth;
        // The opaque scene's shading word (the G-buffer's, RG32_UINT), set beside SceneDepth by the deferred
        // composite: the NO_CONTACT_SHADOW bit of the texel a forward contact-shadow ray hit (UE
        // CastContactShadow( GBuffer ))                                    -> SceneViewInputs::SceneShadingWord
        RDG::TextureRef SceneShadingWord;
        RDG::TextureRef BackdropBlur;   // BackdropBlurRenderer pyramid (all mips) -> UI glass (Render2D)
        RDG::TextureRef HeightFog;      // HeightFogRenderer evaluation (RGBA16F) -> HeightFogApply
        // The clouds' reconstruction pair written this frame (VolumetricCloudRenderer::GetFrameResult: transients
        // the view extracts as its history, SceneViewState::Clouds); invalid when the frame has none ->
        // CloudComposite
        RDG::TextureRef CloudScatter;
        RDG::TextureRef CloudGuide;
        // The cloud layer's shadow map (VolumetricCloudRenderer::GetShadowMap, a transient of this graph), set
        // once its shadow node was accepted; invalid when the frame has none   -> Deferred: Composite
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
        // The view's lights and light grid (ViewLightBuffers); invalid refs until AddFrameLightGrid ran.
        ViewLightBuffers Lights;
        // The global distance field brought up to this frame (SceneRenderer::AddFrameGlobalDistanceField,
        // imported: DistanceField::GlobalDistanceField owns the clipmaps) and its clipmap parameters (centre.xyz
        // cm, voxel cm); invalid refs when nothing of the frame reads it   -> the radiance cache's trace, "Debug:
        // Global Distance Field"
        std::array<RDG::TextureRef, 4> GlobalDistanceField;
        std::array<glm::vec4, 4>       GlobalDistanceFieldLevels{};
        RDG::TextureRef                GlobalDistanceFieldObjectGrid; // its object grid (GlobalDistanceField.hpp)
        // The traced GI's final gather of this frame (SceneRenderer::AddFrameTracedGI: the world radiance cache's
        // update, then GlobalIllumination::ScreenProbeGather under SoftwareTraced / HardwareTraced or
        // GlobalIllumination::IrradianceFieldGather under ProbeVolume): per pixel, the diffuse irradiance and the
        // specular radiance. Invalid when the GI method traces nothing   -> Deferred: Composite
        RDG::TextureRef GIDiffuse;
        RDG::TextureRef GISpecular;
        // THE VIEW'S WORLD RADIANCE CACHE as its readers bind it (GlobalIllumination::RadianceCacheFrame, the read
        // section of Common/RadianceCache.glslh): the neutral cache - one zeroed params block standing for all
        // three buffers (no clipmap: every read answers "past the coarsest clipmap"), System.Black for both
        // atlases - from SceneRenderer::ImportSceneViewTextures, replaced by this frame's update once
        // AddFrameTracedGI ran it   -> the forward meshes' and the glass's diffuse estimator, which must be the
        // one the reflection captures' stored diffuse was measured with
        RDG::BufferRef  RadianceCacheParams;
        RDG::BufferRef  RadianceCacheIndirection;
        RDG::BufferRef  RadianceCacheProbes;
        RDG::TextureRef RadianceCacheIrradiance;
        RDG::TextureRef RadianceCacheDepth;
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
        // The opaque scene's device depth the contact shadows march (Common/ContactShadow.glslh), or System.Black
        // (reversed-Z 0 = the far plane everywhere: nothing stands in front of any ray, so no contact shadow).
        RDG::TextureRef SceneDepth;
        // The opaque scene's shading word, whose NO_CONTACT_SHADOW bit picks the hit's Casting / Non Casting
        // Intensity, or System.ZeroUint (an integer texture for the usampler2D; never decisive: with the black
        // depth no ray hits).
        RDG::TextureRef SceneShadingWord;
        // The view's light grid (ViewLightBuffers::GridParams / GridCells): a lit pass shades a pixel with the
        // point and spot lights of its froxel. Bound where the layout has the two storage blocks.
        RDG::BufferRef LightGridParams;
        RDG::BufferRef LightGridCells;
        // The view's world radiance cache (FrameTransients::RadianceCache*, the neutral one where the view traces
        // no GI): the forward passes read their diffuse estimator from it, as the deferred composite's final
        // gather does, so a capture measured by the cache is normalised by the cache on a forward pixel too.
        RDG::BufferRef  RadianceCacheParams;
        RDG::BufferRef  RadianceCacheIndirection;
        RDG::BufferRef  RadianceCacheProbes;
        RDG::TextureRef RadianceCacheIrradiance;
        RDG::TextureRef RadianceCacheDepth;

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
                  { EnvIrradiance, EnvSpecular, BrdfLut, CloudShadowMap, ReflectionCaptureCubes, LocalShadowAtlas,
                    SceneDepth, SceneShadingWord, RadianceCacheIrradiance, RadianceCacheDepth } )
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
    inline constexpr std::string_view kSceneViewCloudShadowMapName   = "u_CloudShadowMap";
    inline constexpr std::string_view kSceneViewReflectionCubesName = "u_ReflectionCaptureCubes";
    inline constexpr std::string_view kSceneViewReflectionListName  = "ReflectionCapturesUB";
    inline constexpr std::string_view kSceneViewLocalShadowAtlasName = "u_LocalShadowAtlas";
    inline constexpr std::string_view kSceneViewSceneDepthName       = "u_SceneDepth";
    inline constexpr std::string_view kSceneViewSceneShadingWordName = "u_SceneShadingWord";
    // The radiance cache's read (Common/RadianceCache.glslh; the names every reader of the cache binds).
    inline constexpr std::string_view kSceneViewRadianceCacheParamsName      = "RadianceCacheParamsUB";
    inline constexpr std::string_view kSceneViewRadianceCacheIndirectionName = "RadianceCacheIndirectionUB";
    inline constexpr std::string_view kSceneViewRadianceCacheProbesName      = "RadianceCacheProbesUB";
    inline constexpr std::string_view kSceneViewRadianceCacheIrradianceName  = "u_RadianceCacheIrradiance";
    inline constexpr std::string_view kSceneViewRadianceCacheDepthName       = "u_RadianceCacheDepth";
    // The light grid's two blocks (Mesh/LightGrid.glslh): the build shader, the composite, the forward cells.
    inline constexpr std::string_view kLightGridParamsBlockName = "LightGridParamsUB";
    inline constexpr std::string_view kLightGridCellsBlockName  = "LightGridCellsUB";
    // A READER of the world layout's cells (Mesh/LightGrid.glslh, LIGHT_GRID_WORLD_CELLS); the build shader writes
    // either layout through kLightGridCellsBlockName.
    inline constexpr std::string_view kLightGridWorldCellsBlockName = "LightGridWorldCellsUB";

    [[nodiscard]] inline SceneViewInputs SceneViewInputsOf( const FrameGraphRefs& refs )
    {
        const FrameTransients& t = refs.Transients;
        SceneViewInputs        inputs;
        inputs.ShadowCascades         = t.ShadowCascades;
        inputs.EnvIrradiance  = t.EnvIrradiance.IsValid() ? t.EnvIrradiance : refs.System.BlackCube;
        inputs.EnvSpecular    = t.EnvSpecular.IsValid() ? t.EnvSpecular : refs.System.BlackCube;
        inputs.BrdfLut        = t.BrdfLut.IsValid() ? t.BrdfLut : refs.System.Black;
        inputs.CloudShadowMap         = CloudShadowMapOrWhite( refs );
        inputs.ReflectionCaptureCubes = t.ReflectionCaptureCubes;
        inputs.ReflectionCaptures     = t.ReflectionCaptures;
        inputs.LocalShadowAtlas       = t.LocalShadowAtlas.IsValid() ? t.LocalShadowAtlas : refs.System.White;
        inputs.SceneDepth             = t.SceneDepth.IsValid() ? t.SceneDepth : refs.System.Black;
        inputs.SceneShadingWord       = t.SceneShadingWord.IsValid() ? t.SceneShadingWord : refs.System.ZeroUint;
        inputs.LightGridParams        = t.Lights.GridParams;
        inputs.LightGridCells         = t.Lights.GridCells;
        inputs.RadianceCacheParams      = t.RadianceCacheParams;
        inputs.RadianceCacheIndirection = t.RadianceCacheIndirection;
        inputs.RadianceCacheProbes      = t.RadianceCacheProbes;
        inputs.RadianceCacheIrradiance =
             t.RadianceCacheIrradiance.IsValid() ? t.RadianceCacheIrradiance : refs.System.Black;
        inputs.RadianceCacheDepth = t.RadianceCacheDepth.IsValid() ? t.RadianceCacheDepth : refs.System.Black;
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
        [[nodiscard]] inline bool LayoutReadsBuffer( const RDG::ShaderBindingLayout& layout,
                                                     std::string_view                name )
        {
            return std::any_of(
                 layout.Slots.begin(), layout.Slots.end(), [name]( const RDG::ShaderSlot& slot )
                 { return slot.Name == name && slot.Kind == RDG::ShaderResourceKind::StorageBuffer; } );
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
            // Marched texel by texel (Common/ContactShadow.glslh): nearest, clamped at the screen's edge.
            bind( kSceneViewSceneDepthName, inputs.SceneDepth, RDG::SamplerDesc::PointClamp() );
            // Read with texelFetch at the hit texel (an integer format is never filtered).
            bind( kSceneViewSceneShadingWordName, inputs.SceneShadingWord, RDG::SamplerDesc::PointClamp() );
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
        // The light grid, where the shader reads it. A frame graph without the grid (no AddFrameLightGrid) hands
        // invalid refs, and a shader that reads the grid then faults its pass by name rather than reading nothing.
        for ( const auto& [name, buffer] : { std::pair{ kLightGridParamsBlockName, inputs.LightGridParams },
                                             std::pair{ kLightGridCellsBlockName, inputs.LightGridCells } } )
            if ( SceneViewDetail::LayoutReadsBuffer( layout, name ) )
                block.Storage( name, buffer, RDG::Access::StorageRead );
        // The radiance cache, where the shader reads it (the forward passes' estimator). Bound here and not in
        // BindWhere: the surface cache's lighting passes bind their own cache under the same names.
        for ( const auto& [name, buffer] :
              { std::pair{ kSceneViewRadianceCacheParamsName, inputs.RadianceCacheParams },
                std::pair{ kSceneViewRadianceCacheIndirectionName, inputs.RadianceCacheIndirection },
                std::pair{ kSceneViewRadianceCacheProbesName, inputs.RadianceCacheProbes } } )
            if ( SceneViewDetail::LayoutReadsBuffer( layout, name ) )
                block.Storage( name, buffer, RDG::Access::StorageRead );
        for ( const auto& [name, texture] :
              { std::pair{ kSceneViewRadianceCacheIrradianceName, inputs.RadianceCacheIrradiance },
                std::pair{ kSceneViewRadianceCacheDepthName, inputs.RadianceCacheDepth } } )
            if ( SceneViewDetail::LayoutSamples( layout, name ) )
                block.Sampled( name, texture, RDG::Access::SampledGraphics, RDG::SubresourceRange::All(),
                               RDG::SamplerDesc::LinearClamp() );
    }
} // namespace Desert::Graphic
