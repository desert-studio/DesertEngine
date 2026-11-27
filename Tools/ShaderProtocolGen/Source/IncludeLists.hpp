#pragma once
// THE generated shader includes: one list, split in seven parts so each part's file is owned by one subsystem
// group. Main joins the parts; the engine never repeats the list (ShaderCompiler gets only the finished text).
#include <Engine/Graphic/ShaderProtocols/ShaderProtocol.hpp>

#include <span>
#include <string_view>

namespace Desert::ShaderProtocolGen
{
    using Graphic::ShaderProtocols::ShaderIncludeDecl;
    std::span<const ShaderIncludeDecl* const> SceneIncludes();          // IncludesScene.cpp
    std::span<const ShaderIncludeDecl* const> PostIncludes();           // IncludesPost.cpp
    std::span<const ShaderIncludeDecl* const> GeometryShadowIncludes(); // IncludesGeometryShadow.cpp
    std::span<const ShaderIncludeDecl* const> LightingIncludes();       // IncludesLighting.cpp
    std::span<const ShaderIncludeDecl* const> VfxIncludes();            // IncludesVfx.cpp
    std::span<const ShaderIncludeDecl* const> AtmosphereIncludes();     // IncludesAtmosphere.cpp
    std::span<const ShaderIncludeDecl* const> WaterIncludes();          // IncludesWater.cpp

    // The vertex attributes the generated vertex-input includes declare
    // (Graphic/ShaderProtocols/VertexInputs.hpp): written into the .gen.cpp as Core::IsGeneratedVertexInputName,
    // the includer's refusal of a hand-written twin.
    std::span<const std::string_view> GeneratedVertexInputNames(); // IncludesScene.cpp
} // namespace Desert::ShaderProtocolGen
