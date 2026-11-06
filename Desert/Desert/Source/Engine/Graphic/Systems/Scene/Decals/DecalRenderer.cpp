#include "DecalRenderer.hpp"

#include <Engine/Core/FrameManager.hpp>
#include <Engine/Core/Formats/ShaderProgramMeta.hpp>
#include <Engine/Graphic/Image.hpp>
#include <Engine/Graphic/Materials/MaterialExecutor.hpp>
#include <Engine/Graphic/Materials/MaterialOverrides.hpp>
#include <Engine/Graphic/PipelineCache.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/Shader.hpp>
#include <Engine/Graphic/Texture.hpp>
#include <Engine/Graphic/ViewTargetLayouts.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Image/ImageService.hpp>
#include <Engine/Runtime/Services/Material/MaterialService.hpp>
#include <Engine/Runtime/Services/Shader/ShaderService.hpp>
#include <Engine/Runtime/Services/Texture/TextureService.hpp>

#include <Common/Core/Logger.hpp>
#include <Common/Core/Profiler.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <span>
#include <string>

namespace Desert::Graphic::System
{
    namespace
    {
        // The engine's decal (UE DefaultDeferredDecalMaterial): what a DecalComponent with no material draws.
        constexpr const char* kDefaultDecalShaderName = "DefaultDecal";
        // The unit cube as 12 triangles from gl_VertexIndex (DefaultDecal.shader DecalBoxCorner); no vertex
        // stream.
        constexpr uint32_t kBoxVertexCount = 36u;
        // A decal shader's push block: DecalPush { uint FirstRow; three pads } (16 bytes); a material's own push
        // values may follow it, up to the 128 bytes every Vulkan device guarantees.
        constexpr std::size_t kDecalPushBytes = 16u;
        constexpr std::size_t kMaxPushBytes   = 128u;
        // Reserved once: a scene's decals grow the lists past this only once, never per frame.
        constexpr std::size_t kInitialDecals   = 256u;
        constexpr std::size_t kInitialPrograms = 16u;

        Image2D* ResolveTextureImage( uint64_t textureHandle )
        {
            if ( textureHandle == 0 )
                return nullptr;
            auto* textures = Runtime::ResourceRegistry::GetTextureService();
            auto* images   = Runtime::ResourceRegistry::GetImageService();
            if ( textures == nullptr || images == nullptr )
                return nullptr;
            auto* texture = textures->Get( Assets::AssetHandle( textureHandle ) );
            if ( texture == nullptr )
                return nullptr;
            return static_cast<Image2D*>( images->Resolve( texture->GetImageHandle() ) );
        }

        glm::vec4 UnitAxis( const glm::mat4& decalToWorld, const int column )
        {
            const glm::vec3 axis( decalToWorld[column] );
            const float     length = glm::length( axis );
            if ( length <= 0.0f )
                return glm::vec4( 0.0f );
            return glm::vec4( axis / length, 0.0f );
        }

        // The unit box against the view frustum (UE: the decal proxy's bounds in the view's frustum cull): culled
        // when all eight corners lie outside one clip plane. Each plane test is linear in the homogeneous corner,
        // so the test is exact for the convex box, behind the camera included; depth is the engine's 0..w range.
        bool BoxOutsideFrustum( const glm::mat4& decalToClip )
        {
            std::array<int, 6> outside{};
            for ( int corner = 0; corner < 8; ++corner )
            {
                const float     x = static_cast<float>( ( corner & 1 ) * 2 - 1 );
                const float     y = static_cast<float>( ( ( corner >> 1 ) & 1 ) * 2 - 1 );
                const float     z = static_cast<float>( ( ( corner >> 2 ) & 1 ) * 2 - 1 );
                const glm::vec4 clip = decalToClip * glm::vec4( x, y, z, 1.0f );
                outside[0] += static_cast<int>( clip.x < -clip.w );
                outside[1] += static_cast<int>( clip.x > clip.w );
                outside[2] += static_cast<int>( clip.y < -clip.w );
                outside[3] += static_cast<int>( clip.y > clip.w );
                outside[4] += static_cast<int>( clip.z < 0.0f );
                outside[5] += static_cast<int>( clip.z > clip.w );
            }
            return std::find( outside.begin(), outside.end(), 8 ) != outside.end();
        }
    } // namespace

