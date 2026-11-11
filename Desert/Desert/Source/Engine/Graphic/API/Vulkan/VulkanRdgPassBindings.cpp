#include <Engine/Graphic/API/Vulkan/VulkanRdgPassBindings.hpp>

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <format>
#include <string>
#include <tuple>

namespace Desert::Graphic::API::Vulkan
{
    namespace
    {
        using ReflectedSlot = RdgReflectedSlot;

        template <class Map>
        void Collect( std::vector<ReflectedSlot>& out, uint32_t set, const Map& map, std::string_view bucket,
                      RDG::ShaderResourceKind kind )
        {
            for ( const auto& [binding, resource] : map )
            {
                // Each element of an arrayed binding is its own slot, named by its index, so the block fills,
                // validates and reports it like any other slot.
                uint32_t elements = 1;
                if constexpr ( requires( const typename Map::mapped_type& r ) { r.ArraySize; } )
                    elements = resource.ArraySize;
                if ( elements <= 1 )
                {
                    out.push_back( ReflectedSlot{ { set, binding, 0 }, resource.Name, bucket, kind } );
                    continue;
                }
                for ( uint32_t element = 0; element < elements; ++element )
                    out.push_back( ReflectedSlot{ { set, binding, element },
                                                  std::format( "{}[{}]", resource.Name, element ),
                                                  bucket,
                                                  kind } );
            }
        }

        std::vector<ReflectedSlot> CollectSlots( const ShaderResource::ReflectionData& reflection )
        {
            using Kind = RDG::ShaderResourceKind;
            std::vector<ReflectedSlot> slots;
            for ( const auto& [set, descriptors] : reflection.ShaderDescriptorSets )
            {
                Collect( slots, set, descriptors.UniformBuffers, "uniform buffer", Kind::UniformBuffer );
                Collect( slots, set, descriptors.StorageBuffers, "storage buffer", Kind::StorageBuffer );
                Collect( slots, set, descriptors.StorageImage2DSamplers, "storage image", Kind::StorageTexture );
                Collect( slots, set, descriptors.StorageImage3DSamplers, "storage 3D image",
                         Kind::StorageTexture );
                Collect( slots, set, descriptors.AccelerationStructures, "acceleration structure",
                         Kind::AccelerationStructure );
                Collect( slots, set, descriptors.SampledImages, "separate texture", Kind::Texture );
                Collect( slots, set, descriptors.Samplers, "sampler", Kind::Sampler );
            }
            // Deterministic order, so the reported slot does not depend on hash-map iteration.
            std::sort( slots.begin(), slots.end(),
                       []( const ReflectedSlot& a, const ReflectedSlot& b )
                       {
                           return std::tie( a.Slot.Set, a.Slot.Binding, a.Slot.Element ) <
                                  std::tie( b.Slot.Set, b.Slot.Binding, b.Slot.Element );
                       } );
            return slots;
        }

        std::string_view KindName( RDG::ShaderResourceKind kind )
        {
            switch ( kind )
            {
                case RDG::ShaderResourceKind::StorageTexture:
                    return "Storage texture";
                case RDG::ShaderResourceKind::Texture:
                    return "Separate texture";
                case RDG::ShaderResourceKind::Sampler:
                    return "Sampler";
                case RDG::ShaderResourceKind::UniformBuffer:
                    return "Uniform buffer";
                case RDG::ShaderResourceKind::StorageBuffer:
                    return "Storage buffer";
                case RDG::ShaderResourceKind::AccelerationStructure:
                    return "Acceleration structure";
            }
            return "?";
        }
    } // namespace

    size_t RdgSlotTable::Find( const uint64_t hash, const std::string_view name ) const noexcept
    {
        auto found = std::lower_bound( ByHash.begin(), ByHash.end(), hash,
                                       []( const std::pair<uint64_t, uint32_t>& row, const uint64_t key )
                                       { return row.first < key; } );
        for ( ; found != ByHash.end() && found->first == hash; ++found )
        {
            if ( Slots[found->second].Name == name )
                return found->second;
        }
        return Slots.size();
    }

    size_t RdgSlotTable::Find( const std::string_view name ) const noexcept
    {
        return Find( RDG::HashShaderName( name ), name );
    }

