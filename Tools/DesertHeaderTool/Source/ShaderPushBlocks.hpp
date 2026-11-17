#pragma once

// PushConstant blocks of the .shader programs -> the C++ push structs the passes fill
// (Engine/Generated/ShaderPushConstants.gen.hpp). One declaration of a pass's push block, the .shader one: the
// C++ side is generated from it on every build, so the two cannot diverge (UE SHADER_PARAMETER_STRUCT, one
// declaration; here the shader text is that declaration because the DSL owns the block).
//
// Layout: `PushConstant` is `layout(push_constant) uniform` (DShaderParser.cpp), and a Vulkan push block packs
// std430. Every generated member is 4-byte aligned in C++ (float, int32, uint32, glm vectors/matrices), so the
// std430 alignment gaps are explicit Std430PadN members and sizeof() is the block's end offset, exactly the size the
// shader reflection reports for the push range.

#include <cstdint>
#include <string>
#include <vector>

namespace Desert::HeaderTool
{
    struct ShaderSource
    {
        std::string RelativePath; // under the shader root, generic separators
        std::string Text;
    };

    struct ShaderPushField
    {
        std::string   CppType;
        std::string   Name;
        std::uint32_t Count  = 0; // array extent, 0 = not an array
        std::uint32_t Offset = 0; // std430 byte offset
    };

    struct ShaderPushStruct
    {
        std::string                  Name; // C++ struct name: the shader's, then its pass/stage/block when needed
        std::string                  Source; // "<relative path> (<pass> <stage>)"
        std::uint32_t                Size = 0;
        std::vector<ShaderPushField> Fields;
    };

    struct ShaderPushScan
    {
        std::vector<ShaderPushStruct> Structs; // sorted by name, one per distinct name
        std::vector<std::string>      Errors;  // "path:line: message"; any one fails the build
    };

    // Every PushConstant block of `sources`. A block whose layout cannot be expressed exactly (a type with no
    // C++ twin, a preprocessor directive inside the block, an array whose std430 stride is not its C++ size)
    // is an error, never a skipped block.
    ShaderPushScan ScanShaderPushBlocks( const std::vector<ShaderSource>& sources ) noexcept;
} // namespace Desert::HeaderTool