    DecalRenderer::DecalRenderer()
    {
        m_Decals.reserve( kInitialDecals );
        m_Frame.reserve( kInitialDecals );
        m_Rows.reserve( kInitialDecals );
        m_Draws.reserve( kInitialDecals );
        m_Programs.reserve( kInitialPrograms );
        m_ProgramsByMaterial.reserve( kInitialPrograms );
    }

    void DecalRenderer::Add( const DecalProxy& proxy )
    {
        m_Decals.push_back( proxy );
    }

    void DecalRenderer::ClearQueue()
    {
        m_Decals.clear();
    }

    void DecalRenderer::PrepareFrame( const glm::mat4& viewProjection, const glm::vec3& cameraPosition,
                                      const float projectionYScale, PipelineCache& pipelines )
    {
        m_Frame.clear();
        m_Rows.clear();
        m_Draws.clear();
        m_Programs.clear();
        // What a shader reload replaced, released once no frame in flight can still draw with it.
        const auto& frames = ::Desert::Engine::FrameManager::GetInstance();
        m_Retired.Collect( frames.GetAbsoluteFrameCount(), frames.GetMaxFramesInFlight() );
        if ( m_Decals.empty() )
            return;

        const glm::mat4 inverseViewProjection = glm::inverse( viewProjection );
        for ( const DecalProxy& decal : m_Decals )
        {
            // A box scaled flat has no inside: nothing projects, and its inverse would be NaN.
            if ( std::abs( glm::determinant( decal.DecalToWorld ) ) < 1.0e-6f )
                continue;

            // UE: distance fade from FadeStart over FadeLength, and the screen-size cull of
            // ComputeBoundsScreenSize - the bounding sphere's diameter as a share of the screen height.
            const glm::vec3 center( decal.DecalToWorld[3] );
            const float     distance = glm::length( center - cameraPosition );
            float           opacity  = 1.0f;
            if ( decal.FadeLength > 0.0f )
                opacity = 1.0f - std::clamp( ( distance - decal.FadeStart ) / decal.FadeLength, 0.0f, 1.0f );
            const float radius =
                 std::sqrt( glm::dot( glm::vec3( decal.DecalToWorld[0] ), glm::vec3( decal.DecalToWorld[0] ) ) +
                            glm::dot( glm::vec3( decal.DecalToWorld[1] ), glm::vec3( decal.DecalToWorld[1] ) ) +
                            glm::dot( glm::vec3( decal.DecalToWorld[2] ), glm::vec3( decal.DecalToWorld[2] ) ) );
            const float screenSize = std::abs( projectionYScale ) * radius / std::max( distance, 1.0f );
            if ( opacity <= 0.0f || ( decal.FadeScreenSize > 0.0f && screenSize < decal.FadeScreenSize ) )
                continue;
            const glm::mat4 decalToClip = viewProjection * decal.DecalToWorld;
            if ( BoxOutsideFrustum( decalToClip ) )
                continue;

            const Program* program = ProgramFor( decal.Material, pipelines );
            if ( program == nullptr )
                continue;
            // The component may only mask channels OFF (ShaderProgramMeta.hpp DecalChannel).
            const uint32_t channels = decal.ChannelMask & program->Channels;
            if ( channels == 0u )
                continue;

            FrameDecal frame{
                 .SortOrder = decal.SortOrder, .EntityId = decal.EntityId, .Program = program, .Row = {} };
            frame.Row.DecalToClip = decalToClip;
            frame.Row.ClipToDecal = glm::inverse( decal.DecalToWorld ) * inverseViewProjection;
            frame.Row.AxisX       = UnitAxis( decal.DecalToWorld, 0 );
            frame.Row.AxisY       = UnitAxis( decal.DecalToWorld, 1 );
            frame.Row.AxisZ       = UnitAxis( decal.DecalToWorld, 2 );
            frame.Row.Params      = glm::vec4( opacity, decal.EdgeFade, static_cast<float>( channels ), 0.0f );
            m_Frame.push_back( frame );
        }

        // UE SortOrder, then the entity id: equal orders stack the same way every frame (a distance tie-break
        // would swap two overlapping decals as the camera walks past them). std::sort: in place, no allocation.
        std::sort( m_Frame.begin(), m_Frame.end(),
                   []( const FrameDecal& a, const FrameDecal& b )
                   {
                       if ( a.SortOrder != b.SortOrder )
                           return a.SortOrder < b.SortOrder;
                       return a.EntityId < b.EntityId;
                   } );

        for ( const FrameDecal& frame : m_Frame )
        {
            const auto row = static_cast<uint32_t>( m_Rows.size() );
            m_Rows.push_back( frame.Row );
            if ( !m_Draws.empty() && m_Draws.back().Program == frame.Program )
            {
                ++m_Draws.back().Count;
                continue;
            }
            m_Draws.push_back( Draw{ .Program = frame.Program, .FirstRow = row, .Count = 1u } );
            if ( std::find( m_Programs.begin(), m_Programs.end(), frame.Program ) == m_Programs.end() )
            {
                frame.Program->Block = static_cast<uint32_t>( m_Programs.size() );
                m_Programs.push_back( frame.Program );
            }
        }
    }

