#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace Desert::Graphic
{
    // The rendering backend a process runs on. Chosen once, before the window's surface, by RhiSelection; the
    // only switch on it is CreateRhi (Rhi.cpp) and the editor's choice of its UI backend.
    enum class RhiKind : uint8_t
    {
        Vulkan = 1,
        D3D12  = 2
    };

    [[nodiscard]] std::string_view       ToString( RhiKind kind ) noexcept;
    [[nodiscard]] std::optional<RhiKind> ParseRhiKind( std::string_view text ) noexcept; // "vulkan" | "d3d12"
} // namespace Desert::Graphic
