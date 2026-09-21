// WHICH VULKAN PIPELINES A PLAYER'S BUILD IS ALLOWED TO CREATE.
//
// ── THE MEASUREMENT THIS SUITE WAS WRITTEN AROUND ────────────────────────────────────────────────
//
// A Shipping Runtime boot on this tree creates 67 Vulkan pipelines: 39 graphics and 28 COMPUTE. The
// note in Common/Core/DevInstruments.hpp said 38, and it was not wrong when it was written — it was
// counting the graphics side only, because only VulkanPipeline logs itself ("Created X VulkanPipeline")
// and VulkanPipelineCompute logs nothing at all. A number nobody can re-derive goes stale silently,
// which is why this file pins a RELATION and derives every count it prints.
//
// Four of those 67 were reachable only through Graphic::DebugViewState, and nothing in the player's
// source set writes one: SceneRenderer::SetDebugView has no caller in Runtime/Source,
// Desert/Desert/Source or Desert/Common/Source, and those fields cannot arrive from a .desce either
// (К2 took them out of the scene format; Desert/Tests/Engine/SceneDebugFields derives that ban from the
// struct). They were built at every player's startup and could not be bound by anything.
//
// ── WHAT IT COST, AND WHY THE NUMBER IS SMALL ────────────────────────────────────────────────────
//
// Measured from each pipeline's OWN vkCreate* line, not from a frame-to-frame difference:
//
//     phase                                     first-ever run      every run after
//     SPIR-V compilation (our ShaderCache)            554 ms           293-331 ms
//     pipeline creation (vkCreate*)                 9 611 ms            81-104 ms
//       of which the four developer pipelines        127.2 ms           1.1-2.0 ms
//
// The gap between the two columns is NOT our cache: deleting Cooked/ShaderCache puts the SPIR-V phase
// back to 554 ms and leaves pipeline creation at 81.5 ms. What warms is a machine-level Metal shader
// cache, which survives both deleting our caches and relinking the binary. So the saving is 127 ms for
// a player whose machine has never seen these shaders and ~1 ms for everyone else — and 1 ms is BELOW
// the run-to-run spread of the phase it sits in (±12 ms over three runs). Time is therefore not the
// argument this suite defends. The argument is that an instrument a player's binary cannot use is
// surface a player's binary should not carry, and that the next pipeline added without a thought will
// be added by somebody who never read any of this.
//
// ── THE RELATION ─────────────────────────────────────────────────────────────────────────────────
//
// Every pipeline-creation site in the PLAYER'S source set is registered below with a verdict, and the
// set of sites is DERIVED from the tree. Three ways to be red, and each is a real failure mode:
//
//   1. a site with no row — somebody added a pipeline and nobody decided whether a player gets it;
//   2. a row marked DeveloperOnly whose site is not inside `#if DESERT_DEV_INSTRUMENTS` — the boundary
//      was removed, or never drawn, and the pipeline is back in the game;
//   3. a row marked Shipped whose site IS inside the boundary — the mirror defect, and the more
//      expensive one: a pipeline the product needs would vanish from the player's build only, where
//      nobody is running a test.
//
// A stale row (a row whose site no longer exists) is red as well, so deleting a pipeline is a diff with
// an author rather than a line that quietly stops meaning anything.
//
// ── AND THE HALF OF THE OLD NOTE THAT WAS WRONG ──────────────────────────────────────────────────
//
// DevInstruments.hpp also named "the selection-outline family (SilhouettePipeline,
// SilhouetteSkinnedPipeline, JFA_*) which draws the EDITOR's selection highlight and has no caller in a
// player at all". It has one. `MeshECSSystem` computes `outlined = isSelected || mesh.OutlineDraw`, and
// `OutlineDraw` is a SERIALIZED field of StaticMeshComponent with a checkbox in the Materials panel —
// so an author who ticks "Draw outline" ships an outlined mesh. On top of that JFA_Init and JFA_Final
// run on every frame of every build regardless of selection, because the composite is what hands the
// scene colour to tonemap. Both halves of that are relations between two places, so both are asserted
// below rather than left in a paragraph: the last three defects of this shape in this repository were
// all "a comment described a mechanism the tree does not have".
//
// COMMENTS ARE STRIPPED BEFORE ANYTHING IS SEARCHED FOR — this file's own prose names every token it
// looks for, and a census that shoots at prose gets switched off, twice here already.

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    fs::path RepoRoot()
    {
        fs::path prefix = ".";
        for ( int up = 0; up < 6; ++up )
        {
            if ( std::ifstream( ( prefix / "Desert/Common/Source/Common/Core/DevInstruments.hpp" ).string() ) )
                return prefix;
            prefix /= "..";
        }
        return {};
    }

    std::string Read( const fs::path& file )
    {
        const std::ifstream in( file.string() );
        if ( !in )
            return {};
        std::ostringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }

    // Line and block comments out, string literals kept: the DebugName that identifies a site IS a
    // string literal, and every paragraph in this repository quotes the tokens it is about.
    std::string StripComments( const std::string& source )
    {
        std::string out;
        out.reserve( source.size() );
        enum class State
        {
            Code,
            Line,
            Block,
            String
        } state = State::Code;

        for ( size_t i = 0; i < source.size(); ++i )
        {
            const char c    = source[i];
            const char next = i + 1 < source.size() ? source[i + 1] : '\0';
            switch ( state )
            {
                case State::Code:
                    if ( c == '/' && next == '/' )
                    {
                        state = State::Line;
                        ++i;
                    }
                    else if ( c == '/' && next == '*' )
                    {
                        state = State::Block;
                        ++i;
                    }
                    else
                    {
                        if ( c == '"' )
                            state = State::String;
                        out.push_back( c );
                    }
                    break;
                case State::Line:
                    if ( c == '\n' )
                    {
                        state = State::Code;
                        out.push_back( c );
                    }
                    break;
                case State::Block:
                    if ( c == '*' && next == '/' )
                    {
                        state = State::Code;
                        ++i;
                    }
                    else if ( c == '\n' )
                    {
                        out.push_back( c );
                    }
                    break;
                case State::String:
                    out.push_back( c );
                    if ( c == '\\' )
                    {
                        if ( i + 1 < source.size() )
                            out.push_back( source[++i] );
                    }
                    else if ( c == '"' )
                    {
                        state = State::Code;
                    }
                    break;
            }
        }
        return out;
    }

    std::vector<std::string> Lines( const std::string& text )
    {
        std::vector<std::string> out;
        std::istringstream       in( text );
        std::string              line;
        while ( std::getline( in, line ) )
            out.push_back( line );
        return out;
    }

    std::string Trim( const std::string& s )
    {
        const size_t begin = s.find_first_not_of( " \t\r" );
        if ( begin == std::string::npos )
            return {};
        const size_t end = s.find_last_not_of( " \t\r" );
        return s.substr( begin, end - begin + 1 );
    }

    // ── THE PLAYER'S OWN SOURCE SET ─────────────────────────────────────────────────────────────────
    //
    // The three roots the `Runtime` premake compiles and links, exactly as ShippingBoundary derives
    // them. The editor is not here: it is a development tool, so a pipeline it creates for itself is not
    // a violation of anything — and EditorGridPass, EditorColliderPass and EditorCubemapPreviewPass are
    // compiled into the Editor target and into nothing else, so they have never been able to reach a
    // packaged game.
    const std::vector<std::string>& PlayerSourceRoots()
    {
        static const std::vector<std::string> roots = { "Desert/Desert/Source", "Desert/Common/Source",
                                                        "Runtime/Source" };
        return roots;
    }

    // THE FACTORY ITSELF IS NOT A CREATION SITE. Pipeline.cpp is where every Create() lands and
    // PipelineCache.hpp is the dedup in front of it; both mention the call by definition and neither
    // decides that a pipeline should exist. Listing them as rows would put two permanent "Shipped"
    // entries in the register that mean nothing.
    const std::set<std::string>& FactoryFiles()
    {
        static const std::set<std::string> files = { "Desert/Desert/Source/Engine/Graphic/Pipeline.cpp",
                                                     "Desert/Desert/Source/Engine/Graphic/PipelineCache.hpp" };
        return files;
    }

    // What a creation site looks like. `GetOrCreate` alone would be wrong — ParticleRenderer has a
    // per-emitter GetOrCreate of its own and it builds buffers, not pipelines — so the pipeline cache is
    // matched through the one spelling the tree uses to reach it.
    bool IsCreationSite( const std::string& line )
    {
        return line.find( "GraphicsPipeline::Create" ) != std::string::npos ||
               line.find( "ComputePipeline::Create" ) != std::string::npos ||
               line.find( "GetPipelineCache().GetOrCreate" ) != std::string::npos;
    }

    struct Found
    {
        std::string File;      ///< repo-relative, forward slashes
        std::string Name;      ///< the DebugName expression that names this pipeline
        bool        BehindGate = false;
        int         Line       = 0;
    };

    // The nearest preceding `DebugName = <expr>` in the same file. Every site in the tree has one within
    // a few lines — a pipeline without a DebugName cannot be identified in a device-lost report either,
    // so "no name found" is reported as its own failure rather than silently skipped.
    std::string DebugNameAbove( const std::vector<std::string>& lines, size_t at )
    {
        constexpr size_t kLookBack = 60;
        const size_t     stop      = at >= kLookBack ? at - kLookBack : 0;
        for ( size_t i = at + 1; i-- > stop; )
        {
            const size_t key = lines[i].find( "DebugName" );
            if ( key == std::string::npos )
                continue;
            const size_t eq = lines[i].find( '=', key );
            if ( eq == std::string::npos )
                continue;
            std::string value = lines[i].substr( eq + 1 );
            const size_t cut  = value.find_first_of( ",;}" );
            if ( cut != std::string::npos )
                value = value.substr( 0, cut );
            return Trim( value );
            }
        return {};
    }

    // Every creation site in the player's source set, with the answer to "is it inside
    // `#if DESERT_DEV_INSTRUMENTS`". `#else` flips the answer; `#if !DESERT_DEV_INSTRUMENTS` is the
    // shipping-only arm and is therefore NOT behind the gate.
    const std::vector<Found>& Sites()
    {
        static const std::vector<Found> found = []
        {
            std::vector<Found> out;
            const fs::path     root = RepoRoot();
            if ( root.empty() )
                return out;

            std::vector<fs::path> files;
            for ( const std::string& sub : PlayerSourceRoots() )
            {
                const fs::path dir = root / sub;
                if ( !fs::exists( dir ) )
                    continue;
                for ( const auto& entry : fs::recursive_directory_iterator( dir ) )
                {
                    if ( !entry.is_regular_file() )
                        continue;
                    const std::string ext = entry.path().extension().string();
                    if ( ext == ".cpp" || ext == ".hpp" || ext == ".h" )
                        files.push_back( entry.path() );
                }
            }
            std::sort( files.begin(), files.end() );

            for ( const fs::path& file : files )
            {
                std::string relative = fs::relative( file, root ).generic_string();
                if ( FactoryFiles().count( relative ) )
                    continue;

                const std::vector<std::string> lines = Lines( StripComments( Read( file ) ) );
                std::vector<bool>              gate; // one entry per open #if
                for ( size_t i = 0; i < lines.size(); ++i )
                {
                    const std::string trimmed = Trim( lines[i] );
                    if ( trimmed.rfind( "#if", 0 ) == 0 )
                    {
                        const size_t token  = trimmed.find( "DESERT_DEV_INSTRUMENTS" );
                        const bool   negated = trimmed.find( '!' ) != std::string::npos &&
                                             trimmed.find( '!' ) < token;
                        gate.push_back( token != std::string::npos && !negated );
                    }
                    else if ( trimmed.rfind( "#else", 0 ) == 0 && !gate.empty() )
                    {
                        gate.back() = !gate.back();
                    }
                    else if ( trimmed.rfind( "#endif", 0 ) == 0 && !gate.empty() )
                    {
                        gate.pop_back();
                    }

                    if ( !IsCreationSite( lines[i] ) )
                        continue;

                    Found site;
                    site.File       = relative;
                    site.Name       = DebugNameAbove( lines, i );
                    site.BehindGate = std::find( gate.begin(), gate.end(), true ) != gate.end();
                    site.Line       = static_cast<int>( i ) + 1;
                    out.push_back( site );
                }
            }
            return out;
        }();
        return found;
    }

    // ── THE REGISTER ────────────────────────────────────────────────────────────────────────────────
    //
    // One row per creation site, keyed by (file, the DebugName expression). The count is derived from
    // the rows and from the tree and the two are compared; nothing here states a number.
    //
    // `Why` is mandatory on a DeveloperOnly row and asserted to be non-empty: the whole value of this
    // register is that the next person inherits the reason instead of the surprise. A Shipped row is the
    // unremarkable case and carries a reason only where the answer is not obvious from the name.
    enum class Verdict
    {
        Shipped,       ///< the product needs it; it must NOT be behind the boundary
        DeveloperOnly, ///< only a developer can reach it; it MUST be behind the boundary
    };

    struct Row
    {
        const char* File;
        const char* Name;
        Verdict     V;
        const char* Why;
    };

    const std::vector<Row>& Register()
    {
        static const std::vector<Row> rows = {
             // ── the four the boundary exists for ────────────────────────────────────────────────────
             { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Mesh/MeshRenderer.cpp",
               "\"StaticMeshWireframe\"", Verdict::DeveloperOnly,
               "selected only by DebugViewState::WireframeMode, which no player-side file writes" },
             { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Mesh/MeshRenderer.cpp",
               "\"DebugLinePipeline\"", Verdict::DeveloperOnly,
               "draws AABB wireframes, gated by DebugViewState::ShowBoundingBoxes" },
             { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Mesh/MeshRenderer.cpp",
               "\"OverdrawPipeline\"", Verdict::DeveloperOnly,
               "the overdraw heat map, gated by DebugViewState::DeferredDebug" },
             { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Mesh/MeshRenderer.cpp",
               "\"OverdrawResolvePipeline\"", Verdict::DeveloperOnly,
               "the overdraw heat map's fullscreen resolve, same toggle" },

             // ── mesh geometry, shadows and the selection outline ────────────────────────────────────
             { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Mesh/MeshRenderer.cpp",
               "\"GenericMesh_\" + shaderName", Verdict::Shipped, "" },
             { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Mesh/MeshRenderer.cpp",
               "\"SkinnedMesh_Load\"", Verdict::Shipped, "" },
             { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Mesh/MeshRenderer.cpp",
               "\"StaticMeshGeometry\"", Verdict::Shipped, "" },
             { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Mesh/MeshRenderer.cpp",
               "\"StaticMeshGeometryInstanced\"", Verdict::Shipped, "" },
             { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Mesh/MeshRenderer.cpp",
               "\"StaticMeshGBuffer\"", Verdict::Shipped, "" },
             { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Mesh/MeshRenderer.cpp",
               "\"StaticMeshRSM\"", Verdict::Shipped, "reflective shadow map — the GI bounce's caster" },
             { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Mesh/MeshRenderer.cpp",
               "\"StaticMeshGBufferInstanced\"", Verdict::Shipped, "" },
             { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Mesh/MeshRenderer.cpp",
               "\"StaticMeshGlass\"", Verdict::Shipped, "" },
             { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Mesh/MeshRenderer.cpp",
               "\"SkinnedMeshGeometry\"", Verdict::Shipped, "" },
             { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Mesh/MeshRenderer.cpp",
               "\"SilhouettePipeline\"", Verdict::Shipped,
               "the outline mask — reachable in a player through the serialized StaticMeshComponent"
               " field OutlineDraw, see OutlineFamilyIsReachableFromASavedScene below" },
             { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Mesh/MeshRenderer.cpp",
               "\"SilhouetteSkinnedPipeline\"", Verdict::Shipped, "same, for skinned meshes" },
             { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Mesh/MeshRenderer.cpp",
               "\"ShadowPipeline\"", Verdict::Shipped, "" },
             { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Mesh/MeshRenderer.cpp",
               "\"ShadowPipelineInstanced\"", Verdict::Shipped, "" },
             { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Mesh/MeshRenderer.cpp",
               "\"ShadowPipelineSkinned\"", Verdict::Shipped, "" },

             // ── the Jump Flood outline ──────────────────────────────────────────────────────────────
             { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/PostProcessing/JumpFloodOutlineRenderer.cpp",
               "debugName", Verdict::Shipped,
               "JFA_Init, JFA_Step and JFA_Final from one helper. Init and Final run on EVERY frame of"
               " every build: the composite is what hands the scene colour to tonemap" },

             // ── sky, terrain, clouds, fog ───────────────────────────────────────────────────────────
             { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Skybox/SkyboxRenderer.cpp", "debugName",
               Verdict::Shipped, "" },
             { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Skybox/SkyboxRenderer.cpp",
               "\"ProceduralSky\"", Verdict::Shipped, "" },
             { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Skybox/SkyboxRenderer.cpp", "name",
               Verdict::Shipped, "the atmosphere LUT compute family" },
             { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Terrain/TerrainRenderer.cpp",
               "\"TerrainPipeline\"", Verdict::Shipped, "" },
             { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Clouds/VolumetricCloudRenderer.cpp",
               "kMarchShaderName", Verdict::Shipped, "" },
             { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Clouds/VolumetricCloudRenderer.cpp",
               "kShadowMapShaderName", Verdict::Shipped, "" },
             { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Clouds/VolumetricCloudRenderer.cpp",
               "kSkyOcclusionShaderName", Verdict::Shipped, "" },
             { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Clouds/VolumetricCloudRenderer.cpp",
               "kResolveShaderName", Verdict::Shipped, "" },
             { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Clouds/VolumetricCloudRenderer.cpp",
               "kCompositeShaderName", Verdict::Shipped, "" },
             { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Fog/HeightFogRenderer.cpp",
               "kFogShaderName", Verdict::Shipped, "" },
             { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Fog/HeightFogRenderer.cpp",
               "kApplyShaderName", Verdict::Shipped, "" },

             // ── deferred shading and its screen-space passes ────────────────────────────────────────
             { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Deferred/CopyRenderer.hpp", "\"Copy\"",
               Verdict::Shipped, "" },
             { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Deferred/DeferredLightingRenderer.hpp",
               "\"DeferredLighting\"", Verdict::Shipped, "" },
             { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Deferred/GIResolveRenderer.hpp",
               "\"GIResolve\"", Verdict::Shipped, "" },
             { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Deferred/GIResolveRenderer.hpp",
               "\"GITemporalResolve\"", Verdict::Shipped, "" },
             { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Deferred/SSAORenderer.hpp", "\"SSAO\"",
               Verdict::Shipped, "" },
             { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Deferred/SSRRenderer.hpp", "\"SSRTrace\"",
               Verdict::Shipped, "" },
             { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Deferred/SSRRenderer.hpp", "\"SSRResolve\"",
               Verdict::Shipped, "" },
             { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Deferred/SSRRenderer.hpp",
               "\"SSRComposite\"", Verdict::Shipped, "" },

             // ── post processing ─────────────────────────────────────────────────────────────────────
             { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/PostProcessing/AutoExposureRenderer.cpp",
               "name", Verdict::Shipped, "the histogram compute family" },
             { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/PostProcessing/BackdropBlurRenderer.hpp",
               "\"BackdropBlurDownsample\"", Verdict::Shipped, "" },
             { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/PostProcessing/BloomRenderer.cpp",
               "shaderName", Verdict::Shipped, "" },
             { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/PostProcessing/FXAARenderer.cpp",
               "debugName", Verdict::Shipped, "" },
             { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/PostProcessing/LensFlareRenderer.cpp",
               "shaderName", Verdict::Shipped, "" },
             { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/PostProcessing/LightShaftRenderer.cpp",
               "shaderName", Verdict::Shipped, "" },
             { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/PostProcessing/SMAARenderer.cpp",
               "std::string( name )", Verdict::Shipped, "" },
             { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/PostProcessing/TonemapRenderer.cpp",
               "debugName", Verdict::Shipped, "" },

             // ── particles ───────────────────────────────────────────────────────────────────────────
             { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Particles/ParticleRenderer.cpp",
               "\"ParticleSimulate\"", Verdict::Shipped, "" },
             { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Particles/ParticleRenderer.cpp",
               "\"ParticleAdd\"", Verdict::Shipped, "" },
             { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Particles/ParticleRenderer.cpp",
               "\"ParticleAlpha\"", Verdict::Shipped, "" },

             // ── environment baking (IBL) ────────────────────────────────────────────────────────────
             { "Desert/Desert/Source/Engine/Graphic/ComputeImages.cpp", "\"BakeProceduralSky\"",
               Verdict::Shipped, "" },
             { "Desert/Desert/Source/Engine/Graphic/ComputeImages.cpp", "spec.Tag", Verdict::Shipped,
               "the cubemap/mip/irradiance/prefilter compute family, named by its caller" },

             // ── 2D, UI and present ──────────────────────────────────────────────────────────────────
             { "Desert/Desert/Source/Engine/Graphic/Render2D/Render2D.cpp", "\"UI2DPipeline\"",
               Verdict::Shipped, "" },
             { "Desert/Desert/Source/Engine/Graphic/Render2D/Render2D.cpp", "\"UITextPipeline\"",
               Verdict::Shipped, "" },
             { "Desert/Desert/Source/Engine/Graphic/Render2D/Render2D.cpp", "\"UIGlassPipeline\"",
               Verdict::Shipped, "" },
             { "Desert/Desert/Source/Engine/Graphic/Render2D/UIMaterialCache.cpp", "\"UIMat_\" + shaderName",
               Verdict::Shipped, "" },
             { "Runtime/Source/RuntimeLayer.cpp", "\"SwapchainBlitPipeline\"", Verdict::Shipped,
               "the present blit — the one pipeline without which nothing reaches the screen" },
        };
        return rows;
    }

    std::string Key( const std::string& file, const std::string& name )
    {
        return file + "  ->  " + name;
    }
} // namespace

