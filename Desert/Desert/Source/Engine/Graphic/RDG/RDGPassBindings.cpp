#include <Engine/Graphic/RDG/RDGPassBindings.hpp>

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cstring>

namespace Desert::Graphic::RDG
{
    namespace
    {
        bool IsSampledAccess( Access access )
        {
            return access == Access::SampledCompute || access == Access::SampledGraphics;
        }

        bool IsStorageAccess( Access access )
        {
            return access == Access::StorageRead || access == Access::StorageWrite;
        }
    } // namespace

    PassBindings::PassBindings( const PassContext& context ) : m_Context( context )
    {
    }

    // Every failure goes through here so the message has one shape: "<pass>: '<slot>' <- '<resource>': <why>".
    // Only the first one is kept; a later entry never overwrites it.
    static void KeepFirstError( std::string& firstError, std::string_view pass, std::string_view shaderName,
                                std::string_view resource, std::string_view why )
    {
        if ( firstError.empty() )
            firstError = fmt::format( "{}: '{}' <- '{}': {}", pass, shaderName, resource, why );
    }

    PassBindings& PassBindings::Sampled( std::string_view shaderName, TextureRef texture, Access declared,
                                         SubresourceRange range )
    {
        if ( !m_FirstError.empty() )
            return *this;
        const std::string_view pass   = m_Context.GetPassName();
        const std::string      handle = fmt::format( "texture #{}", texture.Index );
        if ( !IsSampledAccess( declared ) )
        {
            KeepFirstError( m_FirstError, pass, shaderName, handle,
                            fmt::format( "a sampled entry is declared as {}, not SampledCompute / SampledGraphics",
                                         GetAccessName( declared ) ) );
            return *this;
        }
        const bool taken = std::any_of( m_Textures.begin(), m_Textures.end(),
                                        [&]( const BoundTexture& t ) { return t.ShaderName == shaderName; } ) ||
                           std::any_of( m_Buffers.begin(), m_Buffers.end(),
                                        [&]( const BoundBuffer& b ) { return b.ShaderName == shaderName; } );
        if ( taken )
        {
            KeepFirstError( m_FirstError, pass, shaderName, handle, "the slot is already bound in this block" );
            return *this;
        }
        const Common::ResultStr<TextureBinding> resolved = m_Context.GetTexture( texture, declared, range );
        if ( !resolved.IsSuccess() )
        {
            KeepFirstError( m_FirstError, pass, shaderName, handle, resolved.GetError() );
            return *this;
        }
        m_Textures.push_back( BoundTexture{ .ShaderName = std::string( shaderName ),
                                            .Kind       = ShaderResourceKind::SampledTexture,
                                            .Texture    = resolved.GetValue(),
                                            .Range      = range,
                                            .Declared   = declared } );
        return *this;
    }

    PassBindings& PassBindings::Storage( std::string_view shaderName, TextureRef texture, Access declared,
                                         uint32_t mip )
    {
        if ( !m_FirstError.empty() )
            return *this;
        const std::string_view pass   = m_Context.GetPassName();
        const std::string      handle = fmt::format( "texture #{}", texture.Index );
        if ( !IsStorageAccess( declared ) )
        {
            KeepFirstError( m_FirstError, pass, shaderName, handle,
                            fmt::format( "a storage image entry is declared as {}, not StorageRead / StorageWrite",
                                         GetAccessName( declared ) ) );
            return *this;
        }
        const bool taken = std::any_of( m_Textures.begin(), m_Textures.end(),
                                        [&]( const BoundTexture& t ) { return t.ShaderName == shaderName; } ) ||
                           std::any_of( m_Buffers.begin(), m_Buffers.end(),
                                        [&]( const BoundBuffer& b ) { return b.ShaderName == shaderName; } );
        if ( taken )
        {
            KeepFirstError( m_FirstError, pass, shaderName, handle, "the slot is already bound in this block" );
            return *this;
        }
        const SubresourceRange                  range    = SubresourceRange::Mip( mip );
        const Common::ResultStr<TextureBinding> resolved = m_Context.GetTexture( texture, declared, range );
        if ( !resolved.IsSuccess() )
        {
            KeepFirstError( m_FirstError, pass, shaderName, handle, resolved.GetError() );
            return *this;
        }
        m_Textures.push_back( BoundTexture{ .ShaderName = std::string( shaderName ),
                                            .Kind       = ShaderResourceKind::StorageTexture,
                                            .Texture    = resolved.GetValue(),
                                            .Range      = range,
                                            .Declared   = declared } );
        return *this;
    }

