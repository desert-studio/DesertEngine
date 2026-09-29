#include "Material.hpp"
#include <Engine/Core/Formats/MaterialParamRow.hpp>
#include <Engine/Graphic/Materials/MaterialBinder.hpp>
#include <Engine/Graphic/DefaultTextures.hpp>
#include <Engine/Graphic/Image.hpp>

#include <Common/Core/Logger.hpp>

namespace Desert::Graphic
{
    bool Material::WritePushField( std::string_view field, const void* value, uint32_t size )
    {
        if ( !m_MaterialExecutor || !m_MaterialExecutor->GetShader() )
            return false;
        const auto& layout = m_MaterialExecutor->GetShader()->GetMaterialLayout();
        const auto  slot   = MaterialBinder::ResolvePush( layout, field, size );
        switch ( slot.Status )
        {
            case MaterialBinder::PushWrite::Written:
                m_MaterialExecutor->PushConstant( value, size, slot.Offset );
                return true;
            case MaterialBinder::PushWrite::SizeMismatch:
                LOG_ERROR( "Material '{}': push field '{}' is {} bytes in the shader, {} written; not written",
                           m_MaterialExecutor->GetDubugName(), field, layout.FindPush( field )->Size, size );
                return false;
            case MaterialBinder::PushWrite::Absent:
                return false;
        }
        return false;
    }

    const Core::Formats::MaterialLayout& Material::GetMaterialLayout() const
    {
        static const Core::Formats::MaterialLayout kNoLayout;
        if ( !m_MaterialExecutor || !m_MaterialExecutor->GetShader() )
            return kNoLayout;
        return m_MaterialExecutor->GetShader()->GetMaterialLayout();
    }

    void Material::SetMaterialIndex( uint32_t index )
    {
        WritePushField( "MaterialIndex", &index, sizeof( uint32_t ) );
    }

    void Material::SetPushMatrix( const glm::mat4& matrix )
    {
        WritePushField( "Transform", &matrix, sizeof( glm::mat4 ) );
    }

    void Material::SetInstancedWind( const InstanceWindPush& wind )
    {
        WritePushField( "WindA", &wind.A, sizeof( glm::vec4 ) );
        WritePushField( "WindB", &wind.B, sizeof( glm::vec4 ) );
    }

    Material::Material( std::string&& debugName, std::string&& shaderName )
         : m_MaterialExecutor(
                Graphic::MaterialExecutor::Create( std::move( debugName ), std::move( shaderName ) ) ),
           // The ledger row — see Engine/Graphic/ResourceLedger.hpp. A Material is the single most
           // expensive row in it: on the Vulkan backend one carries its own VkDescriptorPool, a
           // frames x slots grid of descriptor sets, and one uniform/storage buffer object per block its
           // shader declares, each of which is itself frames x slots VkBuffers.
           m_Accounting( ResourceOwnership::Take( ResourceKind::Material ) )
    {
        CachePropertyNames();
    }

    void Material::ClaimOwnership( const ResourceOwner owner, const Common::AssetHandle asset )
    {
        m_Accounting.Claim( owner, asset );

        // AND EVERYTHING THE MATERIAL BROUGHT WITH IT. A `Material` row is one object; the device
        // allocations it is responsible for are the uniform and storage buffers its shader declares, and
        // each of those is frames x slots VkBuffers. Leaving them out is not a rounding error — they were
        // 259 of the 338 rows the first census could not attribute to anybody, i.e. the single largest
        // reason the ledger looked half-blind. They are claimed HERE and not at their own construction
        // because that happens inside MaterialExecutor's constructor, which does not know whether the
        // material being built is an asset's or a render pass's.
        if ( !m_MaterialExecutor )
            return;

        for ( const auto& [name, index] : m_MaterialExecutor->GetUniformBufferProperties() )
        {
            if ( const auto property = m_MaterialExecutor->GetUniformBufferProperty( name ) )
            {
                if ( const auto& buffer = property->GetUniform() )
                    buffer->ClaimOwnership( owner, asset );
            }
        }

        for ( const auto& [name, index] : m_MaterialExecutor->GetStorageBufferProperties() )
        {
            if ( const auto property = m_MaterialExecutor->GetStorageBufferProperty( name ) )
            {
                if ( const auto& buffer = property->GetStorageBuffer() )
                    buffer->ClaimOwnership( owner, asset );
            }
        }
    }

    MaterialInstancePtr Material::CreateInstance( const std::string& name )
    {
        return std::make_shared<MaterialInstance>(
             this, name.empty() ? m_MaterialExecutor->GetDubugName() + "_Instance" : name );
    }

    void Material::RegisterProperty( IProperty* prop )
    {
        m_RegisteredProperties.push_back( prop );
    }