// THE SCANNER FOUND SOMETHING AT ALL. Every absence check below passes on an empty list, and an empty
// list is exactly what a moved source root or a broken comment stripper produces — Ф4's shape, at the
// one gate whose job is to say what exists.
TEST( ShippingPipelines, TheTreeWasActuallyRead )
{
    ASSERT_FALSE( RepoRoot().empty() ) << "could not locate the repository root from the test binary";
    ASSERT_GT( Sites().size(), 40u )
         << "the scanner found " << Sites().size()
         << " pipeline-creation sites in the player's source set; the tree has dozens, so this is a "
            "scanner that stopped working rather than a tree that lost its pipelines";
}

// Relation: a pipeline this engine creates has a DebugName. It is what a device-lost report names the
// pipeline that died BY, which is a player's bug report rather than a developer's convenience, and it is
// also the only stable identity this register can be keyed on.
TEST( ShippingPipelines, EveryCreationSiteNamesItsPipeline )
{
    for ( const auto& site : Sites() )
        EXPECT_FALSE( site.Name.empty() )
             << site.File << ":" << site.Line
             << " builds a pipeline with no DebugName above it. A nameless pipeline cannot be identified "
                "in a device-lost report and cannot be registered here.";
}

// Relation 1: THE GROWTH GATE. A new pipeline is a decision about what a player's binary contains, and
// this is the line that makes somebody take it.
TEST( ShippingPipelines, EveryCreationSiteIsRegistered )
{
    std::set<std::string> registered;
    for ( const auto& row : Register() )
        registered.insert( Key( row.File, row.Name ) );

    for ( const auto& site : Sites() )
    {
        EXPECT_TRUE( registered.count( Key( site.File, site.Name ) ) )
             << site.File << ":" << site.Line << " creates the pipeline " << site.Name
             << " and no row in Desert/Tests/Runtime/ShippingPipelines claims it.\n"
                "  Add one, with a verdict: Shipped (the product needs it) or DeveloperOnly (only a "
                "developer can reach it, and it then has to sit behind #if DESERT_DEV_INSTRUMENTS).";
    }
}

