#include "MeshVertexPath.hpp"

#include <Engine/Core/Formats/ShaderProgramMeta.hpp>

#include <format>
#include <functional>
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
             // Forward             GBuffer              Glass    Shadow depth            Depth prepass
             { "Static.Forward", "Static.GBuffer", nullptr, "Static.ShadowDepth", "Static.DepthPrepass" },
             { "Skinned.Forward", "Skinned.GBuffer", nullptr, "Skinned.ShadowDepth", "Skinned.DepthPrepass" },
             { "Instanced.Forward", "Instanced.GBuffer", nullptr, "Instanced.ShadowDepth",
               "Instanced.DepthPrepass" },
             { "ParticleSprite.Forward", nullptr, nullptr, nullptr, nullptr },
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

    std::optional<std::string> TemplateCellShader( std::string_view templateName, MeshVertexPath path,
                                                   MeshPass                                       pass,
                                                   const std::function<bool( std::string_view )>& isRegistered )
    {
        auto cell = MeshShaderFor( templateName, path, pass );
        if ( !cell || !MeshCellPath( *cell ) || isRegistered( *cell ) )
            return cell;
        // No Surface block: the default program is the (Static x Forward) cell, and an opaque one's depth-prepass
        // program (Core::Formats::kCustomDepthPrepassPassName) its (Static x DepthPrepass) cell.
        if ( path != MeshVertexPath::Static )
            return std::nullopt;
        if ( pass == MeshPass::Forward )
            return std::string( templateName );
        if ( pass == MeshPass::DepthPrepass )
            if ( std::string prepass =
                      std::format( "{}/{}", templateName, Core::Formats::kCustomDepthPrepassPassName );
                 isRegistered( prepass ) )
                return prepass;
        return std::nullopt;
    }

    ShadowCasterCell ShadowCasterCellFor( Core::Formats::SurfaceBlendMode blend )
    {
        return blend == Core::Formats::SurfaceBlendMode::Masked ? ShadowCasterCell::Own : ShadowCasterCell::Shared;
    }

    std::optional<std::string> ShadowCasterShaderFor( std::string_view                materialTemplate,
                                                      std::string_view                defaultTemplate,
                                                      Core::Formats::SurfaceBlendMode blend, MeshVertexPath path )
    {
        const std::string_view owner =
             ShadowCasterCellFor( blend ) == ShadowCasterCell::Own ? materialTemplate : defaultTemplate;
        return MeshShaderFor( owner, path, MeshPass::ShadowDepth );
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
            case MeshVertexPath::ParticleSprite:
                return 9; // Particles (the pool); the alive list at 10 is bound beside it by ParticleRenderer
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
            case MeshVertexPath::ParticleSprite:
                return "ParticleSprite";
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
            case MeshPass::DepthPrepass:
                return "DepthPrepass";
        }
        return "?";
    }

    std::size_t MeshCellPipelineKeyHash::operator()( const MeshCellPipelineKey& key ) const
    {
        const std::size_t state  = std::hash<const void*>{}( key.PassState );
        const std::size_t shader = std::hash<std::string>{}( key.CellShader );
        return state ^ ( shader + 0x9e3779b97f4a7c15ULL + ( state << 6 ) + ( state >> 2 ) );
    }
} // namespace Desert::Graphic
