#pragma once

#include <Common/Core/ResultStr.hpp>

#include <Engine/Graphic/API/Vulkan/VulkanShaderResource.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
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
        // The descriptor of an arrayed binding (a separate texture2D u_X[N]: slot "u_X[e]" is element e); 0 else.
        uint32_t Element = 0;

        friend bool operator==( const RdgSlotKey&, const RdgSlotKey& ) = default;
    };

    // One PassBindings entry placed at its reflected slot. Points into the PassBindings it was resolved from,
    // so it lives no longer than that block (the exec).
    struct RdgResolvedEntry
    {
        RdgSlotKey               Slot;
        RDG::ShaderResourceKind  Kind    = RDG::ShaderResourceKind::Texture;
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

    // A resource slot of the shader as reflection records it: its bucket decides the kind an entry must have.
    struct RdgReflectedSlot
    {
        RdgSlotKey              Slot;
        std::string             Name; // "<name>[<element>]" for an element of an arrayed binding
        std::string_view        Bucket;
        RDG::ShaderResourceKind Kind = RDG::ShaderResourceKind::Texture;
    };

    // RDG-PARALLEL-6 (UE FShaderParameterMapInfo, bound once per shader). Every resource slot of ONE reflection,
    // built once when the shader is reflected (VulkanShader keeps it beside its ReflectionData and rebuilds it
    // with it on a reload), so a dispatch, a trace and a pass declaration read a table and never walk the
    // reflection maps, copy a name or sort again. Immutable after MakeRdgSlotTable: any thread reads it.
    struct RdgSlotTable
    {
        std::vector<RdgReflectedSlot> Slots; // by (set, binding, element): the order every report uses
        // FPS-ARCH-BINDINGS: (RDG::HashShaderName( Name ), index into Slots) by hash - an entry's slot is found by
        // its declared name hash (integer compares), the name only confirming the hit.
        std::vector<std::pair<uint64_t, uint32_t>> ByHash;

        // The index in Slots of the slot whose name hashes to @p hash and is @p name, or Slots.size() for none.
        [[nodiscard]] size_t Find( uint64_t hash, std::string_view name ) const noexcept;
        [[nodiscard]] size_t Find( std::string_view name ) const noexcept;
    };

    [[nodiscard]] std::shared_ptr<const RdgSlotTable> MakeRdgSlotTable( const ShaderResource::ReflectionData& reflection );

    // Resolves every entry of @p bindings against @p reflection of shader @p shaderName and checks the draw is
    // complete. Refused, naming the pass and the slot, when: the block has a failed entry; a name is not a
    // resource of the shader; an entry's kind does not match the reflected descriptor bucket; a slot is filled
    // by the block AND @p other; a resource slot of the shader is filled by neither; the push constants are
    // given by both, by neither while the shader declares a range, or with a size other than the declared one.
    // RDG-FAULT1. What a pass's binding block is validated against (RDG::ValidatePassBindings): every slot the
    // reflected shader declares, acceleration structures included (BindingBlockBuilder::AccelerationStructure),
    // and its push-constant range.
    RDG::ShaderBindingLayout MakeShaderBindingLayout( const ShaderResource::ReflectionData& reflection,
                                                      std::string_view                      shaderName );
    // The same over the shader's slot table (@p slots built from @p reflection): what the engine calls; the
    // reflection-only forms above and below build the table for one call (pure-data callers, tests).
    RDG::ShaderBindingLayout MakeShaderBindingLayout( const ShaderResource::ReflectionData& reflection,
                                                      const RdgSlotTable& slots, std::string_view shaderName );

    // RDG-FAULT1. @p other (slot keys) as the shader names of those slots, for a setup-time block's
    // OtherRouteFill. A key the reflection does not declare is left out: the record-time resolve still checks the
    // real route.
    RDG::OtherRouteFill MakeOtherRouteFill( const ShaderResource::ReflectionData& reflection,
                                            const RdgOtherRoute&                  other );
    RDG::OtherRouteFill MakeOtherRouteFill( const RdgSlotTable& slots, const RdgOtherRoute& other );

    Common::ResultStr<std::vector<RdgResolvedEntry>>
    ResolveRdgPassBindings( const ShaderResource::ReflectionData& reflection, std::string_view shaderName,
                            const RDG::PassBindings& bindings, const RdgOtherRoute& other );
    // @p slots is the table of @p reflection (VulkanShader::GetRdgSlots); the entries go to @p resolved (cleared
    // first; the caller's per-thread scratch, so a steady frame allocates nothing).
    // FPS-ARCH-BINDINGS (UE: the parameter struct is bound to the shader once, the draw writes by offset): when
    // the block's layout was derived from @p slots (ShaderBindingLayout::Backend), an entry's slot is the index
    // it was declared with - no lookup at all; otherwise (a layout a renderer wrote by hand, a program reloaded
    // since the pass was declared) by its declared name hash. The completeness and push checks walk indices.
    Common::BoolResultStr ResolveRdgPassBindings( const ShaderResource::ReflectionData& reflection,
                                                  const RdgSlotTable& slots, std::string_view shaderName,
                                                  const RDG::PassBindings& bindings, const RdgOtherRoute& other,
                                                  std::vector<RdgResolvedEntry>& resolved );
} // namespace Desert::Graphic::API::Vulkan