    std::shared_ptr<const RdgSlotTable> MakeRdgSlotTable( const ShaderResource::ReflectionData& reflection )
    {
        auto table   = std::make_shared<RdgSlotTable>();
        table->Slots = CollectSlots( reflection );
        table->ByHash.reserve( table->Slots.size() );
        for ( uint32_t i = 0; i < static_cast<uint32_t>( table->Slots.size() ); ++i )
            table->ByHash.emplace_back( RDG::HashShaderName( table->Slots[i].Name ), i );
        std::stable_sort( table->ByHash.begin(), table->ByHash.end(),
                          []( const std::pair<uint64_t, uint32_t>& a, const std::pair<uint64_t, uint32_t>& b )
                          { return a.first < b.first; } );
        return table;
    }

    RDG::ShaderBindingLayout MakeShaderBindingLayout( const ShaderResource::ReflectionData& reflection,
                                                      std::string_view                      shaderName )
    {
        return MakeShaderBindingLayout( reflection, *MakeRdgSlotTable( reflection ), shaderName );
    }

    RDG::ShaderBindingLayout MakeShaderBindingLayout( const ShaderResource::ReflectionData& reflection,
                                                      const RdgSlotTable& slots, std::string_view shaderName )
    {
        RDG::ShaderBindingLayout layout;
        layout.ShaderName = std::string( shaderName );
        layout.Slots.reserve( slots.Slots.size() );
        for ( const ReflectedSlot& slot : slots.Slots )
        {
            layout.Slots.push_back( { slot.Name, slot.Kind } );
        }
        layout.PushConstantBytes = reflection.PushConstantRanges ? reflection.PushConstantRanges->Size : 0;
        // In the table's order, so a slot's layout index is its table index (ResolveRdgPassBindings' fast path).
        RDG::IndexShaderBindingLayout( layout );
        return layout;
    }

    RDG::OtherRouteFill MakeOtherRouteFill( const ShaderResource::ReflectionData& reflection,
                                            const RdgOtherRoute&                  other )
    {
        return MakeOtherRouteFill( *MakeRdgSlotTable( reflection ), other );
    }

    RDG::OtherRouteFill MakeOtherRouteFill( const RdgSlotTable& slots, const RdgOtherRoute& other )
    {
        RDG::OtherRouteFill fill;
        fill.PushConstants = other.PushConstants;
        if ( other.Filled.empty() )
            return fill; // nothing filled by the other route: no slot to name
        for ( const ReflectedSlot& slot : slots.Slots )
        {
            if ( std::find( other.Filled.begin(), other.Filled.end(), slot.Slot ) != other.Filled.end() )
                fill.Slots.emplace_back( slot.Name );
        }
        return fill;
    }

    Common::ResultStr<std::vector<RdgResolvedEntry>>
    ResolveRdgPassBindings( const ShaderResource::ReflectionData& reflection, std::string_view shaderName,
                            const RDG::PassBindings& bindings, const RdgOtherRoute& other )
    {
        std::vector<RdgResolvedEntry> resolved;
        const Common::BoolResultStr   status =
             ResolveRdgPassBindings( reflection, *MakeRdgSlotTable( reflection ), shaderName, bindings, other,
                                     resolved );
        if ( !status.IsSuccess() )
            return Common::MakeError<std::vector<RdgResolvedEntry>>( status.GetError() );
        return Common::MakeSuccess( std::move( resolved ) );
    }