// The other direction: a row whose site is gone stops meaning anything, and a register full of rows that
// mean nothing is a register people stop reading.
TEST( ShippingPipelines, NoRegisteredRowHasLostItsSite )
{
    std::set<std::string> present;
    for ( const auto& site : Sites() )
        present.insert( Key( site.File, site.Name ) );

    for ( const auto& row : Register() )
        EXPECT_TRUE( present.count( Key( row.File, row.Name ) ) )
             << "the register claims a pipeline " << row.Name << " in " << row.File
             << ", and no such creation site exists any more. Delete the row in the same change that "
                "deleted the pipeline.";
}

// Relations 2 and 3: the verdict and the source text have to agree, in BOTH directions. The mirror case
// is the expensive one — a Shipped pipeline accidentally put behind the boundary disappears from the
// player's build only, where no test runs.
TEST( ShippingPipelines, EveryVerdictMatchesTheSourceText )
{
    std::map<std::string, const Row*> byKey;
    for ( const auto& row : Register() )
        byKey[Key( row.File, row.Name )] = &row;

    for ( const auto& site : Sites() )
    {
        const auto found = byKey.find( Key( site.File, site.Name ) );
        if ( found == byKey.end() )
            continue; // EveryCreationSiteIsRegistered reports this one

        const Row& row = *found->second;
        if ( row.V == Verdict::DeveloperOnly )
        {
            EXPECT_TRUE( site.BehindGate )
                 << site.File << ":" << site.Line << ": " << site.Name
                 << " is registered as a developer-only pipeline (" << row.Why
                 << ") but its creation site is NOT inside #if DESERT_DEV_INSTRUMENTS, so a player's "
                    "build creates it.";
        }
        else
        {
            EXPECT_FALSE( site.BehindGate )
                 << site.File << ":" << site.Line << ": " << site.Name
                 << " is registered as part of the product but its creation site sits inside "
                    "#if DESERT_DEV_INSTRUMENTS, so the PLAYER'S build is the only one without it.";
        }
    }
}

