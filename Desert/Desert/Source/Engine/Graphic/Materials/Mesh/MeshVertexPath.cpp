#include "MeshVertexPath.hpp"

#include <Engine/Core/Formats/ShaderProgramMeta.hpp>
#include <Engine/Core/ShaderCompiler/DShader/DShaderParser.hpp>

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
             // Forward  GBuffer  Glass  Shadow depth  Depth/normal  Virtual shadow depth  View depth
             { "Static.Forward", "Static.GBuffer", nullptr, "Static.ShadowDepth", nullptr,
               "Static.VirtualShadowDepth", "Static.Depth" },
             { "Skinned.Forward", "Skinned.GBuffer", nullptr, "Skinned.ShadowDepth", nullptr,
               "Skinned.VirtualShadowDepth", "Skinned.Depth" },
             { "Instanced.Forward", "Instanced.GBuffer", nullptr, "Instanced.ShadowDepth", nullptr,
               "Instanced.VirtualShadowDepth", "Instanced.Depth" },
             { "ParticleSprite.Forward", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr },
             // The GPU-generated grass casts into the cascades only (MeshRenderer::BuildGpuInstancedShadowDraws).
             { "GrassBlade.Forward", "GrassBlade.GBuffer", nullptr, "GrassBlade.ShadowDepth", nullptr, nullptr,
               "GrassBlade.Depth" },
             { "ParticleRibbon.Forward", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr },
        };

        // THE FORWARD PREPASS RELATION: the forward base pass draws an opaque mesh only where "Forward: Depth
        // Prepass" drew it first (its depth test is CloserOrEqual with no write), so every path drawn in the
        // view's opaque mesh passes - the paths with a G-buffer cell - has a view Depth cell. A row that gains
        // Forward and GBuffer without Depth would leave that path's meshes out of the forward frame; it fails here
        // instead. (The particle paths are drawn over the lit scene by ParticleRenderer, never by the mesh base
        // pass.)
        constexpr bool EveryOpaqueMeshPathHasViewDepth()
        {
            for ( uint32_t path = 0; path < kMeshVertexPathCount; ++path )
            {
                const auto& row = kMeshCells[path];
                if ( row[static_cast<uint32_t>( MeshPass::GBuffer )] != nullptr &&
                     row[static_cast<uint32_t>( MeshPass::Depth )] == nullptr )
                    return false;
            }
            return true;
        }
        static_assert( EveryOpaqueMeshPathHasViewDepth(),
                       "a mesh path with a GBuffer cell has no Depth cell: the forward prepass cannot admit it" );

        // THE VIRTUAL-SHADOW COOK RELATION: every opaque mesh path (a G-buffer cell) is drawn forward by
        // MeshRenderer through its VSM twin (ForwardVirtualCell), and the cook lists the twins of exactly
        // kMeshRendererForwardPaths; a path missing there would draw a permutation no package ships.
        constexpr bool EveryOpaqueMeshPathHasForwardVsmTwinCooked()
        {
            for ( uint32_t path = 0; path < kMeshVertexPathCount; ++path )
            {
                const auto& row = kMeshCells[path];
                if ( row[static_cast<uint32_t>( MeshPass::GBuffer )] == nullptr )
                    continue;
                if ( row[static_cast<uint32_t>( MeshPass::Forward )] == nullptr )
                    return false;
                bool listed = false;
                for ( const MeshVertexPath forward : kMeshRendererForwardPaths )
                    listed = listed || static_cast<uint32_t>( forward ) == path;
                if ( !listed )
                    return false;
            }
            for ( const MeshVertexPath forward : kMeshRendererForwardPaths )
                if ( kMeshCells[static_cast<uint32_t>( forward )][static_cast<uint32_t>( MeshPass::Forward )] ==
                     nullptr )
                    return false;
            return true;
        }
        static_assert( EveryOpaqueMeshPathHasForwardVsmTwinCooked(),
                       "an opaque mesh path is missing from kMeshRendererForwardPaths (or has no Forward cell): "
                       "its DESERT_LOCAL_SHADOW_VSM permutation would not be cooked" );
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

    std::optional<std::string> FrontLayerShaderFor( std::string_view templateName )
    {
        // A material names its template, or (a generic override) one of the template's cells: the template is the
        // name before the cell.
        templateName = templateName.substr( 0, templateName.find( '/' ) );
        if ( templateName.empty() )
            return std::nullopt;
        return std::format( "{}/{}", templateName,
                            Core::Preprocess::SurfaceCellName( Core::Preprocess::kSurfaceFrontLayerPath,
                                                               Core::Preprocess::kSurfaceFrontLayerPass ) );
    }

    std::optional<std::string> TemplateCellShader( std::string_view templateName, MeshVertexPath path,
                                                   MeshPass                                       pass,
                                                   const std::function<bool( std::string_view )>& isRegistered )
    {
        // A template without a Surface block declares its depth-normal program as a named pass of its own (the
        // parser refuses an opaque one that does not); a surface template is in the G-buffer and has none.
        if ( pass == MeshPass::DepthNormal )
        {
            if ( path != MeshVertexPath::Static || templateName.empty() )
                return std::nullopt;
            std::string own = std::format( "{}/{}", templateName, Core::Preprocess::kGenericDepthNormalPass );
            return isRegistered( own ) ? std::optional<std::string>( std::move( own ) ) : std::nullopt;
        }
        auto cell = MeshShaderFor( templateName, path, pass );
        if ( cell && MeshCellPath( *cell ) && !isRegistered( *cell ) )
            cell = ( path == MeshVertexPath::Static && pass == MeshPass::Forward )
                        ? std::optional<std::string>( templateName )
                        : std::nullopt;
        return cell;
    }

    ShadowCasterCell ShadowCasterCellFor( Core::Formats::SurfaceBlendMode blend, const bool parallax )
    {
        return blend == Core::Formats::SurfaceBlendMode::Masked || parallax ? ShadowCasterCell::Own
                                                                            : ShadowCasterCell::Shared;
    }

    std::optional<std::string> ShadowCasterShaderFor( std::string_view                materialTemplate,
                                                      std::string_view                defaultTemplate,
                                                      Core::Formats::SurfaceBlendMode blend, const bool parallax,
                                                      MeshVertexPath path )
    {
        const std::string_view owner =
             ShadowCasterCellFor( blend, parallax ) == ShadowCasterCell::Own ? materialTemplate : defaultTemplate;
        auto cell = MeshShaderFor( owner, path, MeshPass::ShadowDepth );
        if ( cell && parallax )
            return ParallaxCellOf( *cell );
        return cell;
    }

    std::string ParallaxCellOf( const std::string_view cellShader )
    {
        return std::format( "{}{}", cellShader, Core::Preprocess::kSurfaceParallaxCellSuffix );
    }

    bool IsParallaxCell( const std::string_view shaderName )
    {
        return shaderName.ends_with( Core::Preprocess::kSurfaceParallaxCellSuffix );
    }

    std::optional<MeshVertexPath> MeshCellPath( std::string_view shaderName )
    {
        // A `.Parallax` permutation is drawn on its base cell's path.
        if ( IsParallaxCell( shaderName ) )
            shaderName.remove_suffix( Core::Preprocess::kSurfaceParallaxCellSuffix.size() );
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
            case MeshVertexPath::GrassBlade:
                return 17; // InstanceTransforms; the blade shape at 13 is bound beside it by the mesh renderer
            case MeshVertexPath::ParticleSprite:
            case MeshVertexPath::ParticleRibbon:
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
            case MeshVertexPath::GrassBlade:
                return "GrassBlade";
            case MeshVertexPath::ParticleRibbon:
                return "ParticleRibbon";
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
            case MeshPass::DepthNormal:
                return "DepthNormal";
            case MeshPass::VirtualShadowDepth:
                return "VirtualShadowDepth";
            case MeshPass::Depth:
                return "Depth";
        }
        return "?";
    }

    std::size_t MeshCellPipelineKeyHash::operator()( const MeshCellPipelineKey& key ) const
    {
        const std::size_t state  = std::hash<const void*>{}( key.PassState );
        const std::size_t shader =
             std::hash<std::string>{}( key.CellShader ) ^ ( std::hash<uint64_t>{}( key.Variant ) << 1 );
        return state ^ ( shader + 0x9e3779b97f4a7c15ULL + ( state << 6 ) + ( state >> 2 ) );
    }
} // namespace Desert::Graphic
