#include "WaterRenderer.hpp"

#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/ViewTargetLayouts.hpp>
#include <Engine/Graphic/Materials/Properties/StorageBufferProperty.hpp>
#include <Engine/Graphic/Materials/Properties/UniformBufferProperty.hpp>
#include <Engine/Graphic/PipelineCache.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Material/MaterialService.hpp>
#include <Engine/Runtime/Services/Texture/TextureService.hpp>
#include <Engine/Graphic/Materials/MaterialOverrides.hpp>
#include <Engine/Graphic/Materials/Properties/Texture2DProperty.hpp>
#include <Engine/Graphic/Texture.hpp>
#include <Engine/Core/Frustum.hpp>
#include <Common/Core/Profiler.hpp>

#include <cmath>
#include <cstddef>
#include <format>
#include <algorithm>
#include <array>
#include <string>
#include <unordered_map>

namespace
{
    // The slot names this pass binds, spelled once (RDGShaderParameters.hpp).
    struct WaterShaderParameters
    {
        DESERT_SHADER_PARAMETER( UniformBuffer, WaterUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, WaterTiles );
        DESERT_SHADER_PARAMETER( UniformBuffer, WaterMaterialUB );
    };
} // namespace

namespace Desert::Graphic::System
{
    namespace
    {
        // Matches WaterUB (binding 0) in Programs/Water/WaterSurfaceData.glslh: frame data only.
        struct WaterUB
        {
            glm::mat4 ViewProjection;
            glm::mat4 PrevViewProjection;
            glm::vec4 CameraPos;
            glm::vec4 SunDir;
            glm::vec4 SunColor;
            glm::vec4 Exposure; // x = ViewFrame::PreExposure, y = seconds since the renderer started, zw unused
            glm::mat4 InvViewProjection;
        };

        // Matches WaterMaterialUB (binding 2): the `.demat`'s values, packed by name.
        struct WaterMaterialUB
        {
            glm::vec4 Scattering;
            glm::vec4 Absorption;
            glm::vec4 ColorScaleBehindWater;
            glm::vec4 Shading; // PhaseG, Roughness, RefractionStrength, Specular
            glm::vec4 Ripple;  // RippleTileSizeA, RippleTileSizeB, RippleSpeed, RippleStrength
            glm::vec4 RippleDirection;
        };

        // The ripple maps of WaterSurface.shader's Properties: the texture slots the material binds.
        constexpr std::array<const char*, 2> kRippleMaps = { "RippleNormalA", "RippleNormalB" };

        // Matches WaterTileRow (WaterTiles, binding 1).
        struct WaterTileRow
        {
            glm::vec4 Tile;
            glm::vec4 Lod;
            glm::vec4 Zone;
        };
        static_assert( sizeof( WaterTileRow ) == 3 * sizeof( glm::vec4 ), "std430 rows of three vec4" );

        // The culling box's padding above and below the still plane. The surface is flat (WATER-W6); WATER-W7's
        // waves widen it by the bodies' MaxWaveHeight.
        constexpr float kFlatSurfaceVerticalExtentCm = 1.0f;
    } // namespace

    Common::BoolResultStr WaterRenderer::Initialize()
    {
        // The engine's program must build: a material naming it is the common case and its failure is loud here.
        const auto shader = Runtime::ResourceRegistry::GetShaderService()->GetByName( "WaterSurface" );
        if ( !shader )
            return Common::MakeError( "WaterRenderer: missing shader 'WaterSurface'" );
        return BOOLSUCCESS;
    }

    void WaterRenderer::WarnOnce( const Assets::AssetHandle& material, const std::string& why )
    {
        if ( m_Warned.insert( static_cast<uint64_t>( material ) ).second )
            LOG_WARN( "[Water] water material {}: {}", static_cast<uint64_t>( material ), why );
    }

