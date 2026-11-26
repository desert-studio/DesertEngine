#pragma once
// What a generated shader include contains. A layout header declares one ShaderIncludeDecl per include;
// Tools/ShaderProtocolGen is the only reader. Generated paths live under "Generated/<Domain>/", except the twelve
// existing scene paths (Common/CameraUB.glslh, Mesh/*.glslh, Generated/SceneBindings.glslh …), kept for their
// includers.
#include <Engine/Graphic/ShaderProtocols/ShaderLayout.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace Desert::Graphic::ShaderProtocols
{
    enum class ShaderBlockKind : uint8_t
    {
        Uniform,
        ReadBuffer,
        WriteBuffer,
        Buffer
    };
    enum class ShaderBindingKind : uint8_t
    {
        Fixed,      // C++ owns the number: emitted as the DSL sugar Uniform(N)/ReadBuffer(N)
        Overridable // C++ owns the default; a program may #define Macro first (surface cells): raw layout()
    };

    struct ShaderBinding
    {
        ShaderBindingKind Kind  = ShaderBindingKind::Fixed;
        uint32_t          Value = 0;
        std::string_view  Macro; // Overridable only
    };

    // A whole block, emitted only when C++ owns the binding. Members inline (Members' fields) and/or one runtime
    // array of RuntimeArrayOf named RuntimeArrayName.
    struct ShaderBlockDecl
    {
        ShaderBlockKind         Kind = ShaderBlockKind::Uniform;
        ShaderBinding           Binding;
        std::string_view        BlockName; // also the RDG parameter name: owners build their RDG parameter from it
        std::string_view        InstanceName; // empty = anonymous instance
        const ShaderStructDecl* Members        = nullptr;
        const ShaderStructDecl* RuntimeArrayOf = nullptr;
        std::string_view        RuntimeArrayName;
    };

    struct ShaderConstantDecl
    {
        enum class Type : uint8_t
        {
            Int,
            Uint,
            Float
        };
        Type             Kind = Type::Int;
        std::string_view Name;
        double           Value = 0.0; // exact for every int/uint below 2^53
    };

    // Text that is not a declaration (vertex input attribute lists): run by the tool, never at engine run time.
    using ShaderTextFn = std::string ( * )();

    // Every struct is emitted as `struct GlslName { … };` AND `#define DESERT_<GlslName>_MEMBERS …` (members of a
    // block whose binding belongs to the program). Requires = other generated paths #included first.
    struct ShaderIncludeDecl
    {
        std::string_view                  Path;
        std::string_view                  SourceHeader; // repo-relative, written in the banner and the manifest
        std::span<const std::string_view> Requires;
        std::span<const ShaderStructDecl* const> Structs;
        std::span<const ShaderBlockDecl>         Blocks;
        std::span<const ShaderConstantDecl>      Constants;
        ShaderTextFn                             Text = nullptr;
    };
} // namespace Desert::Graphic::ShaderProtocols