    const DecalRenderer::Program* DecalRenderer::ProgramFor( const Assets::AssetHandle& material,
                                                             PipelineCache&             pipelines )
    {
        const auto key = static_cast<uint64_t>( material );
        if ( const auto found = m_ProgramsByMaterial.find( key ); found != m_ProgramsByMaterial.end() )
        {
            Program& program = found->second;
            if ( program.Refused )
                return nullptr;
            FollowShaderReload( program, material, pipelines );
            return &program;
        }

        // A material still loading is asked again next frame rather than remembered as refused.
        if ( !material.IsNull() )
        {
            const auto* materials = Runtime::ResourceRegistry::GetMaterialService();
            if ( materials != nullptr && materials->IsPending( material ) )
                return nullptr;
        }

        Program& program = m_ProgramsByMaterial[key];
        if ( const auto built = Build( program, material, pipelines ); !built )
        {
            // Said once per material: the refusal describes the asset, not the frame.
            LOG_ERROR( "[Decals] material {}: {} - the decal draws nothing", key, built.GetError() );
            program.Pipeline.reset();
            program.Material.reset();
            program.Refused = true;
            return nullptr;
        }
        return &program;
    }

    void DecalRenderer::FollowShaderReload( Program& program, const Assets::AssetHandle& material,
                                            PipelineCache& pipelines )
    {
        // UIMaterialCache::FollowShaderReload's rule: a hot reload recompiles the shader in place and bumps its
        // code generation (PipelineCache::InvalidateByShader drops the cached pipeline); the program rebuilds
        // against the new code, and the replaced pipeline and material are parked until no frame in flight reads
        // them. A rebuild that fails keeps the previous build and is not retried until the next reload.
        if ( !program.Pipeline || !program.Pipeline->GetSpecification().Shader )
            return;
        const uint64_t generation = program.Pipeline->GetSpecification().Shader->GetCodeGeneration();
        if ( generation == program.Generation )
            return;
        std::shared_ptr<GraphicsPipeline>   previousPipeline = program.Pipeline;
        std::unique_ptr<DataDrivenMaterial> previousMaterial = std::move( program.Material );
        if ( const auto rebuilt = Build( program, material, pipelines ); !rebuilt )
        {
            LOG_ERROR( "[Decals] material {}: did not rebuild after its shader reloaded ({}); it keeps the "
                       "previous build",
                       static_cast<uint64_t>( material ), rebuilt.GetError() );
            program.Pipeline   = std::move( previousPipeline );
            program.Material   = std::move( previousMaterial );
            program.Generation = generation;
            return;
        }
        m_Retired.Park( RetiredProgram{ std::move( previousPipeline ), std::move( previousMaterial ) },
                        ::Desert::Engine::FrameManager::GetInstance().GetAbsoluteFrameCount() );
    }