    WaterRenderer::WaterProgram* WaterRenderer::ProgramFor( const Assets::AssetHandle& material )
    {
        auto* materials = Runtime::ResourceRegistry::GetMaterialService();
        if ( materials == nullptr )
            return nullptr;
        const auto templ = materials->ShaderHandleOf( material );
        if ( static_cast<uint64_t>( templ.Shader ) == 0 || templ.CompileName.empty() )
        {
            WarnOnce( material, "names no material with a loaded template - the body draws no water" );
            return nullptr;
        }
        if ( auto it = m_Programs.find( templ.CompileName ); it != m_Programs.end() )
            return it->second.Pipeline ? &it->second : nullptr;

        WaterProgram& program = m_Programs[templ.CompileName]; // an empty entry remembers a refusal
        const auto    shader  = Runtime::ResourceRegistry::GetShaderService()->GetByName( templ.CompileName );
        if ( !shader )
        {
            WarnOnce( material, std::format( "its template '{}' is not loaded", templ.CompileName ) );
            return nullptr;
        }
        if ( shader->GetProgramMeta().Domain != Core::Formats::ShaderDomain::SingleLayerWater )
        {
            WarnOnce( material,
                      std::format( "its template '{}' is of the {} domain, not SingleLayerWater - a water body "
                                   "draws only a water material",
                                   templ.CompileName,
                                   Core::Formats::ShaderDomainName( shader->GetProgramMeta().Domain ) ) );
            return nullptr;
        }

        GraphicsPipelineSpecification spec;
        spec.DebugName    = "WaterSurfacePipeline:" + templ.CompileName;
        spec.Shader       = shader;
        spec.TargetLayout = SceneTargetLayout();
        spec.BlendEnable  = false;
        ApplyShaderRenderState( spec, shader->GetProgramMeta().State );
        const auto pipeline = m_SceneRenderer->GetPipelineCache().GetOrCreate( spec );
        if ( !pipeline )
        {
            WarnOnce( material, std::format( "its template '{}' does not build: {}", templ.CompileName,
                                             pipeline.GetError() ) );
            return nullptr;
        }
        program.Pipeline = pipeline.GetValue();
        program.Material = std::make_unique<DataDrivenMaterial>( templ.CompileName );
        return &program;
    }

    void WaterRenderer::ApplyMaterial( const Assets::AssetHandle& material, WaterProgram& program )
    {
        // The schema first (every default), the `.demat` chain over it by name: nearest wins.
        std::unordered_map<std::string, glm::vec4> values;
        const auto& meta = program.Pipeline->GetSpecification().Shader->GetProgramMeta();
        for ( const auto& param : meta.Params )
            if ( !param.IsTexture && !param.IsAssetRef() )
                values[param.Name] = param.Default;
        MaterialOverrides overrides;
        if ( auto* materials = Runtime::ResourceRegistry::GetMaterialService() )
            (void)materials->ResolveOverrides( material, overrides );
        for ( const auto& [name, value] : overrides.Params )
            values[name] = value;
        const auto value = [&values]( const char* name ) -> glm::vec4
        {
            const auto it = values.find( name );
            return it != values.end() ? it->second : glm::vec4( 0.0f );
        };
        const auto direction = []( float degrees )
        {
            const float r = glm::radians( degrees );
            return glm::vec2( std::cos( r ), std::sin( r ) );
        };

        WaterMaterialUB ub{};
        ub.Scattering            = value( "ScatteringCoefficients" );
        ub.Absorption            = value( "AbsorptionCoefficients" );
        ub.ColorScaleBehindWater = value( "ColorScaleBehindWater" );
        ub.Shading = glm::vec4( value( "PhaseG" ).x, value( "Roughness" ).x, value( "RefractionStrength" ).x,
                                value( "Specular" ).x );
        ub.Ripple  = glm::vec4( std::max( value( "RippleTileSizeA" ).x, 1.0f ),
                                std::max( value( "RippleTileSizeB" ).x, 1.0f ), value( "RippleSpeed" ).x,
                                value( "RippleStrength" ).x );
        const glm::vec2 a      = direction( value( "RippleAngleA" ).x );
        const glm::vec2 b      = direction( value( "RippleAngleB" ).x );
        ub.RippleDirection     = glm::vec4( a, b );
        if ( auto* block = program.Material->Get<UniformBufferProperty>( WaterShaderParameters::WaterMaterialUB ) )
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): a uniform block uploaded as bytes
            block->SetRawData( reinterpret_cast<const std::byte*>( &ub ), sizeof( ub ) );