    Common::BoolResultStr ResolveRdgPassBindings( const ShaderResource::ReflectionData& reflection,
                                                  const RdgSlotTable& table, std::string_view shaderName,
                                                  const RDG::PassBindings& bindings, const RdgOtherRoute& other,
                                                  std::vector<RdgResolvedEntry>& resolved )
    {
        resolved.clear();
        const std::string_view pass = bindings.GetContext().GetPassName();

        const Common::BoolResultStr status = bindings.GetStatus();
        if ( !status.IsSuccess() )
            return Common::MakeError( status.GetError() );

        const std::vector<ReflectedSlot>& slots = table.Slots;
        // FPS-ARCH-BINDINGS: the block's layout derived from THIS table (VulkanRendererAPI::GetBindingLayout) has
        // the table's slot order, so the index each entry was declared with is its slot here.
        const RDG::ShaderBindingLayout* layout = bindings.GetLayout();
        const bool sameTable = layout != nullptr && layout->Backend.get() == static_cast<const void*>( &table );
        // Which slots the block filled, by slot index: the completeness check below is one pass over the slots.
        // Per-thread scratch: a steady frame allocates nothing.
        thread_local std::vector<uint8_t> t_ByBlock;
        std::vector<uint8_t>&             byBlock = t_ByBlock;
        byBlock.assign( slots.size(), 0 );
        const auto isOther = [&]( const RdgSlotKey& key )
        { return std::find( other.Filled.begin(), other.Filled.end(), key ) != other.Filled.end(); };

        resolved.reserve( bindings.GetTextures().size() + bindings.GetBuffers().size() );
        const auto place = [&]( std::string_view name, uint64_t nameHash, uint32_t declaredSlot,
                                RDG::ShaderResourceKind kind, const RDG::BoundTexture* texture,
                                const RDG::BoundBuffer* buffer ) -> Common::BoolResultStr
        {
            const size_t index = sameTable ? declaredSlot : table.Find( nameHash, name );
            if ( index >= slots.size() )
                return Common::MakeFormattedError( "{}: '{}' is not a resource of shader '{}'", pass, name,
                                                   shaderName );
            const ReflectedSlot* const slot = &slots[index];
            if ( slot->Kind != kind )
                return Common::MakeFormattedError(
                     "{}: '{}' is bound as a {} but shader '{}' declares it a {} (set {} binding {})", pass, name,
                     KindName( kind ), shaderName, slot->Bucket, slot->Slot.Set, slot->Slot.Binding );
            if ( !other.Filled.empty() && isOther( slot->Slot ) )
                return Common::MakeFormattedError(
                     "{}: '{}' of shader '{}' is filled both by the pass bindings and by the material / pipeline "
                     "setter; a graph resource is bound only through the pass bindings",
                     pass, name, shaderName );
            byBlock[index] = 1;
            resolved.push_back( RdgResolvedEntry{ slot->Slot, kind, texture, buffer } );
            return Common::MakeSuccess( true );
        };

        for ( const RDG::BoundTexture& texture : bindings.GetTextures() )
        {
            const Common::BoolResultStr placed =
                 place( texture.ShaderName, texture.NameHash, texture.Slot, texture.Kind, &texture, nullptr );
            if ( !placed.IsSuccess() )
                return placed;
        }
        for ( const RDG::BoundBuffer& buffer : bindings.GetBuffers() )
        {
            const Common::BoolResultStr placed =
                 place( buffer.ShaderName, buffer.NameHash, buffer.Slot, buffer.Kind, nullptr, &buffer );
            if ( !placed.IsSuccess() )
                return placed;
        }

        // Every resource slot of the shader is filled by exactly one route: nothing is left to a fallback image.
        for ( size_t index = 0; index < slots.size(); ++index )
        {
            const ReflectedSlot& slot = slots[index];
            if ( byBlock[index] == 0 && !isOther( slot.Slot ) )
                return Common::MakeFormattedError( "{}: {} '{}' (set {} binding {}) of shader '{}' is filled "
                                                   "neither by the pass bindings nor by the material",
                                                   pass, slot.Bucket, slot.Name, slot.Slot.Set,
                                                   slot.Slot.Binding, shaderName );
        }

        const size_t pushSize = bindings.GetPushConstants().size();
        if ( pushSize != 0 && other.PushConstants )
            return Common::MakeFormattedError(
                 "{}: the push constants of shader '{}' are given both by the pass bindings and by the material",
                 pass, shaderName );
        if ( reflection.PushConstantRanges )
        {
            const uint32_t declared = reflection.PushConstantRanges->Size;
            if ( pushSize == 0 && !other.PushConstants )
                return Common::MakeFormattedError(
                     "{}: shader '{}' declares a {}-byte push-constant block '{}' that nothing fills", pass,
                     shaderName, declared, reflection.PushConstantRanges->Name );
            if ( pushSize != 0 && pushSize != declared )
                return Common::MakeFormattedError(
                     "{}: {} bytes of push constants for shader '{}', which declares {} ('{}')", pass, pushSize,
                     shaderName, declared, reflection.PushConstantRanges->Name );
        }
        else if ( pushSize != 0 )
            return Common::MakeFormattedError(
                 "{}: {} bytes of push constants for shader '{}', which declares no push-constant block", pass,
                 pushSize, shaderName );

        return Common::MakeSuccess( true );
    }
} // namespace Desert::Graphic::API::Vulkan