    Common::BoolResultStr DecalRenderer::Build( Program& program, const Assets::AssetHandle& material,
                                                PipelineCache& pipelines ) const
    {
        auto* shaders = Runtime::ResourceRegistry::GetShaderService();
        if ( shaders == nullptr )
            return Common::MakeError( "no shader service" );
        auto* materials = Runtime::ResourceRegistry::GetMaterialService();

        std::string shaderName = kDefaultDecalShaderName;
        if ( !material.IsNull() )
        {
            if ( materials == nullptr )
                return Common::MakeError( "no material service" );
            const auto materialTemplate = materials->ShaderHandleOf( material );
            if ( materialTemplate.Shader.IsNull() || materialTemplate.CompileName.empty() )
                return Common::MakeError( "the handle names no material asset with a loaded template" );
            shaderName = materialTemplate.CompileName;
        }

        const auto shader = shaders->GetByName( shaderName );
        if ( !shader )
            return Common::MakeFormattedError( "no shader named '{}' is registered (the default decal is "
                                               "Editor/Resources/Shaders/Programs/Decals/DefaultDecal.shader)",
                                               shaderName );
        const auto& meta = shader->GetProgramMeta();
        if ( !Core::Formats::DrawnByDecalPath( meta.Domain ) )
            return Common::MakeFormattedError(
                 "shader '{}' has Domain {}, and a decal draws only a Domain {} shader", shaderName,
                 Core::Formats::ShaderDomainName( meta.Domain ),
                 Core::Formats::ShaderDomainName( Core::Formats::kDecalPathDomain ) );
        if ( const auto valid = program.Layout.Validate( shader, DecalShaderParameters::kAll ); !valid )
            return valid;

        // The DBuffer contract (Common/DBuffer.glslh): rgb = src + dst x (1 - src.a), alpha (the transmittance) =
        // dst x (1 - src.a). Back faces, no depth attachment: one fragment per covered pixel, also with the camera
        // inside the box; the receiver is the prepass depth the fragment samples.
        GraphicsPipelineSpecification spec;
        spec.DebugName           = "Decal:" + shaderName;
        spec.Shader              = shader;
        spec.TargetLayout        = DBufferLayout();
        spec.Topology            = PrimitiveTopology::Triangles;
        spec.CullMode            = CullMode::Front;
        spec.DepthTestEnabled    = false;
        spec.DepthWriteEnabled   = false;
        spec.BlendEnable         = true;
        spec.SrcColorBlendFactor = BlendFactor::One;
        spec.DstColorBlendFactor = BlendFactor::OneMinusSrcAlpha;
        spec.SrcAlphaBlendFactor = BlendFactor::Zero;
        spec.DstAlphaBlendFactor = BlendFactor::OneMinusSrcAlpha;
        const auto pipeline      = pipelines.GetOrCreate( spec );
        if ( !pipeline )
            return Common::MakeError( std::string( pipeline.GetError() ) );

        program.Pipeline   = pipeline.GetValue();
        program.Material   = std::make_unique<DataDrivenMaterial>( shader );
        program.Channels   = meta.DecalChannelMask;
        program.Generation = shader->GetCodeGeneration();
        if ( material.IsNull() )
            return BOOLSUCCESS;

        // The asset's values by name onto the decal's own runtime material (Render2D::UIMaterialCache's route).
        MaterialOverrides overrides;
        if ( !materials->ResolveOverrides( material, overrides ) )
            return BOOLSUCCESS;
        for ( const auto& [name, value] : overrides.Params )
            if ( !program.Material->SetParam( name, value ) )
                LOG_WARN( "[Decals] '{}' has no parameter '{}' - the value in the .demat is ignored", shaderName,
                          name );
        for ( const auto& [name, texture] : overrides.Textures )
        {
            Image2D* image = ResolveTextureImage( texture );
            if ( texture != 0 && image == nullptr )
                LOG_ERROR( "[Decals] '{}' binds texture {} to '{}' and it did not resolve; the slot keeps the "
                           "shader's default",
                           shaderName, texture, name );
            if ( !program.Material->SetTexture( name, image ) )
                LOG_WARN( "[Decals] '{}' has no Texture2D '{}' - the binding in the .demat is ignored", shaderName,
                          name );
        }
        return BOOLSUCCESS;
    }