// A DeveloperOnly row without a reason is the promise this whole mechanism exists to stop being made.
TEST( ShippingPipelines, EveryDeveloperOnlyRowSaysWhy )
{
    for ( const auto& row : Register() )
        if ( row.V == Verdict::DeveloperOnly )
            EXPECT_GT( std::string( row.Why ).size(), 20u )
                 << row.Name << " is cut from a player's build and the register does not say why.";
}

// ── WHY THE FOUR ARE DEVELOPER-ONLY, AS A RELATION RATHER THAN AS A PARAGRAPH ───────────────────────
//
// All four are reachable only through Graphic::DebugViewState, and the reason nothing can reach them in
// a player is that NOTHING IN THE PLAYER'S SOURCE SET WRITES ONE. If somebody gives the runtime a way to
// set a debug view — a console command, a scripting binding — that ceases to be true, and the four rows
// above become wrong on the same day. This is the line that says so.
TEST( ShippingPipelines, NothingInAPlayerCanSetADebugView )
{
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    std::vector<std::string> callers;
    for ( const std::string& sub : PlayerSourceRoots() )
    {
        const fs::path dir = root / sub;
        if ( !fs::exists( dir ) )
            continue;
        for ( const auto& entry : fs::recursive_directory_iterator( dir ) )
        {
            if ( !entry.is_regular_file() )
                continue;
            const std::string ext = entry.path().extension().string();
            if ( ext != ".cpp" && ext != ".hpp" && ext != ".h" )
                continue;

            const std::string relative = fs::relative( entry.path(), root ).generic_string();
            // The declaration itself lives in SceneRenderer.hpp, which is the one file allowed to
            // mention the name.
            if ( relative == "Desert/Desert/Source/Engine/Graphic/SceneRenderer.hpp" )
                continue;

            const std::string text = StripComments( Read( entry.path() ) );
            if ( text.find( "SetDebugView(" ) != std::string::npos ||
                 text.find( "SetDebugView (" ) != std::string::npos )
                callers.push_back( relative );
        }
    }

    // SceneRenderer.cpp forwards the editor's state into MeshRenderer; that is the forwarding, not a
    // source of one. Any OTHER player-side caller is a new way to turn a debug view on in a shipped
    // game, and the register above stops being true the moment one exists.
    std::vector<std::string> unexpected;
    for ( const std::string& caller : callers )
        if ( caller != "Desert/Desert/Source/Engine/Graphic/SceneRenderer.cpp" &&
             caller != "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Mesh/MeshRenderer.hpp" )
            unexpected.push_back( caller );

    EXPECT_TRUE( unexpected.empty() )
         << "a player-side file now calls SetDebugView: "
         << ( unexpected.empty() ? std::string{} : unexpected.front() )
         << ". The four developer-only pipeline rows in this register rest on nothing in a player being "
            "able to turn a debug view on. Re-decide them.";
}

