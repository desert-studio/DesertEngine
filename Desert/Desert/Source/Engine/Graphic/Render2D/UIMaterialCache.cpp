#include "UIMaterialCache.hpp"

#include <Engine/Core/FrameManager.hpp>
#include <Engine/Core/Formats/ShaderProgramMeta.hpp>
#include <Engine/Graphic/Framebuffer.hpp>
#include <Engine/Graphic/Image.hpp>
#include <Engine/Graphic/Materials/MaterialOverrides.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/PipelineCache.hpp>
#include <Engine/Graphic/Render2D/DrawList2D.hpp>
#include <Engine/Graphic/Render2D/Render2DExecutorRetire.hpp>
#include <Engine/Graphic/Shader.hpp>
#include <Engine/Graphic/Texture.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Image/ImageService.hpp>
#include <Engine/Runtime/Services/Material/MaterialService.hpp>
#include <Engine/Runtime/Services/Shader/ShaderService.hpp>
#include <Engine/Runtime/Services/Texture/TextureService.hpp>

namespace Desert::Graphic::Render2D
{
    namespace
    {
        // The shipped fill an element draws when its material slot cannot be executed. Named here rather
        // than spelled at three call sites so the refusal and the picture cannot describe different things.
        constexpr const char* kErrorShaderName = "UIMatError";

        // The vertex layout of the 2D batcher — pos / uv / straight RGBA, i.e. DrawList2D::Vertex2D. It is
        // the SAME layout Render2D::Init builds its own three pipelines with; a UI material draws the
        // batcher's own geometry, so a second spelling of it here would be a second thing to keep level.
        VertexBufferLayout UILayout()
        {
            return VertexBufferLayout( { VertexBufferElement( ShaderDataType::Float2, "a_Position" ),
                                         VertexBufferElement( ShaderDataType::Float2, "a_TexCoord" ),
                                         VertexBufferElement( ShaderDataType::Float4, "a_Color" ) } );
        }

        Image2D* ResolveTextureImage( uint64_t textureHandle )
        {
            if ( textureHandle == 0 )
                return nullptr;
            auto* texService = Runtime::ResourceRegistry::GetTextureService();
            auto* imgService = Runtime::ResourceRegistry::GetImageService();
            if ( !texService || !imgService )
                return nullptr;
            auto* tex = texService->Get( Assets::AssetHandle( textureHandle ) );
            if ( !tex )
                return nullptr;
            return static_cast<Image2D*>( imgService->Resolve( tex->GetImageHandle() ) );
        }
    } // namespace

    UIMaterialCache::Entry UIMaterialCache::Build( const std::string& shaderName, std::string& refusal ) const
    {
        Entry entry;

        auto* shaderService = Runtime::ResourceRegistry::GetShaderService();
        if ( !shaderService )
        {
            refusal = "no shader service";
            return entry;
        }

        const auto shader = shaderService->GetByName( shaderName );
        if ( !shader )
        {
            refusal = "no shader named '" + shaderName + "' is registered";
            return entry;
        }

        const auto& meta = shader->GetProgramMeta();
        if ( !Core::Formats::DrawnByUIPath( meta.Domain ) )
        {
            // THE REFUSAL UE NEVER WROTE. A Surface material in a UI slot compiles and links perfectly
            // well; what it does not get is a camera, a world position or a normal, because the UI path
            // binds none of them. Naming both domains is what makes the message actionable — the author
            // has to know which one the slot wanted, not only that this one was wrong.
            refusal = std::string( "shader '" ) + shaderName + "' has Domain " +
                      Core::Formats::ShaderDomainName( meta.Domain ) +
                      ", and a UI element can only fill "
                      "itself with a Domain " +
                      Core::Formats::ShaderDomainName( Core::Formats::kUIPathDomain ) + " material";
            return entry;
        }

        GraphicsPipelineSpecification spec;
        spec.DebugName   = "UIMat_" + shaderName;
        spec.Shader      = shader;
        spec.Framebuffer = m_Target;
        spec.Layout      = UILayout();
        spec.Topology    = PrimitiveTopology::Triangles;
        // The UI PHASE's own state, and not the material's to choose: the batcher draws a flat overlay
        // into the scene target through a LOAD pass. Everything below this line the shader may override,
        // and ApplyShaderRenderState touches only what the shader actually declared.
        spec.CullMode          = CullMode::None;
        spec.DepthTestEnabled  = false;
        spec.DepthWriteEnabled = false;
        spec.BlendEnable       = true;
        spec.UseLoadRenderPass = true;
        ApplyShaderRenderState( spec, meta.State );

        const auto pipeline = GraphicsPipeline::Create( spec );
        if ( !pipeline )
        {
            refusal = "pipeline for '" + shaderName + "' — " + pipeline.GetError();
            return entry;
        }

        entry.Pipeline = pipeline.GetValue();
        entry.Material = std::make_unique<DataDrivenMaterial>( shaderName );
        return entry;
    }

    const UIMaterialCache::Entry* UIMaterialCache::ErrorEntry()
    {
        if ( m_Error )
            return m_Error.get();

        std::string refusal;
        Entry       built = Build( kErrorShaderName, refusal );
        if ( !built.Pipeline )
        {
            // The error fill itself failed to build. Say so once and return nothing: the element then
            // draws NOTHING, which is the outcome this whole class exists to avoid — so the log line is
            // the only thing standing between the author and a mystery, and it must not be swallowed.
            LOG_ERROR( "[UIMaterial] the error fill '{}' could not be built ({}), so an element with a "
                       "broken material slot draws nothing at all",
                       kErrorShaderName, refusal );
            return nullptr;
        }
        built.Error = true;
        m_Error     = std::make_unique<Entry>( std::move( built ) );
        return m_Error.get();
    }