    // Shared by Uniform and the buffer Storage: the kind decides which accesses are legal.
    static void AddBuffer( const PassContext& context, std::vector<BoundTexture>& textures,
                           std::vector<BoundBuffer>& buffers, std::string& firstError, std::string_view shaderName,
                           BufferRef buffer, Access declared, ShaderResourceKind kind )
    {
        if ( !firstError.empty() )
            return;
        const std::string_view pass   = context.GetPassName();
        const std::string      handle = fmt::format( "buffer #{}", buffer.Index );
        const bool             legal  = kind == ShaderResourceKind::UniformBuffer ? declared == Access::UniformRead
                                                                                  : IsStorageAccess( declared );
        if ( !legal )
        {
            KeepFirstError(
                 firstError, pass, shaderName, handle,
                 fmt::format( "a {} entry cannot be declared as {}",
                              kind == ShaderResourceKind::UniformBuffer ? "uniform buffer" : "storage buffer",
                              GetAccessName( declared ) ) );
            return;
        }
        const bool taken = std::any_of( textures.begin(), textures.end(),
                                        [&]( const BoundTexture& t ) { return t.ShaderName == shaderName; } ) ||
                           std::any_of( buffers.begin(), buffers.end(),
                                        [&]( const BoundBuffer& b ) { return b.ShaderName == shaderName; } );
        if ( taken )
        {
            KeepFirstError( firstError, pass, shaderName, handle, "the slot is already bound in this block" );
            return;
        }
        const Common::ResultStr<BufferBinding> resolved = context.GetBuffer( buffer, declared );
        if ( !resolved.IsSuccess() )
        {
            KeepFirstError( firstError, pass, shaderName, handle, resolved.GetError() );
            return;
        }
        buffers.push_back( BoundBuffer{ .ShaderName = std::string( shaderName ),
                                        .Kind       = kind,
                                        .Buffer     = resolved.GetValue(),
                                        .Declared   = declared } );
    }

    PassBindings& PassBindings::Uniform( std::string_view shaderName, BufferRef buffer )
    {
        AddBuffer( m_Context, m_Textures, m_Buffers, m_FirstError, shaderName, buffer, Access::UniformRead,
                   ShaderResourceKind::UniformBuffer );
        return *this;
    }

    PassBindings& PassBindings::Storage( std::string_view shaderName, BufferRef buffer, Access declared )
    {
        AddBuffer( m_Context, m_Textures, m_Buffers, m_FirstError, shaderName, buffer, declared,
                   ShaderResourceKind::StorageBuffer );
        return *this;
    }

    PassBindings& PassBindings::PushConstants( const void* data, uint32_t size )
    {
        if ( !m_FirstError.empty() )
            return *this;
        if ( !m_PushConstants.empty() )
        {
            KeepFirstError( m_FirstError, m_Context.GetPassName(), "push constants", "block",
                            "the push-constant block is already set in this block" );
            return *this;
        }
        if ( size == 0 || data == nullptr )
        {
            KeepFirstError( m_FirstError, m_Context.GetPassName(), "push constants", "block",
                            "an empty push-constant block" );
            return *this;
        }
        m_PushConstants.resize( size );
        std::memcpy( m_PushConstants.data(), data, size );
        return *this;
    }

    Common::BoolResultStr PassBindings::GetStatus() const
    {
        if ( !m_FirstError.empty() )
            return Common::MakeError( m_FirstError );
        return Common::MakeSuccess( true );
    }

    const PassContext& PassBindings::GetContext() const
    {
        return m_Context;
    }

    std::span<const BoundTexture> PassBindings::GetTextures() const
    {
        return m_Textures;
    }

    std::span<const BoundBuffer> PassBindings::GetBuffers() const
    {
        return m_Buffers;
    }

    std::span<const std::byte> PassBindings::GetPushConstants() const
    {
        return m_PushConstants;
    }
} // namespace Desert::Graphic::RDG
