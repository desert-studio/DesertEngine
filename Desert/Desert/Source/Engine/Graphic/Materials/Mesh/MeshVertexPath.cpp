#include "MeshVertexPath.hpp"

#include <format>
#include <string_view>

namespace Desert::Graphic
{
    namespace
    {
        // Indexed [path][pass]. Written out as a literal table rather than an if-chain so that a hole is
        // visible as a hole: the two defects this file exists for were both a missing cell nobody could
        // see, because the combination was never named anywhere. Every entry is a CELL of whichever surface
        // template the material names ("<Path>.<Pass>", DShaderParser's SurfaceCellName); shadow depth too: an
        // opaque template's ShadowDepth cell is the path's vertex header plus Mesh/Surface/Pass_ShadowDepth,
        // with no surface evaluated. Glass is not a cell of its own: the translucent template's Forward cell draws
        // it.
        constexpr const char* kMeshCells[kMeshVertexPathCount][kMeshPassCount] = {
             // Forward             GBuffer              Glass    Shadow depth
             { "Static.Forward", "Static.GBuffer", nullptr, "Static.ShadowDepth" },
             { "Skinned.Forward", "Skinned.GBuffer", nullptr, "Skinned.ShadowDepth" },
             { "Instanced.Forward", "Instanced.GBuffer", nullptr, "Instanced.ShadowDepth" },
        };
    } // namespace

    const char* MeshCellFor( MeshVertexPath path, MeshPass pass )
    {
        return kMeshCells[static_cast<uint32_t>( path )][static_cast<uint32_t>( pass )];
    }

    std::optional<std::string> MeshShaderFor( std::string_view templateName, MeshVertexPath path, MeshPass pass )
    {
        // The translucency pass draws a translucent template's OWN lit cell (UE: each translucent material is
        // drawn by its own shader in the translucency pass) — there is no translucency program of the engine's.
        if ( path == MeshVertexPath::Static && pass == MeshPass::Glass )
            return templateName.empty() ? std::nullopt
                                        : std::optional<std::string>( std::format(
                                               "{}/{}", templateName, MeshCellFor( path, MeshPass::Forward ) ) );
        const char* cell = MeshCellFor( path, pass );
        if ( cell == nullptr || templateName.empty() )
            return std::nullopt;
        return std::format( "{}/{}", templateName, cell );
    }

    std::optional<MeshVertexPath> MeshCellPath( std::string_view shaderName )
    {
        for ( uint32_t p = 0; p < kMeshVertexPathCount; ++p )
            for ( uint32_t s = 0; s < kMeshPassCount; ++s )
            {
                // "<Template>/<Cell>": the part after the template's slash IS the cell, whichever template.
                const char* cell = kMeshCells[p][s];
                if ( cell == nullptr )
                    continue;
                const std::string_view c( cell );
                if ( shaderName.size() > c.size() + 1 && shaderName.ends_with( c ) &&
                     shaderName[shaderName.size() - c.size() - 1] == '/' )
                    return static_cast<MeshVertexPath>( p );
            }
        return std::nullopt;
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