        // The ripple maps: the material's texture by handle; a slot it leaves empty keeps the schema's default.
        for ( const auto& [name, handle] : overrides.Textures )
        {
            if ( handle == 0 || std::find_if( kRippleMaps.begin(), kRippleMaps.end(), [&name]( const char* slot )
                                              { return name == slot; } ) == kRippleMaps.end() )
                continue;
            auto* property = program.Material->Get<Texture2DProperty>( name );
            auto* texture  = Runtime::ResourceRegistry::GetTextureService()->Get( Common::UUID( handle ) );
            if ( property == nullptr || texture == nullptr )
                continue; // still loading: picked up the frame it lands
            auto* image = dynamic_cast<Image2D*>(
                 Runtime::ResourceRegistry::GetImageService()->Resolve( texture->GetImageHandle() ) );
            if ( image != nullptr )
                property->SetImage( image, RDG::Access::SampledGraphics );
        }
    }

    void WaterRenderer::PrepareFrame()
    {
        m_FrameDraws.clear();
        const auto* camera = m_SceneRenderer->GetMainCamera();
        m_Frame = nullptr;
        if ( camera == nullptr || m_Queue.empty() )
            return;

        // THE FRAME'S WATER MATERIAL: the first body's that names one. Bodies of one frame naming different
        // materials draw with the first (REMAINDER WATER: a per-tile body index, UE's WaterBodyIndex).
        Assets::AssetHandle material = Assets::AssetHandle::Null();
        for ( const WaterZoneDraw& zone : m_Queue )
            for ( const Assets::AssetHandle& body : zone.BodyMaterials )
            {
                if ( static_cast<uint64_t>( body ) == 0 )
                    continue;
                if ( static_cast<uint64_t>( material ) == 0 )
                    material = body;
                else if ( body != material )
                    WarnOnce( body, "differs from another body's in the same frame - drawn with the first one" );
            }
        if ( static_cast<uint64_t>( material ) == 0 )
            return; // no body names a look: no water (WaterBodyData::WaterMaterial)
        WaterProgram* program = ProgramFor( material );
        if ( program == nullptr )
            return;

        const glm::mat4     view     = camera->GetViewMatrix();
        const glm::mat4     proj     = camera->GetProjectionMatrix();
        const glm::vec3     observer = glm::vec3( glm::inverse( view )[3] );
        const Core::Frustum frustum( proj, view );

        // Every zone's tiles in its density order: one instanced draw per run of one grid density (a row carries
        // its own zone, so a run may span two zones).
        std::vector<WaterTileRow> rows;
        for ( const WaterZoneDraw& zone : m_Queue )
        {
            const Water::WaterQuadTree                  tree( zone.Zone, zone.Bodies );
            const std::vector<Water::WaterTileInstance> tiles =
                 tree.Traverse( observer, &frustum, kFlatSurfaceVerticalExtentCm );
            const glm::vec2 origin = tree.GetRootMin();
            for ( const Water::WaterTileInstance& tile : tiles )
            {
                const uint32_t quads = tree.QuadsPerSide( tile.DensityIndex );
                if ( m_FrameDraws.empty() || m_FrameDraws.back().QuadsPerSide != quads )
                {
                    m_FrameDraws.push_back( { .FirstRow     = static_cast<uint32_t>( rows.size() ),
                                              .RowCount     = 0,
                                              .QuadsPerSide = quads } );
                }
                ++m_FrameDraws.back().RowCount;

                WaterTileRow row;
                row.Tile = glm::vec4( tile.Corner.x, tile.Corner.y, tile.SurfaceY, tile.Size );
                row.Lod  = glm::vec4( static_cast<float>( tile.LODLevel ), tile.ShouldMorph ? 1.0f : 0.0f,
                                     tile.CanMorphTwice ? 1.0f : 0.0f, tile.HeightMorph );
                row.Zone = glm::vec4( origin.x, origin.y, tree.GetLODScale(), static_cast<float>( quads ) );
                rows.push_back( row );
            }
        }
        if ( rows.empty() )
            return;

        WaterUB     ub{};
        const auto* viewFrame = m_SceneRenderer->GetViewFrame();
        ub.ViewProjection     = viewFrame->ViewProjection;
        ub.PrevViewProjection = viewFrame->PrevViewProjection;
        ub.CameraPos          = glm::vec4( observer, 1.0f );
        ub.SunDir             = glm::vec4( glm::normalize( glm::vec3( -0.4f, -0.85f, -0.35f ) ), 0.0f );
        ub.SunColor           = glm::vec4( 1.0f, 0.98f, 0.92f, 3.0f );
        ub.Exposure           = glm::vec4(
             viewFrame->PreExposure,
             std::chrono::duration<float>( std::chrono::steady_clock::now() - m_Start ).count(), 0.0f, 0.0f );
        ub.InvViewProjection = glm::inverse( proj * view );
        if ( const auto& lights = m_SceneRenderer->GetDirectionLights().DirectionLights; !lights.empty() )
        {
            ub.SunDir   = lights[0].Direction;
            ub.SunColor = lights[0].ColorIntensity;
        }

        // Uploaded up front at final size, before any draw records (the terrain's reason: growing a storage buffer
        // reallocates it under a draw already recorded against the old one).
        ApplyMaterial( material, *program );
        if ( auto* block = program->Material->Get<UniformBufferProperty>( WaterShaderParameters::WaterUB ) )
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): a uniform block uploaded as bytes
            block->SetRawData( reinterpret_cast<const std::byte*>( &ub ), sizeof( ub ) );
        if ( auto* tiles = program->Material->Get<StorageBufferProperty>( WaterShaderParameters::WaterTiles ) )
            tiles->SetRawData( rows.data(), static_cast<uint32_t>( rows.size() * sizeof( WaterTileRow ) ) );
        m_Frame = program;
    }

    void WaterRenderer::DeclareDraws( RDG::PassBuilder& pass, const RDG::TextureRef sceneCopy,
                                      const SceneViewInputs& view )
    {
        // Exactly when RenderManual draws.
        if ( m_FrameDraws.empty() || m_Frame == nullptr || !sceneCopy.IsValid() )
            return;
        const auto layout = m_Layout.Get( m_Frame->Pipeline->GetSpecification().Shader );
        auto block = pass.Bindings( layout, m_Frame->Material->GetMaterialExecutor()->GetRouteFill() );
        BindSceneViewInputs( block, view, *layout );
        constexpr RDG::TextureParameter kSceneColor{ "SceneColorTexture" };
        constexpr RDG::SamplerParameter kClamp{ "LinearClampSampler" };
        block.Texture( kSceneColor, sceneCopy, RDG::Access::SampledGraphics, RDG::SubresourceRange::All() )
             .Sampler( kClamp, RDG::SamplerDesc::LinearClamp() );
    }

    Common::BoolResultStr WaterRenderer::RenderManual( const RDG::PassContext& context )
    {
        const auto* camera = m_SceneRenderer->GetMainCamera();
        if ( m_FrameDraws.empty() || m_Frame == nullptr || camera == nullptr )
            return BOOLSUCCESS;
        DataDrivenMaterial& material = *m_Frame->Material;
        DESERT_PROFILE_PASS( "Water: Surface" );

        // The row base and the matrix are push constants, snapshotted per draw at record.
        const RDG::PassBindings block( context, context.GetBindingBlock( 0 ) );
        material.SetPushMatrix( camera->GetProjectionMatrix() * camera->GetViewMatrix() );
        for ( const FrameDraw& draw : m_FrameDraws )
        {
            material.SetMaterialIndex( draw.FirstRow );
            if ( auto drawn = Renderer::GetInstance().DrawProcedural(
                      block, *m_Frame->Pipeline, material.GetMaterialExecutor(),
                      draw.QuadsPerSide * draw.QuadsPerSide * 6u, draw.RowCount );
                 !drawn.IsSuccess() )
                return drawn;
        }
        return BOOLSUCCESS;
    }
} // namespace Desert::Graphic::System
