#include "MeshVertexPath.hpp"

namespace Desert::Graphic
{
    namespace
    {
        // Indexed [path][pass]. Written out as a literal table rather than an if-chain so that a hole is
        // visible as a hole: the two defects this file exists for were both a missing cell nobody could
        // see, because the combination was never named anywhere.
        constexpr const char* kMeshShaders[kMeshVertexPathCount][kMeshPassCount] = {
             // Forward                  GBuffer               Glass                Shadow depth
             { "StaticMeshPBR", "StaticMeshGBuffer", "StaticMeshGlass", "Shadow" },
             { "SkinnedMeshPBR", nullptr, nullptr, "Shadow_Skinned" },
             { "StaticMeshPBR_Instanced", "StaticMeshGBuffer_Instanced", nullptr, "Shadow_Instanced" },
        };
    } // namespace

    const char* MeshShaderFor( MeshVertexPath path, MeshPass pass )
    {
        return kMeshShaders[static_cast<uint32_t>( path )][static_cast<uint32_t>( pass )];
    }

    std::optional<uint32_t> MeshPathOwnBinding( MeshVertexPath path )
    {
        switch ( path )
        {
            case MeshVertexPath::Skinned:
                return 1; // Bones
            case MeshVertexPath::Instanced:
                return 17; // InstanceTransforms
            case MeshVertexPath::Static:
                break; // reads its model matrix from the push constant; adds no descriptor
        }
        return std::nullopt;
    }

    const char* MeshVertexPathName( MeshVertexPath path )
    {
        switch ( path )
        {
            case MeshVertexPath::Static:
                return "Static";
            case MeshVertexPath::Skinned:
                return "Skinned";
            case MeshVertexPath::Instanced:
                return "Instanced";
        }
        return "?";
    }

    const char* MeshPassName( MeshPass pass )
    {
        switch ( pass )
        {
            case MeshPass::Forward:
                return "Forward";
            case MeshPass::GBuffer:
                return "GBuffer";
            case MeshPass::Glass:
                return "Glass";
            case MeshPass::ShadowDepth:
                return "ShadowDepth";
        }
        return "?";
    }
} // namespace Desert::Graphic
