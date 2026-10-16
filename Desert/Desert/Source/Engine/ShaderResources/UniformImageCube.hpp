#pragma once

#include <Common/Core/ResultStr.hpp>

#include <Engine/Graphic/Shader.hpp>
#include <Engine/Graphic/Image.hpp>

#include <optional>

namespace Desert::ShaderResources
{
    class UniformImageCube
    {
    public:
        virtual ~UniformImageCube() = default;

        virtual uint32_t GetBinding() const = 0;

        // The binding of this texture's own sampler state (`<name>Sampler`), which the material writes from the
        // property's state; empty when the shader declares none (the texture is then a pass's input).
        [[nodiscard]] std::optional<uint32_t> GetSamplerBinding() const
        {
            return m_SamplerBinding;
        }

        virtual void SetImageCube( const Graphic::ImageCube* imageCube ) = 0;

        // GetImageHash() was here with no caller — same as UniformImage2D. Г12.

    private:
        static std::shared_ptr<UniformImageCube> Create( const std::string_view debugName, uint32_t binding );

        std::optional<uint32_t> m_SamplerBinding;

        friend class ShaderResourcesManager;
    };
} // namespace Desert::ShaderResources