// ── THE OUTLINE FAMILY IS NOT DEAD, AND THIS IS THE LINE THAT DOES IT ──────────────────────────────
//
// Common/Core/DevInstruments.hpp claimed the selection-outline family "has no caller in a player at
// all". It has one, and it is a saved scene: MeshECSSystem ORs StaticMeshComponent::OutlineDraw — a
// SERIALIZED field with a checkbox in the Materials panel — into the outline flag. Both ends of that
// chain read as correct on their own, which is the defect shape that has cost this repository the most,
// so the relation is asserted rather than described.
TEST( ShippingPipelines, OutlineFamilyIsReachableFromASavedScene )
{
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    const std::string ecs =
         StripComments( Read( root / "Desert/Desert/Source/Engine/ECS/System/MeshECSSystem.hpp" ) );
    ASSERT_FALSE( ecs.empty() ) << "MeshECSSystem.hpp could not be read";

    const size_t at = ecs.find( "outlined" );
    ASSERT_NE( at, std::string::npos )
         << "MeshECSSystem no longer computes an `outlined` flag; the outline family's reason to be in a "
            "player's build was that this flag can come from a saved scene. Re-decide the "
            "Silhouette/JFA rows in this register.";

    EXPECT_NE( ecs.find( "OutlineDraw" ), std::string::npos )
         << "MeshECSSystem no longer reads StaticMeshComponent::OutlineDraw. Without it the outline is "
            "editor selection only, and the Silhouette pipelines become developer-only.";

    // ...and the field really is serialized, which is what makes it reach a player at all.
    const std::string registry =
         StripComments( Read( root / "Desert/Desert/Source/Engine/Core/Serialize/ComponentRegistry.cpp" ) );
    ASSERT_FALSE( registry.empty() ) << "ComponentRegistry.cpp could not be read";
    EXPECT_NE( registry.find( "OutlineDraw" ), std::string::npos )
         << "OutlineDraw is no longer written to / read from a .desce, so a shipped scene cannot carry "
            "an outlined mesh and the outline pipelines would be developer-only.";
}