    void UIMaterialCache::Rebuild( const std::shared_ptr<Framebuffer>& target )
    {
        m_Target = target;

        // The MATERIALS survive a target change and the PIPELINES cannot: a pipeline is compiled against
        // a render pass, and Scene::Init has just made a new one. Rebuilding in place keeps every
        // resolved row and every bound sampler, so a canvas does not flicker back to defaults on a resize.
        for ( auto& [handle, entry] : m_Entries )
        {
            if ( !entry.Material )
                continue;
            std::string refusal;
            Entry       rebuilt = Build( entry.Material->GetShaderName(), refusal );
            if ( rebuilt.Pipeline )
            {
                entry.Pipeline = std::move( rebuilt.Pipeline );
                continue;
            }
            LOG_ERROR( "[UIMaterial] '{}' lost its pipeline on a target change and now draws the error "
                       "fill — {}",
                       entry.Material->GetShaderName(), refusal );
            entry.Pipeline = nullptr;
        }

        if ( m_Error )
        {
            std::string refusal;
            Entry       rebuilt = Build( kErrorShaderName, refusal );
            m_Error->Pipeline   = rebuilt.Pipeline; // null here is reported by ErrorEntry's caller path
            if ( !rebuilt.Pipeline )
                LOG_ERROR( "[UIMaterial] the error fill lost its pipeline on a target change — {}", refusal );
        }
    }

    const UIMaterialCache::Entry* UIMaterialCache::Resolve( const Assets::AssetHandle& handle )
    {
        if ( handle.IsNull() || !m_Target )
            return nullptr;

        const uint64_t frame = Engine::FrameManager::GetInstance().GetAbsoluteFrameCount();

        const auto hit = m_Entries.find( handle );
        if ( hit != m_Entries.end() )
        {
            hit->second.LastUsedFrame = frame;
            if ( hit->second.Pipeline )
                return &hit->second;
            return ErrorEntry();
        }

        auto* materialService = Runtime::ResourceRegistry::GetMaterialService();
        if ( !materialService )
            return ErrorEntry();

        const std::string shaderName = materialService->ShaderNameOf( handle );
        std::string       refusal;
        Entry             built;
        if ( shaderName.empty() )
            refusal = "the handle names no material asset (it was deleted, or never registered)";
        else
            built = Build( shaderName, refusal );

        if ( !built.Pipeline )
        {
            // Once per handle. The picture repeats the complaint every frame; the log does not have to.
            if ( m_Reported.find( handle ) == m_Reported.end() )
            {
                m_Reported.emplace( handle, refusal );
                LOG_ERROR( "[UIMaterial] element material {} draws the error fill: {}",
                           static_cast<uint64_t>( handle ), refusal );
            }
            return ErrorEntry();
        }

        // The asset's authored values, flattened through the instance chain (base first, child last).
        // Applied by NAME, so a parameter the shader no longer declares is dropped by SetParam rather
        // than shifting every row slot after it.
        MaterialOverrides overrides;
        if ( materialService->ResolveOverrides( handle, overrides ) )
        {
            for ( const auto& [name, value] : overrides.Params )
                if ( !built.Material->SetParam( name, value ) )
                    LOG_WARN( "[UIMaterial] '{}' has no parameter '{}' — the value in the .demat is "
                              "ignored",
                              shaderName, name );
            for ( const auto& [name, texture] : overrides.Textures )
            {
                // A HANDLE THAT NAMES A TEXTURE AND A HANDLE THAT NAMES NOTHING ARE NOT THE SAME SLOT,
                // and DataDrivenMaterial::SetTexture cannot tell them apart: it takes null to mean "the
                // shader's own default", which is exactly right for a slot the author cleared and
                // exactly wrong for one whose texture failed to resolve. Both then draw the schema's
                // white, and only one of them is what the file says. Asked HERE, where the handle is
                // still in hand, because this is the last place that knows the difference.
                Image2D* image = ResolveTextureImage( texture );
                if ( texture != 0 && !image )
                    LOG_ERROR( "[UIMaterial] '{}' binds texture {} to '{}' and it did not resolve; the "
                               "slot falls back to the shader's own default and the surface will not "
                               "look like the file says",
                               shaderName, texture, name );
                if ( !built.Material->SetTexture( name, image ) )
                    LOG_WARN( "[UIMaterial] '{}' has no Texture2D '{}' — the binding in the .demat is "
                              "ignored",
                              shaderName, name );
            }
        }

        built.LastUsedFrame = frame;
        auto [it, inserted] = m_Entries.emplace( handle, std::move( built ) );
        return &it->second;
    }

    void UIMaterialCache::RetireUnused()
    {
        const uint64_t frame  = Engine::FrameManager::GetInstance().GetAbsoluteFrameCount();
        const uint32_t window = ExecutorRetireWindow();

        for ( auto it = m_Entries.begin(); it != m_Entries.end(); )
        {
            if ( MayRetireExecutor( it->second.LastUsedFrame, frame, window ) )
                it = m_Entries.erase( it );
            else
                ++it;
        }
    }
} // namespace Desert::Graphic::Render2D