    bool Material::BindSchemaDefaultTexture( const std::string& sampler )
    {
        if ( !m_MaterialExecutor )
            return false;

        auto property = m_MaterialExecutor->GetTexture2DProperty( sampler );
        if ( !property )
            return false;

        // The schema is asked, never guessed. A sampler the shader declares by hand without a matching
        // `Properties` entry (StaticMeshPBR's environment maps, the cloud shadow map) has no authored
        // default and gets White -- the colour the backend fallback already held, so nothing moves.
        auto kind = Core::Formats::DefaultTextureKind::White;
        if ( const auto& shader = m_MaterialExecutor->GetShader() )
        {
            for ( const auto& param : shader->GetProgramMeta().Params )
            {
                if ( param.IsTexture && !param.IsCubeTexture && param.Name == sampler )
                {
                    kind = param.DefaultTexture;
                    break;
                }
            }
        }

        const Image2D* image = DefaultTextures::Get().Resolve( kind );
        if ( !image )
        {
            // Resolve() has already said WHICH kind failed; this line says which material and slot are
            // left holding the previous image, because that is the visible symptom (DC §1.4).
            LOG_ERROR( "[Materials] '{}' could not be given the '{}' default for its '{}' slot, so that "
                       "sampler keeps whatever was bound to it last",
                       m_MaterialExecutor->GetDubugName(), Core::Formats::DefaultTextureKindName( kind ),
                       sampler );
            return false;
        }

        property->SetImage( image );
        return true;
    }

    // ---------------------------------------------------------------------------

    std::pair<UniformBufferProperty*, FieldProperty*> Material::FindFieldInAnyUB(
         std::string_view fieldName ) const
    {
        const std::string key( fieldName );
        for ( const auto& [ubName, idx] : m_MaterialExecutor->GetUniformBufferProperties() )
        {
            auto ubProp = m_MaterialExecutor->GetUniformBufferProperty( ubName );
            if ( !ubProp )
                continue;
            if ( auto* field = ubProp->GetField( key ) )
                return { ubProp.get(), field };
        }
        return { nullptr, nullptr };
    }

    void Material::UploadRegisteredProperties()
    {
        for ( auto* prop : m_RegisteredProperties )
        {
            if ( !prop->IsDirty() )
                continue;

            if ( prop->GetKind() == PropertyKind::Texture2D )
            {
                void* texPtr = nullptr;
                prop->CopyValueTo( &texPtr );
                if ( texPtr )
                {
                    if ( auto texProp = m_MaterialExecutor->GetTexture2DProperty( std::string( prop->GetShaderName() ) ) )
                        texProp->SetImage( static_cast<const Image2D*>( texPtr ) );
                }
            }
            else
            {
                auto [ub, field] = FindFieldInAnyUB( prop->GetShaderName() );
                if ( ub && field )
                {
                    std::byte buf[256] = {};
                    prop->CopyValueTo( buf );
                    ub->WriteField( field, buf, prop->GetByteSize() );
                }
            }

            prop->MarkClean();
        }
    }

    void Material::ApplyInstanceOverrides( const MaterialInstance* instance )
    {
        const auto& props = instance->GetPropertySet();
        for ( const auto& [name, prop] : props.GetProperties() )
        {
            if ( !prop.bIsOverridden )
                continue;

            // Local alias: AppleClang 15 can't capture structured bindings in lambdas yet.
            const auto& propName = name;
            std::visit(
                 [&]( auto&& val )
                 {
                     using T = std::decay_t<decltype( val )>;
                     if constexpr ( std::is_same_v<T, void*> )
                     {
                         if ( val )
                         {
                             if ( auto texProp = m_MaterialExecutor->GetTexture2DProperty( propName ) )
                                 texProp->SetImage( static_cast<const Image2D*>( val ) );
                         }
                     }
                     else
                     {
                         auto [ub, field] = FindFieldInAnyUB( propName );
                         if ( ub && field )
                         {
                             ub->WriteField( field, &val, sizeof( T ) );
                         }
                     }
                 },
                 prop.Value );
        }
    }

    // Flush every UB that still has dirty fields. A field stays dirty for frames-in-flight frames, so
    // this writes the new data into EACH per-frame-in-flight buffer copy (not just the copy for the
    // frame it first changed on). Flushing only the UBs "touched" this frame would leave the other
    // copies at their initial zero contents, so any frame presenting those indices would render the
    // mesh black/garbage — the source of the per-frame flicker.
    //
    // Whole-filled buffers report no dirty fields and are therefore skipped without being listed
    // anywhere; that used to be DataDrivenMaterial's job to remember.
    void Material::FlushFieldFilledUniformBuffers()
    {
        if ( !m_MaterialExecutor )
            return;

        for ( const auto& [ubName, idx] : m_MaterialExecutor->GetUniformBufferProperties() )
        {
            auto ubProp = m_MaterialExecutor->GetUniformBufferProperty( ubName );
            if ( ubProp && ubProp->HasDirtyFields() )
                ubProp->UpdateFields();
        }
    }

    void Material::Bind( const MaterialInstance* instance )
    {
        if ( !m_MaterialExecutor )
            return;

        // 1. TProperty defaults → FieldProperty (only dirty ones)
        UploadRegisteredProperties();

        // 2. MaterialInstance overrides on top
        ApplyInstanceOverrides( instance );

        // 3. Get them onto the GPU.
        FlushFieldFilledUniformBuffers();

        OnBind( const_cast<MaterialInstance*>( instance ) );
    }

    void Material::CachePropertyNames()
    {
        if ( m_MaterialExecutor )
        {
            for ( const auto& [name, index] : m_MaterialExecutor->GetUniformBufferProperties() )
                m_PropertyNames.push_back( name );

            for ( const auto& [name, index] : m_MaterialExecutor->GetTexture2DProperties() )
                m_PropertyNames.push_back( name );

            for ( const auto& [name, index] : m_MaterialExecutor->GetTextureCubeProperties() )
                m_PropertyNames.push_back( name );
        }
    }

} // namespace Desert::Graphic