// The other half of "the family draws": the composite runs whether or not anything is outlined, because
// it is what hands the scene colour to tonemap. Only the propagation steps are skipped.
TEST( ShippingPipelines, TheOutlineCompositeRunsOnEveryFrame )
{
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    const std::string jfa = StripComments( Read(
         root / "Desert/Desert/Source/Engine/Graphic/Systems/Scene/PostProcessing/JumpFloodOutlineRenderer.cpp" ) );
    ASSERT_FALSE( jfa.empty() ) << "JumpFloodOutlineRenderer.cpp could not be read";

    const size_t execute = jfa.find( "void JumpFloodOutlineRenderer::Execute" );
    ASSERT_NE( execute, std::string::npos );

    const size_t active = jfa.find( "m_OutlineActive", execute );
    const size_t final  = jfa.rfind( "JFA_Final" );
    ASSERT_NE( active, std::string::npos ) << "Execute no longer consults m_OutlineActive";
    ASSERT_NE( final, std::string::npos ) << "Execute no longer runs the JFA_Final composite";
    EXPECT_GT( final, active )
         << "the JFA_Final composite is no longer the LAST thing Execute does. It used to run "
            "unconditionally, after the m_OutlineActive check that skips only the propagation steps — "
            "which is why the outline pipelines are part of the product and not a developer's tool.";
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
