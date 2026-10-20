#pragma once

#include <Engine/Graphic/ShaderProtocols/Camera.hpp>

#include <string>
#include <string_view>

namespace Desert::Graphic::ShaderProtocols
{
    /// THE INCLUDES WHOSE TEXT IS GENERATED FROM A C++ PROTOCOL (Std140Block.hpp) rather than read from disk.
    /// @p requested is an angle include's shader-root-relative path, normalised with '/' separators. Asked by the
    /// shader includer (to serve the text) and by the shader cache key (to hash it and walk it) — the two must
    /// agree, so both ask here. nullptr: not a generated include.
    [[nodiscard]] inline const std::string* FindGeneratedProtocolInclude( const std::string_view requested )
    {
        if ( requested == kCameraUBInclude )
            return &CameraUBGlsl();
        return nullptr;
    }
} // namespace Desert::Graphic::ShaderProtocols
