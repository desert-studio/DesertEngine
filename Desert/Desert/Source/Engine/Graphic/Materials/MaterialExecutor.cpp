#include <algorithm>
#include <Engine/Graphic/Materials/MaterialExecutor.hpp>
#include <Engine/Graphic/Materials/Material.hpp>
#include <Engine/Graphic/Materials/SceneResources.hpp>
#include <Engine/Core/FrameManager.hpp>
#include <Engine/Graphic/RendererAPI.hpp>
#include <Engine/Core/Formats/MaterialLayout.hpp>

#include <Engine/Graphic/API/Vulkan/VulkanMaterialBackend.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanShader.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <Engine/ShaderResources/ShaderResourcesManager.hpp>

#include <cstring>
#include <format>

namespace Desert::Graphic
{
    namespace
    {
        // The push block of a Vulkan program, by the one function its pipeline's layout range uses.
        uint32_t VulkanPushBlockSize( const std::shared_ptr<Shader>& shader )
        {
            if ( !shader )
            {
                return 0u;
            }
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): Vulkan branch, Vulkan shader
            auto* vulkanShader = static_cast<API::Vulkan::VulkanShader*>( shader.get() );
            return ShaderResources::ShaderLayout::PushBlockSize( vulkanShader->GetShaderPushConstant() );
        }
    } // namespace

    MaterialExecutor::MaterialExecutor( std::string&& debugName, const std::shared_ptr<Shader>& shader,
                                        const Core::Formats::ShaderProgramMeta& parameterSchema,
                                        std::unique_ptr<MaterialBackend>&&      materialBackend,
                                        uint32_t                                pushBlockSize )
         : m_DebugName( std::move( debugName ) ), m_MaterialBackend( std::move( materialBackend ) ),
           m_Shader( shader )
    {
        // Exactly the shader's push block — the pipeline's layout range — so what the material pushes is
        // never longer than the range it is pushed through. Zeroed so the bytes a draw does not write are
        // defined.
        m_PushConstantBuffer.Allocate( pushBlockSize );
        m_PushConstantBuffer.ZeroInitialize();
        InitializeProperties( parameterSchema );
    }

    void MaterialExecutor::PushConstant( const void* buffer, uint32_t bufferSize, uint32_t offset )
    {
        // Not Buffer::Write: that sets Size to the LAST write's length, and the block is the shader's whole
        // range whatever was written last.
        if ( static_cast<std::size_t>( offset ) + bufferSize > m_PushConstantBuffer.AllocatedSize )
        {
            if ( !m_ReportedPushOverflow )
            {
                m_ReportedPushOverflow = true;
                LOG_ERROR(
                     "Material '{}': a push write of {} bytes at offset {} is past shader '{}''s {}-byte push "
                     "block; refused (reported once per material)",
                     m_DebugName, bufferSize, offset, m_Shader ? m_Shader->GetName() : std::string( "<none>" ),
                     m_PushConstantBuffer.AllocatedSize );
            }
            return;
        }
        std::memcpy( static_cast<std::byte*>( m_PushConstantBuffer.Data ) + offset, buffer, bufferSize );
    }

    void MaterialExecutor::InitializeProperties( const Core::Formats::ShaderProgramMeta& parameterSchema )
    {
        {
            // The property names change here: the kept route fill is made again on its next request.
            const std::scoped_lock lock( m_RouteFillLock );
            m_RouteFill.reset();
        }
        // THE MATERIAL OWNS EXACTLY ITS PROPERTIES. A sampler the shader declares and the schema's
        // Properties block does not is a pass parameter (shadow cascades, environment cubes, BRDF LUT, cloud
        // shadow map, scene textures): the pass binds it through RDG::PassBindings, and no property exists
        // here for anything to write into -- "filled both" cannot be built.
        const auto parameterFor = [&]( const std::string& name, bool cube )
        { return Core::Formats::FindMaterialTextureParameter( parameterSchema, name, cube ); };

        auto uniformManager =
             ShaderResources::ShaderResourcesManager::Create( "Material_" + m_Shader->GetName(), m_Shader );

        // THE NAME AND THE ANSWER COME FROM THE SAME MANAGER, so a miss here is a broken invariant
        // rather than an ordinary failure — and that is precisely why these four unwraps went
        // unchecked for so long. They stopped being safe the day the registrars learned to REFUSE:
        // a resource that could not be created is no longer registered under its name, so the name
        // list and the lookup can now disagree by design. Skipping the entry keeps the two property
        // vectors and their lookup maps consistent with each other, which is what the draw path
        // indexes; building a property around nothing would have put a null in the vector and moved
        // the crash to the first frame that drew with this material.
        const auto addProperties = [&]( const auto& names, auto&& admit, auto&& fetch, auto&& make, auto& storage,
                                        auto& lookup, const char* kind )
        {
            for ( auto [name, index] : names )
            {
                if ( !admit( name ) )
                    continue;
                auto resource = fetch( name );
                if ( !resource )
                {
                    LOG_ERROR( "[MaterialExecutor] shader '{}' declares {} '{}', but it is not "
                               "registered: {}. The material is built WITHOUT it.",
                               m_Shader->GetName(), kind, name, resource.GetError() );
                    continue;
                }
                storage.push_back( make( name, resource.GetValue() ) );
                lookup[name] = storage.size() - 1;
            }
        };

        addProperties(
             uniformManager->GetUniformBufferTotal().Names, []( const std::string& ) { return true; },
             [&]( const std::string& n ) { return uniformManager->GetUniformBuffer( n ); },
             []( const std::string&, const auto& r ) { return std::make_shared<UniformBufferProperty>( r ); },
             m_UniformBufferPropertiesStorage, m_UniformBufferPropertiesLookup, "uniform buffer" );

        addProperties(
             uniformManager->GetStorageBufferTotal().Names, []( const std::string& ) { return true; },
             [&]( const std::string& n ) { return uniformManager->GetStorageBuffer( n ); },
             []( const std::string&, const auto& r ) { return std::make_shared<StorageBufferProperty>( r ); },
             m_StorageBufferPropertiesStorage, m_StorageBufferPropertiesLookup, "storage buffer" );

        addProperties(
             uniformManager->GetUniformImageCubeTotal().Names,
             [&]( const std::string& n ) { return parameterFor( n, true ) != nullptr; },
             [&]( const std::string& n ) { return uniformManager->GetUniformImageCube( n ); },
             []( const std::string&, const auto& r ) { return std::make_shared<TextureCubeProperty>( r ); },
             m_TextureCubePropertiesStorage, m_TextureCubePropertiesLookup, "image cube" );

        addProperties(
             uniformManager->GetUniformImage2DTotal().Names, [&]( const std::string& n )
             { return parameterFor( n, false ) != nullptr; }, [&]( const std::string& n )
             { return uniformManager->GetUniformImage2D( n ); }, [&]( const std::string& n, const auto& r )
             { return std::make_shared<Texture2DProperty>( r, parameterFor( n, false )->DefaultTexture ); },
             m_Texture2DPropertiesStorage, m_Texture2DPropertiesLookup, "image2D" );

        // ROW TEXTURES have no descriptor to reflect: the shader reads them from the material row through the
        // bindless table (Core::Formats::IsRowTexture, BuildMaterialLayout). Their property holds the image and
        // the sampler state the row slot is written from (DataDrivenMaterial::GetParamRow), so assigning a
        // texture is the same call for both kinds. ReconcileMaterialLayout refuses a sampler of the same name.
        for ( const auto& param : parameterSchema.Params )
        {
            if ( !Core::Formats::IsRowTexture( parameterSchema.LayoutBindings, param ) ||
                 m_Texture2DPropertiesLookup.contains( param.Name ) )
                continue;
            m_Texture2DPropertiesStorage.push_back(
                 std::make_shared<Texture2DProperty>( nullptr, param.DefaultTexture ) );
            m_Texture2DPropertiesLookup[param.Name] = m_Texture2DPropertiesStorage.size() - 1;
        }
        ResolveSceneBlocks();
    }

    void MaterialExecutor::ResolveSceneBlocks() noexcept
    {
        // The names are the writers' own parameters (SceneLightingBinding, SceneFrameBinding, CloudShadowBinding),
        // looked up once here instead of once per material per frame.
        namespace SR = SceneResources;
        namespace SP = ShaderProtocols;
        m_SceneBlocks = SceneBlocks{
             .Camera             = FindUniformBufferProperty( SP::Camera::Parameter.Name ),
             .Time               = FindUniformBufferProperty( SR::kTimeBlock.Name ),
             .PointLights        = FindStorageBufferProperty( SP::PointLight::Parameter.Name ),
             .SpotLights         = FindStorageBufferProperty( SP::SpotLight::Parameter.Name ),
             .DirectionLights    = FindUniformBufferProperty( SP::DirectionLight::Parameter.Name ),
             .LightsMetadata     = FindUniformBufferProperty( SP::LightsMetadata::Parameter.Name ),
             .Shadow             = FindUniformBufferProperty( SR::kShadowBlock.Name ),
             .SkyLook            = FindUniformBufferProperty( kSkyLookBlock.Name ),
             .CloudShadow        = FindUniformBufferProperty( SR::kCloudShadowBlock.Name ),
             .RainOcclusion      = FindUniformBufferProperty( SR::kRainOcclusionBlock.Name ),
             .GpuScenePrimitives = FindStorageBufferProperty( SR::kGpuScenePrimitives.Name ),
             .ObjectBones        = FindStorageBufferProperty( SR::kObjectBones.Name ),
             .WorldParameters    = FindStorageBufferProperty( SR::kWorldParameters.Name ) };
    }

    void MaterialExecutor::RouteFillInputs( std::vector<uint8_t>& inputs ) const
    {
        inputs.clear();
        uint8_t push = 0;
        if ( m_PushConstantBuffer.Size != 0u )
            push = 1;
        inputs.push_back( push );
        for ( const auto& [name, index] : m_StorageBufferPropertiesLookup )
        {
            uint8_t written = 0;
            if ( m_StorageBufferPropertiesStorage[index]->IsWritten() )
                written = 1;
            inputs.push_back( written );
        }
        const auto samplerOf = []( const auto& uniform ) -> uint8_t
        {
            if ( uniform && uniform->GetSamplerBinding() )
                return 2;
            return 0;
        };
        for ( const auto& [name, index] : m_Texture2DPropertiesLookup )
        {
            const auto& property = m_Texture2DPropertiesStorage[index];
            if ( property->IsRowTexture() )
                inputs.push_back( 1 );
            else
                inputs.push_back( samplerOf( property->GetUniform() ) );
        }
        for ( const auto& [name, index] : m_TextureCubePropertiesLookup )
            inputs.push_back( samplerOf( m_TextureCubePropertiesStorage[index]->GetUniform() ) );
    }

    std::shared_ptr<const RDG::OtherRouteFill> MaterialExecutor::GetRouteFill() const
    {
        // Per thread, so a steady frame allocates nothing to compare.
        thread_local std::vector<uint8_t> t_Inputs;
        RouteFillInputs( t_Inputs );
        const std::scoped_lock lock( m_RouteFillLock );
        if ( m_RouteFill && t_Inputs == m_RouteFillInputs )
            return m_RouteFill;
        RDG::OtherRouteFill fill;
        fill.PushConstants = m_PushConstantBuffer.Size != 0u;
        const auto add     = [&fill]( const auto& lookup )
        {
            for ( const auto& [name, index] : lookup )
                fill.Slots.push_back( name );
        };
        add( m_UniformBufferPropertiesLookup );
        // A storage buffer nothing ever wrote is NOT filled: its slot is then "filled by neither the pass nor the
        // material" in the pass's setup validation, before any draw is recorded.
        for ( const auto& [name, index] : m_StorageBufferPropertiesLookup )
            if ( m_StorageBufferPropertiesStorage[index]->IsWritten() )
                fill.Slots.push_back( name );
        // A row texture fills no slot of the pass: it is a row value read through the bindless table. A bound
        // texture fills its separate texture and its own sampler state (`<name>Sampler`).
        const auto addTexture = [&fill]( const std::string& name, const auto& uniform )
        {
            fill.Slots.push_back( name );
            if ( uniform && uniform->GetSamplerBinding() )
                fill.Slots.push_back( std::format( "{}{}", name, Core::Formats::kMaterialTextureSamplerSuffix ) );
        };
        for ( const auto& [name, index] : m_Texture2DPropertiesLookup )
            if ( !m_Texture2DPropertiesStorage[index]->IsRowTexture() )
                addTexture( name, m_Texture2DPropertiesStorage[index]->GetUniform() );
        for ( const auto& [name, index] : m_TextureCubePropertiesLookup )
            addTexture( name, m_TextureCubePropertiesStorage[index]->GetUniform() );
        // Deterministic, so a validation message does not depend on hash-map order.
        std::sort( fill.Slots.begin(), fill.Slots.end() );
        m_RouteFillInputs = t_Inputs;
        m_RouteFill       = std::make_shared<const RDG::OtherRouteFill>( std::move( fill ) );
        return m_RouteFill;
    }

    void MaterialExecutor::ApplyForRecording() const
    {
        MaterialBackend* backend = m_MaterialBackend.get();

        // 1. The owner's dirty MPROPERTY values into their fields (a no-op after a Bind, which cleaned them).
        if ( m_Owner )
            m_Owner->UploadRegisteredProperties();

        // 2. Fields into the active copy. Only buffers that claimed the field route: an unclaimed buffer's fields
        //    are the constructor's zeros (FieldProperty starts every field dirty), and flushing them would claim
        //    the route for a buffer someone else fills whole. Idempotent per copy (UpdateFields writes only
        //    fields newer than the copy's applied version), so a Bind just before costs nothing more here.
        for ( const auto& prop : m_UniformBufferPropertiesStorage )
        {
            if ( prop->GetFillKind() == ShaderResources::FillKind::Fields && prop->HasDirtyFields() )
                prop->UpdateFields();
        }

        // 3. The active view's descriptors.
        for ( const auto& prop : m_UniformBufferPropertiesStorage )
            prop->Apply( backend );
        for ( const auto& prop : m_Texture2DPropertiesStorage )
            prop->Apply( backend );
        for ( const auto& prop : m_TextureCubePropertiesStorage )
            prop->Apply( backend );
        for ( const auto& prop : m_StorageBufferPropertiesStorage )
            prop->Apply( backend );

        backend->FlushUpdates();
    }

    uint64_t MaterialExecutor::NewestPropertyWrite() const
    {
        uint64_t newest = 0;
        for ( const auto& prop : m_UniformBufferPropertiesStorage )
            newest = std::max( newest, prop->GetVersion() );
        for ( const auto& prop : m_Texture2DPropertiesStorage )
            newest = std::max( newest, prop->GetVersion() );
        for ( const auto& prop : m_TextureCubePropertiesStorage )
            newest = std::max( newest, prop->GetVersion() );
        for ( const auto& prop : m_StorageBufferPropertiesStorage )
            newest = std::max( newest, prop->GetVersion() );
        return newest;
    }

    Common::BoolResultStr MaterialExecutor::PrepareForRecording() const
    {
        if ( !m_MaterialBackend )
            return Common::MakeFormattedError( "material '{}' has no backend", m_DebugName );
        // CPU-RDG-PREPARE (UE FMaterialRenderProxy::CacheUniformExpressions: rewritten only when invalidated).
        // A prepare that would write nothing into the ACTIVE (view, frame in flight) sets is skipped: their
        // revision equals the inputs' (PreparedRevision). Each frame-in-flight copy records its own, so an edit
        // reaches every slot. Never skipped: pending registered values (the upload would write them), and a cube
        // property (its image has no resource generation to compare; the producer restates it every frame).
        const bool skippable = ( m_Owner == nullptr || !m_Owner->HasPendingRegisteredProperties() ) &&
                               m_TextureCubePropertiesStorage.empty();
        if ( skippable && m_MaterialBackend->IsPreparedAt( CurrentRevision() ) )
        {
            m_MaterialBackend->FlushUpdates(); // as the whole prepare: a later rewrite this frame is swallowed
            m_PreparedFrame   = Engine::FrameManager::GetInstance().GetAbsoluteFrameCount();
            m_PreparedVersion = NewestPropertyWrite();
            return Common::MakeSuccess( true );
        }
        const Common::BoolResultStr prepared = m_MaterialBackend->PrepareViewResources( *this );
        if ( !prepared )
            return Common::MakeFormattedError( "material '{}': {}", m_DebugName, prepared.GetError() );
        // RENDER-THREADS M3: the upload and the descriptor writes happen here, once per graph, on the graph's
        // thread - the recording pass only binds the sets (CheckRecordable).
        ApplyForRecording();
        m_PreparedFrame   = Engine::FrameManager::GetInstance().GetAbsoluteFrameCount();
        m_PreparedVersion = NewestPropertyWrite();
        // The revision AFTER the apply (its own writes - the registered upload, UpdateFields - included). A field
        // still pending for this copy (an upload that failed) is owed, so nothing is recorded.
        const bool fieldsPending = std::any_of(
             m_UniformBufferPropertiesStorage.begin(), m_UniformBufferPropertiesStorage.end(),
             []( const auto& prop )
             { return prop->GetFillKind() == ShaderResources::FillKind::Fields && prop->HasDirtyFields(); } );
        if ( skippable && !fieldsPending )
            m_MaterialBackend->NotePreparedAt( CurrentRevision() );
        return prepared;
    }

    PreparedRevision MaterialExecutor::CurrentRevision() const
    {
        PreparedRevision revision;
        revision.Properties = NewestPropertyWrite();
        for ( const auto& prop : m_UniformBufferPropertiesStorage )
            revision.Properties = std::max( revision.Properties, prop->NewestFieldVersion() );
        for ( const auto& prop : m_Texture2DPropertiesStorage )
        {
            if ( const auto* image = prop->GetImage() )
                revision.Images = std::max( revision.Images, image->GetResourceGeneration() );
        }
        revision.Copies = m_MaterialBackend->NewestCopyEpoch( *this );
        return revision;
    }

    Common::BoolResultStr MaterialExecutor::CheckRecordable() const
    {
        if ( m_PreparedFrame != Engine::FrameManager::GetInstance().GetAbsoluteFrameCount() )
            return Common::MakeFormattedError( "material '{}' was not prepared this frame: the pass that draws it "
                                               "declares it in its setup (PassBuilder::Prepares)",
                                               m_DebugName );
        const bool fieldsPending = std::any_of(
             m_UniformBufferPropertiesStorage.begin(), m_UniformBufferPropertiesStorage.end(),
             []( const auto& prop )
             { return prop->GetFillKind() == ShaderResources::FillKind::Fields && prop->HasDirtyFields(); } );
        if ( NewestPropertyWrite() > m_PreparedVersion || fieldsPending ||
             ( m_Owner != nullptr && m_Owner->HasPendingRegisteredProperties() ) )
            return Common::MakeFormattedError( "material '{}' was written after its prepare and a recording pass "
                                               "may not write it: the write belongs in the pass's setup, or "
                                               "travels by value with the draw (DrawPushFields)",
                                               m_DebugName );
        return Common::MakeSuccess( true );
    }

    std::unique_ptr<MaterialExecutor>
    MaterialExecutor::Create( std::string&& debugName, const std::string& shaderName,
                              const Core::Formats::ShaderProgramMeta* parameterSchema )
    {
        const auto& resolvedShader = Runtime::ResourceRegistry::GetShaderService()->GetByName( shaderName );
        if ( !resolvedShader )
        {
            LOG_ERROR( "Could not find the shader: {}", shaderName );
            DESERT_VERIFY( false );
        }

        switch ( RendererAPI::GetAPIType() )
        {
            case RendererAPIType::None:
                return nullptr;
            case RendererAPIType::Vulkan:
            {
                return std::make_unique<MaterialExecutor>(
                     std::move( debugName ), resolvedShader,
                     parameterSchema != nullptr ? *parameterSchema : resolvedShader->GetProgramMeta(),
                     std::make_unique<API::Vulkan::VulkanMaterialBackend>( resolvedShader ),
                     VulkanPushBlockSize( resolvedShader ) );
            }
        }
        DESERT_VERIFY( false, "Unknown RendererAPI" );
        return nullptr;
    }

    std::unique_ptr<MaterialExecutor>
    MaterialExecutor::Create( std::string&& debugName, const std::shared_ptr<Shader>& shader,
                              const Core::Formats::ShaderProgramMeta* parameterSchema )
    {
        switch ( RendererAPI::GetAPIType() )
        {
            case RendererAPIType::None:
                return nullptr;
            case RendererAPIType::Vulkan:
            {
                return std::make_unique<MaterialExecutor>(
                     std::move( debugName ), shader,
                     parameterSchema != nullptr ? *parameterSchema : shader->GetProgramMeta(),
                     std::make_unique<API::Vulkan::VulkanMaterialBackend>( shader ),
                     VulkanPushBlockSize( shader ) );
            }
        }
        DESERT_VERIFY( false, "Unknown RendererAPI" );
        return nullptr;
    }

    std::shared_ptr<UniformBufferProperty>
    MaterialExecutor::GetUniformBufferProperty( const std::string& name ) const
    {
        auto it = m_UniformBufferPropertiesLookup.find( name );
        if ( it != m_UniformBufferPropertiesLookup.end() )
        {
            return m_UniformBufferPropertiesStorage[it->second];
        }
        return nullptr;
    }

    std::shared_ptr<Desert::Graphic::StorageBufferProperty>
    MaterialExecutor::GetStorageBufferProperty( const std::string& name ) const
    {
        auto it = m_StorageBufferPropertiesLookup.find( name );
        if ( it != m_StorageBufferPropertiesLookup.end() )
        {
            return m_StorageBufferPropertiesStorage[it->second];
        }
        return nullptr;
    }

    std::shared_ptr<Texture2DProperty> MaterialExecutor::GetTexture2DProperty( const std::string& name ) const
    {
        auto it = m_Texture2DPropertiesLookup.find( name );
        if ( it != m_Texture2DPropertiesLookup.end() )
        {
            return m_Texture2DPropertiesStorage[it->second];
        }
        return nullptr;
    }

    std::shared_ptr<TextureCubeProperty> MaterialExecutor::GetTextureCubeProperty( const std::string& name ) const
    {
        auto it = m_TextureCubePropertiesLookup.find( name );
        if ( it != m_TextureCubePropertiesLookup.end() )
        {
            return m_TextureCubePropertiesStorage[it->second];
        }
        return nullptr;
    }

    namespace
    {
        // One body for the four raw finders: the lookup hashes the view in place (PropertyNameHash) and the
        // storage keeps the property alive, so the caller holds a plain pointer.
        template <typename T, typename Lookup, typename Storage>
        T* FindInLookup( const Lookup& lookup, const Storage& storage, const std::string_view name )
        {
            const auto it = lookup.find( name );
            return it != lookup.end() ? storage[it->second].get() : nullptr;
        }
    } // namespace

    UniformBufferProperty* MaterialExecutor::FindUniformBufferProperty( const std::string_view name ) const
    {
        return FindInLookup<UniformBufferProperty>( m_UniformBufferPropertiesLookup,
                                                    m_UniformBufferPropertiesStorage, name );
    }

    StorageBufferProperty* MaterialExecutor::FindStorageBufferProperty( const std::string_view name ) const
    {
        return FindInLookup<StorageBufferProperty>( m_StorageBufferPropertiesLookup,
                                                    m_StorageBufferPropertiesStorage, name );
    }

    Texture2DProperty* MaterialExecutor::FindTexture2DProperty( const std::string_view name ) const
    {
        return FindInLookup<Texture2DProperty>( m_Texture2DPropertiesLookup, m_Texture2DPropertiesStorage, name );
    }

    TextureCubeProperty* MaterialExecutor::FindTextureCubeProperty( const std::string_view name ) const
    {
        return FindInLookup<TextureCubeProperty>( m_TextureCubePropertiesLookup, m_TextureCubePropertiesStorage,
                                                  name );
    }
} // namespace Desert::Graphic