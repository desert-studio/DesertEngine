#include <Engine/Graphic/API/Vulkan/VulkanRdgPassBindings.hpp>

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <format>
#include <optional>
#include <string>
#include <tuple>

namespace Desert::Graphic::API::Vulkan
{
    namespace
    {
        // A resource slot of the shader as reflection records it: its bucket decides the kind an entry must have.
        struct ReflectedSlot
        {
            RdgSlotKey       Slot;
            std::string      Name; // "<name>[<element>]" for an element of an arrayed binding
            std::string_view Bucket;
            // Empty for a slot the block cannot fill (an acceleration structure).
            std::optional<RDG::ShaderResourceKind> Kind;
        };

        template <class Map>
        void Collect( std::vector<ReflectedSlot>& out, uint32_t set, const Map& map, std::string_view bucket,
                      std::optional<RDG::ShaderResourceKind> kind )
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
                Collect( slots, set, descriptors.AccelerationStructures, "acceleration structure", std::nullopt );
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
            }
            return "?";
        }
    } // namespace

    RDG::ShaderBindingLayout MakeShaderBindingLayout( const ShaderResource::ReflectionData& reflection,
                                                      std::string_view                      shaderName )
    {
        RDG::ShaderBindingLayout layout;
        layout.ShaderName = std::string( shaderName );
        for ( const ReflectedSlot& slot : CollectSlots( reflection ) )
        {
            if ( slot.Kind )
                layout.Slots.push_back( { slot.Name, *slot.Kind } );
        }
        layout.PushConstantBytes = reflection.PushConstantRanges ? reflection.PushConstantRanges->Size : 0;
        return layout;
    }

    RDG::OtherRouteFill MakeOtherRouteFill( const ShaderResource::ReflectionData& reflection,
                                            const RdgOtherRoute&                  other )
    {
        RDG::OtherRouteFill fill;
        fill.PushConstants = other.PushConstants;
        for ( const ReflectedSlot& slot : CollectSlots( reflection ) )
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
        using Result                = std::vector<RdgResolvedEntry>;
        const std::string_view pass = bindings.GetContext().GetPassName();

        const Common::BoolResultStr status = bindings.GetStatus();
        if ( !status.IsSuccess() )
            return Common::MakeError<Result>( status.GetError() );

        const std::vector<ReflectedSlot> slots   = CollectSlots( reflection );
        const auto                       isOther = [&]( const RdgSlotKey& key )
        { return std::find( other.Filled.begin(), other.Filled.end(), key ) != other.Filled.end(); };

        Result resolved;
        resolved.reserve( bindings.GetTextures().size() + bindings.GetBuffers().size() );
        const auto place = [&]( std::string_view name, RDG::ShaderResourceKind kind,
                                const RDG::BoundTexture* texture,
                                const RDG::BoundBuffer*  buffer ) -> Common::BoolResultStr
        {
            const auto slot = std::find_if( slots.begin(), slots.end(),
                                            [&]( const ReflectedSlot& s ) { return s.Name == name; } );
            if ( slot == slots.end() )
                return Common::MakeFormattedError( "{}: '{}' is not a resource of shader '{}'", pass, name,
                                                   shaderName );
            if ( slot->Kind != kind )
                return Common::MakeFormattedError(
                     "{}: '{}' is bound as a {} but shader '{}' declares it a {} (set {} binding {})", pass, name,
                     KindName( kind ), shaderName, slot->Bucket, slot->Slot.Set, slot->Slot.Binding );
            if ( isOther( slot->Slot ) )
                return Common::MakeFormattedError(
                     "{}: '{}' of shader '{}' is filled both by the pass bindings and by the material / pipeline "
                     "setter; a graph resource is bound only through the pass bindings",
                     pass, name, shaderName );
            resolved.push_back( RdgResolvedEntry{ slot->Slot, kind, texture, buffer } );
            return Common::MakeSuccess( true );
        };

        for ( const RDG::BoundTexture& texture : bindings.GetTextures() )
        {
            const Common::BoolResultStr placed = place( texture.ShaderName, texture.Kind, &texture, nullptr );
            if ( !placed.IsSuccess() )
                return Common::MakeError<Result>( placed.GetError() );
        }
        for ( const RDG::BoundBuffer& buffer : bindings.GetBuffers() )
        {
            const Common::BoolResultStr placed = place( buffer.ShaderName, buffer.Kind, nullptr, &buffer );
            if ( !placed.IsSuccess() )
                return Common::MakeError<Result>( placed.GetError() );
        }

        // Every resource slot of the shader is filled by exactly one route: nothing is left to a fallback image.
        for ( const ReflectedSlot& slot : slots )
        {
            const bool byBlock = std::any_of( resolved.begin(), resolved.end(),
                                              [&]( const RdgResolvedEntry& e ) { return e.Slot == slot.Slot; } );
            if ( !byBlock && !isOther( slot.Slot ) )
                return Common::MakeFormattedError<Result>( "{}: {} '{}' (set {} binding {}) of shader '{}' is "
                                                           "filled neither by the pass bindings nor by "
                                                           "the material",
                                                           pass, slot.Bucket, slot.Name, slot.Slot.Set,
                                                           slot.Slot.Binding, shaderName );
        }

        const size_t pushSize = bindings.GetPushConstants().size();
        if ( pushSize != 0 && other.PushConstants )
            return Common::MakeFormattedError<Result>(
                 "{}: the push constants of shader '{}' are given both by the pass bindings and by the material",
                 pass, shaderName );
        if ( reflection.PushConstantRanges )
        {
            const uint32_t declared = reflection.PushConstantRanges->Size;
            if ( pushSize == 0 && !other.PushConstants )
                return Common::MakeFormattedError<Result>(
                     "{}: shader '{}' declares a {}-byte push-constant block '{}' that nothing fills", pass,
                     shaderName, declared, reflection.PushConstantRanges->Name );
            if ( pushSize != 0 && pushSize != declared )
                return Common::MakeFormattedError<Result>(
                     "{}: {} bytes of push constants for shader '{}', which declares {} ('{}')", pass, pushSize,
                     shaderName, declared, reflection.PushConstantRanges->Name );
        }
        else if ( pushSize != 0 )
            return Common::MakeFormattedError<Result>(
                 "{}: {} bytes of push constants for shader '{}', which declares no push-constant block", pass,
                 pushSize, shaderName );

        return Common::MakeSuccess( std::move( resolved ) );
    }
} // namespace Desert::Graphic::API::Vulkan