    RDG::BufferRef DecalRenderer::UploadRows( RDG::Builder& graph ) const
    {
        const RDG::BufferRef rows =
             graph.CreateBuffer( RDG::BufferDesc{ m_Rows.size() * sizeof( DecalRowGpu ) }, "Decals.Rows" );
        graph.QueueBufferUpload( rows, std::as_bytes( std::span( m_Rows ) ) );
        return rows;
    }

    void DecalRenderer::Declare( RDG::PassBuilder& pass, const RDG::TextureRef sceneDepth,
                                 const RDG::BufferRef rows ) const
    {
        // Block i is m_Programs[i] (Program::Block, set by PrepareFrame): its shader's layout, its material's
        // route, and the pass's three inputs - each entry the declaration of its read.
        for ( const Program* program : m_Programs )
        {
            MaterialExecutor* executor = program->Material->GetMaterialExecutor();
            pass.Prepares( *executor );
            const auto layout = program->Layout.Get( program->Pipeline->GetSpecification().Shader );
            pass.Bindings( layout, executor->GetRouteFill() )
                 .Texture( DecalShaderParameters::SceneDepthTexture, sceneDepth, RDG::Access::SampledGraphics,
                           RDG::SubresourceRange::All() )
                 .Sampler( DecalShaderParameters::PointClampSampler, RDG::SamplerDesc::PointClamp() )
                 .Storage( DecalShaderParameters::DecalRowsBuffer, rows, RDG::Access::StorageRead );
        }
    }

    Common::BoolResultStr DecalRenderer::Render( const RDG::PassContext& context ) const
    {
        DESERT_PROFILE_PASS( "Deferred: Decals" );
        for ( const Draw& draw : m_Draws )
        {
            const Program&                program = *draw.Program;
            const RDG::PassBindings       bindings( context, context.GetBindingBlock( program.Block ) );
            const MaterialExecutor*       executor = program.Material->GetMaterialExecutor();
            const Common::Memory::Buffer& pushed   = executor->GetPushConstantBuffer();
            if ( pushed.Size > kMaxPushBytes )
                return Common::MakeFormattedError( "{}: decal shader '{}' has a {}-byte push block (at most {})",
                                                   context.GetPassName(), program.Material->GetShaderName(),
                                                   pushed.Size, kMaxPushBytes );
            // The draw's push block: the material's values with DecalPush.FirstRow over its first word; the
            // instance index walks the run's rows (DefaultDecal.shader DecalRows[FirstRow + gl_InstanceIndex]).
            std::array<std::byte, kMaxPushBytes> push{};
            if ( pushed.Data != nullptr && pushed.Size != 0u )
                std::memcpy( push.data(), pushed.Data, pushed.Size );
            std::memcpy( push.data(), &draw.FirstRow, sizeof( draw.FirstRow ) );
            const std::size_t                bytes = std::max<std::size_t>( pushed.Size, kDecalPushBytes );
            const std::span<const std::byte> block( push.data(), bytes );
            if ( auto drawn = Renderer::DrawProcedural( bindings, *program.Pipeline, executor, kBoxVertexCount,
                                                        draw.Count, block );
                 !drawn.IsSuccess() )
                return drawn;
        }
        return BOOLSUCCESS;
    }
} // namespace Desert::Graphic::System
