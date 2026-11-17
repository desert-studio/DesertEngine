#pragma once

#include <cstdint>
#include <type_traits>

// SHADER-PUSH-GEN-2 (UE: SHADER_PARAMETER_STRUCT - a pass sets its parameters through the one struct generated
// from the shader, never raw bytes). The push-constant block of every shader is generated as a
// ShaderPush::<Shader> struct (Engine/Generated/ShaderPushConstants.gen.hpp, DesertHeaderTool --shader-push); the
// generator marks each one IsBlock, and the RDG push API (RDG::BindingBlockBuilder::PushConstants<T>,
// RDG::PassBindings::PushConstants, RenderPassDeclaration::BlockDeclaration::PushConstants<T>) accepts a
// ShaderPush::Block alone.
namespace Desert::Graphic::ShaderPush
{
    // True for the generated push structs only (each specialised next to its struct by the generator).
    template <class T>
    inline constexpr bool IsBlock = false;

    template <class T>
    concept Block = IsBlock<T> && std::is_trivially_copyable_v<T> && std::is_standard_layout_v<T>;

    // The size of a generated push block, as a declaration carries it to the graph: built from the type alone
    // (Of<T>), so a pass declares the block its shader has, never a byte count. Default = no push block.
    class BlockSize
    {
    public:
        constexpr BlockSize() noexcept = default;

        template <Block T>
        [[nodiscard]] static constexpr BlockSize Of() noexcept
        {
            return BlockSize( static_cast<uint32_t>( sizeof( T ) ) );
        }

        [[nodiscard]] constexpr uint32_t Bytes() const noexcept
        {
            return m_Bytes;
        }

    private:
        explicit constexpr BlockSize( uint32_t bytes ) noexcept : m_Bytes( bytes )
        {
        }

        uint32_t m_Bytes = 0;
    };
} // namespace Desert::Graphic::ShaderPush
