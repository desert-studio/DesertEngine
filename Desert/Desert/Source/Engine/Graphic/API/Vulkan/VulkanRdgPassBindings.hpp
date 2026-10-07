#pragma once

#include <Common/Core/ResultStr.hpp>

#include <Engine/Graphic/API/Vulkan/VulkanShaderResource.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

// RDG-A2 - the Vulkan consumer side of RDG::PassBindings: shader name -> (set, binding) through the pipeline's
// reflection, and the checks Renderer::DispatchCompute / DrawFullscreen refuse a draw on. The resolution is pure
// over reflection data (no device), so every refusal is testable off-GPU (Desert/Tests/Engine/RenderGraphVulkan).
namespace Desert::Graphic::API::Vulkan
{
    struct RdgSlotKey
    {
        uint32_t Set     = 0;
        uint32_t Binding = 0;

        friend bool operator==( const RdgSlotKey&, const RdgSlotKey& ) = default;
    };

    // One PassBindings entry placed at its reflected slot. Points into the PassBindings it was resolved from,
    // so it lives no longer than that block (the exec).
    struct RdgResolvedEntry
    {
        RdgSlotKey               Slot;
        RDG::ShaderResourceKind  Kind    = RDG::ShaderResourceKind::SampledTexture;
        const RDG::BoundTexture* Texture = nullptr; // set for the texture kinds
        const RDG::BoundBuffer*  Buffer  = nullptr; // set for the buffer kinds
    };

    // What the other route (the material, or a compute pipeline's own setters for asset textures) fills for
    // this draw: the slots it has written with a real resource - never a slot holding only its fallback - and
    // whether it supplies the push constants.
    struct RdgOtherRoute
    {
        std::vector<RdgSlotKey> Filled;
        bool                    PushConstants = false;
    };

    // Resolves every entry of @p bindings against @p reflection of shader @p shaderName and checks the draw is
    // complete. Refused, naming the pass and the slot, when: the block has a failed entry; a name is not a
    // resource of the shader; an entry's kind does not match the reflected descriptor bucket; a slot is filled
    // by the block AND @p other; a resource slot of the shader is filled by neither; the push constants are
    // given by both, by neither while the shader declares a range, or with a size other than the declared one.
    // RDG-FAULT1. What a pass's binding block is validated against (RDG::ValidatePassBindings): every slot the
    // reflected shader declares that a block can fill, and its push-constant range. An acceleration structure
    // has no ShaderResourceKind (a block cannot fill one), so it is not a slot of the layout.
    RDG::ShaderBindingLayout MakeShaderBindingLayout( const ShaderResource::ReflectionData& reflection,
                                                      std::string_view                      shaderName );

    // RDG-FAULT1. @p other (slot keys) as the shader names of those slots, for a setup-time block's
    // OtherRouteFill. A key the reflection does not declare is left out: the record-time resolve still checks the
    // real route.
    RDG::OtherRouteFill MakeOtherRouteFill( const ShaderResource::ReflectionData& reflection,
                                            const RdgOtherRoute&                  other );

    Common::ResultStr<std::vector<RdgResolvedEntry>>
    ResolveRdgPassBindings( const ShaderResource::ReflectionData& reflection, std::string_view shaderName,
                            const RDG::PassBindings& bindings, const RdgOtherRoute& other );
} // namespace Desert::Graphic::API::Vulkan
