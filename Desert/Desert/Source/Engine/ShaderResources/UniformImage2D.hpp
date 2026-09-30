#pragma once

#include <Common/Core/ResultStr.hpp>

#include <Engine/Graphic/Shader.hpp>
#include <Engine/Graphic/Image.hpp>
#include <Engine/Graphic/RDG/RDGAccess.hpp>

#include <optional>

namespace Desert::ShaderResources
{
    class UniformImage2D
    {
    public:
        virtual ~UniformImage2D() = default;

        virtual uint32_t GetBinding() const = 0;

        // @p declared, when given, is the access the graph node that draws with this binding declared on the
        // image: the descriptor then names the layout that access leaves the image in (as
        // ComputePipeline::SetInput does), not the layout the image recorded before the graph ran. Without it, the
        // recorded layout.
        virtual void SetImage2D( const Graphic::Image2D*             image2D,
                                 std::optional<Graphic::RDG::Access> declared ) = 0;

        // GetImageHash() was here with no caller: the descriptor caches key off Image::GetHash() on the
        // image itself, so asking the uniform for its image's hash was a second way to the same answer,
        // and the one that had to dereference a pointer this class does not own. Г12.

    private:
        static std::shared_ptr<UniformImage2D> Create( const std::string_view debugName, uint32_t binding );

        friend class ShaderResourcesManager;
    };
} // namespace Desert::ShaderResources