#include <Engine/Graphic/Rhi/RhiKind.hpp>

namespace Desert::Graphic
{
    std::string_view ToString( const RhiKind kind ) noexcept
    {
        switch ( kind )
        {
            case RhiKind::Vulkan:
                return "vulkan";
            case RhiKind::D3D12:
                return "d3d12";
        }
        return "unknown";
    }

    std::optional<RhiKind> ParseRhiKind( const std::string_view text ) noexcept
    {
        if ( text == "vulkan" )
            return RhiKind::Vulkan;
        if ( text == "d3d12" )
            return RhiKind::D3D12;
        return std::nullopt;
    }
} // namespace Desert::Graphic
