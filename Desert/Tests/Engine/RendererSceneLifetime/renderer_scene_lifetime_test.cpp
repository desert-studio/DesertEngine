// WHAT A SceneRenderer KEEPS ACROSS A SCENE LOAD, AND WHAT IT MUST LET GO OF.
//
// Г11 split `SceneRenderer::Init()` — one function that did two things with two different lifetimes —
// into `EnsureRendererResources()`, which runs once per renderer, and `RebindScene()`, which runs on every
// scene. The rule that separates them is the one Desert/Tests/Engine/ConfigOwnership applies to
// configuration files, with the actor changed: there it is "would two people at the same time want
// different values", here it is "would two scenes loaded one after the other need a different one of
// these".
//
// Measured before/after INTERLEAVED across five sessions on a machine shared with other agents, minimum of
// N: the pre-split Init cost 141-232 ms per load (min 141, N = 21) and the rebind costs 0.4-0.5 ms (min
// 0.4, N = 19). The number that matters to a person is bigger than that and is the reason this suite exists
// at all: the whole load, from the command to the frame that shows it, went from 6817-9510 ms to 90-599 ms
// on Clouds_Protocol and from 1551-1941 ms to 474-494 ms on Sky_PhysicalShowcase — because destroying the
// render systems destroyed a 5.8-SECOND content-keyed cloud bake that had nothing to do with the scene
// having changed. The saving is not "we skipped some pipelines"; it is "we stopped throwing away a cache
// whose own staleness test was already correct", and it is concentrated in the scene that HAS clouds.
//
// WHY THAT MAKES A TEST NECESSARY. Before the split, a render system could hold anything at all and a
// scene load would launder it, because the system was destroyed. Now it is not, and three of the four
// pieces of state that would have leaked were LIVE DEFECTS ALREADY — they leak mid-session too, where no
// Init has ever run:
//
//   * the sky IBL was rebaked only when the sun moved or the clouds changed, so editing the atmosphere
//     itself (ground albedo, the medium, the model switch, the palette, the panorama's resolution) left
//     the world lit by the sky it used to have;
//   * the HDR cubemap had no way to be taken away — SkyboxECSSystem emitted a command only when it FOUND
//     one, so deleting the component left the old sky drawing;
//   * the particle system's per-emitter GPU cache is keyed by the raw entt entity value, which a fresh
//     registry re-issues from zero.
//
// So the invariants below are not about Г11's convenience. They are the conditions under which a render
// system may outlive the scene it was built for, and each one is asserted rather than believed.

#include <gtest/gtest.h>

#include <Engine/Graphic/SkyPayload.hpp>
#include <Engine/Graphic/SkyRules.hpp>

#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    // Walks up from the working directory looking for a file only the repository has, as
    // PureVirtualCensus and DeviceLostCensus do for the same reason: the runner's working directory is not
    // fixed. (The suites share no header; copy-paste is the convention this directory follows.)
    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            std::ifstream probe( prefix + "Desert/Desert/Source/Engine/Graphic/SceneRenderer.cpp" );
            if ( probe )
                return prefix;
            prefix += "../";
        }
        return {};
    }

    std::string ReadAll( const std::string& path )
    {
        std::ifstream      in( path, std::ios::binary );
        std::ostringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }

    // Comments become spaces; newlines survive. Without it every assertion below would be satisfied by the
    // prose in the file it is reading, and this repository's files are mostly prose.
    std::string StripComments( const std::string& src )
    {
        std::string out;
        out.reserve( src.size() );
        for ( std::size_t i = 0; i < src.size(); )
        {
            if ( src.compare( i, 2, "//" ) == 0 )
            {
                while ( i < src.size() && src[i] != '\n' )
                    out += ' ', ++i;
                continue;
            }
            if ( src.compare( i, 2, "/*" ) == 0 )
            {
                while ( i < src.size() && src.compare( i, 2, "*/" ) != 0 )
                    out += ( src[i] == '\n' ? '\n' : ' ' ), ++i;
                if ( i < src.size() )
                    out += "  ", i += 2;
                continue;
            }
            out += src[i];
            ++i;
        }
        return out;
    }

    std::string EngineSource( const std::string& relative )
    {
        const std::string text = ReadAll( RepoRoot() + "Desert/Desert/Source/Engine/" + relative );
        EXPECT_FALSE( text.empty() ) << relative << " could not be read";
        return text;
    }
} // namespace

// ===================================================================================================
// The two lifetimes, as the code states them
// ===================================================================================================

// RELATION: the renderer's own resources are built once, and the rebind is not.
//
// Stated over the source rather than by running an Init, because running one needs a device. What makes
// this worth pinning is that the guard is the whole of the saving: a `return false` that stops being
// reached puts 141 ms and a 5.8-second bake back into every scene load, and nothing else in the engine
// would notice.
TEST( RendererSceneLifetime, TheRendererHalfIsGuardedAndTheSceneHalfIsNot )
{
    const std::string source = StripComments( EngineSource( "Graphic/SceneRenderer.cpp" ) );

    const std::size_t ensure = source.find( "SceneRenderer::EnsureRendererResources()" );
    ASSERT_NE( ensure, std::string::npos ) << "EnsureRendererResources is gone; this suite pins nothing.";

    const std::size_t guard = source.find( "if ( m_RendererResourcesBuilt )", ensure );
    ASSERT_NE( guard, std::string::npos )
         << "EnsureRendererResources no longer opens with the m_RendererResourcesBuilt guard. Without it "
            "every scene load rebuilds ~18 render systems and every framebuffer against a device and a "
            "window that have not moved.";

    const std::size_t body = source.find( '{', ensure );
    ASSERT_NE( body, std::string::npos );
    EXPECT_LT( guard, body + 200u ) << "the guard is no longer the FIRST thing EnsureRendererResources does; "
                                       "work in front of it is work paid on every scene load.";

    const std::size_t rebind = source.find( "SceneRenderer::RebindScene()" );
    ASSERT_NE( rebind, std::string::npos ) << "RebindScene is gone.";
    EXPECT_EQ( source.find( "m_RendererResourcesBuilt", rebind ), std::string::npos )
         << "RebindScene consults the once-only guard. It must run on EVERY Init — it is the half that "
            "releases the previous scene's passes.";
}

// RELATION: whatever drops render systems waits for the device first.
//
// The same guarantee Desert/Tests/Engine/TeardownOrder pins for the five sites that destroy a whole
// SceneRenderer, at the one site inside it. RebindScene releases external passes (which own pipelines) and
// calls OnSceneReplaced (whose overrides release persistent storage buffers), and the last submitted frame
// may still be executing against both.
TEST( RendererSceneLifetime, RebindWaitsForTheDeviceBeforeItReleasesAnything )
{
    const std::string source = StripComments( EngineSource( "Graphic/SceneRenderer.cpp" ) );

    const std::size_t rebind = source.find( "SceneRenderer::RebindScene()" );
    ASSERT_NE( rebind, std::string::npos );
    const std::size_t end = source.find( "\n    }", rebind );
    ASSERT_NE( end, std::string::npos );
    const std::string body = source.substr( rebind, end - rebind );

    const std::size_t idle     = body.find( "WaitDeviceIdle()" );
    const std::size_t forgets  = body.find( "ForgetRenderSystem(" );
    const std::size_t replaced = body.find( "OnSceneReplaced()" );

    ASSERT_NE( idle, std::string::npos ) << "RebindScene no longer idles the device.";
    ASSERT_NE( forgets, std::string::npos ) << "RebindScene no longer drops the previous scene's passes.";
    ASSERT_NE( replaced, std::string::npos ) << "RebindScene no longer tells the systems the world changed.";

    EXPECT_LT( idle, forgets ) << "the wait happens AFTER the passes are released, which is the same as not "
                                  "waiting at all.";
    EXPECT_LT( idle, replaced ) << "OnSceneReplaced overrides free persistent GPU buffers; running them "
                                   "before the wait frees memory a submitted frame is still reading.";
}

// RELATION: ONE string decides what an external pass is called and what the rebind drops.
//
// ExternalSystemKey stamps the prefix on; RebindScene matches on it. A prefix renamed in one of the two
// leaves the rebind matching nothing while still compiling — and the symptom is the previous scene's grid
// and colliders still drawing over the new one, through passes closed over a destroyed RenderRegistry.
TEST( RendererSceneLifetime, OneConstantDecidesWhatBelongsToTheScene )
{
    const std::string source = StripComments( EngineSource( "Graphic/SceneRenderer.cpp" ) );

    ASSERT_NE( source.find( "kExternalSystemPrefix = \"External:\"" ), std::string::npos )
         << "the external-pass prefix is no longer declared as one constant.";

    // Both readers must go through the constant, and NEITHER may spell the literal again.
    std::size_t literals = 0;
    for ( std::size_t at = source.find( "\"External:\"" ); at != std::string::npos;
          at             = source.find( "\"External:\"", at + 1 ) )
        ++literals;

    EXPECT_EQ( literals, 1u ) << "\"External:\" is spelled " << literals
                              << " times in SceneRenderer.cpp. Exactly one of them may exist — the "
                                 "constant's own definition — or the two readers can drift apart.";

    const std::size_t key    = source.find( "ExternalSystemKey" );
    const std::size_t rebind = source.find( "SceneRenderer::RebindScene()" );
    ASSERT_NE( key, std::string::npos );
    ASSERT_NE( rebind, std::string::npos );

    const std::size_t end = source.find( "\n    }", rebind );
    ASSERT_NE( end, std::string::npos );
    EXPECT_NE( source.substr( rebind, end - rebind ).find( "kExternalSystemPrefix" ), std::string::npos )
         << "RebindScene no longer matches on the shared prefix constant.";
}

// ===================================================================================================
// The census: which render systems answer OnSceneReplaced, and why the rest do not
// ===================================================================================================

// A NAMED LIST, like DeviceLostCensus's and TeardownOrder's, because the default answer is "nothing" and a
// default that is never revisited is exactly how a new system's temporal history starts leaking across
// scenes silently. Every system SceneRenderer builds is in one of the two columns, and the suite fails
// when a system exists in neither — so adding one forces the question to be answered in writing.
TEST( RendererSceneLifetime, EverySystemAnswersWhetherItSurvivesASceneChange )
{
    struct System
    {
        const char* Name;      // as SceneRenderer::EnsureRendererResources registers it
        const char* Header;    // relative to Engine/
        bool        Overrides; // does it override OnSceneReplaced?
        const char* Why;       // what it holds, or why it holds nothing
    };

    // clang-format off
    // Kept one row per line: the columns are the argument, and reflowing them destroys it.
    const System systems[] = {
         { "SkyboxSystem", "Graphic/Systems/Scene/Skybox/SkyboxRenderer.hpp", false,
           "the IBL is keyed on SkyBakeFingerprint + the sun + the clouds, and the HDR cubemap is "
           "restated (or explicitly withdrawn) by SkyboxECSSystem every frame" },
         { "MeshSystem", "Graphic/Systems/Scene/Mesh/MeshRenderer.hpp", false,
           "draw queues are cleared every frame and the cascades are re-fitted every frame; the cascade "
           "framebuffers are sized from the renderer's own ShadowQuality, not from the scene" },
         { "JumpFloodSystem", "Graphic/Systems/Scene/PostProcessing/JumpFloodOutlineRenderer.hpp", false,
           "outline appearance is pushed in every frame by the editor" },
         { "TerrainSystem", "Graphic/Systems/Scene/Terrain/TerrainRenderer.hpp", false,
           "material entries are keyed by texture set and only read for terrains in this frame's queue; "
           "the draw queue is cleared every frame" },
         { "TonemapSystem", "Graphic/Systems/Scene/PostProcessing/TonemapRenderer.hpp", false,
           "every parameter is pushed from SceneSettings in BeginScene" },
         { "BackdropBlurSystem", "Graphic/Systems/Scene/PostProcessing/BackdropBlurRenderer.hpp", false,
           "a blur of THIS frame's scene colour; nothing carries" },
         { "BloomSystem", "Graphic/Systems/Scene/PostProcessing/BloomRenderer.hpp", false,
           "threshold pushed per frame, mip chain sized from the viewport" },
         { "LightShaftSystem", "Graphic/Systems/Scene/PostProcessing/LightShaftRenderer.hpp", false,
           "the SunLightFx slice is pushed per frame with the sky command" },
         { "LensFlareSystem", "Graphic/Systems/Scene/PostProcessing/LensFlareRenderer.hpp", false,
           "params pushed per frame in BeginScene" },
         { "SSAOSystem", "Graphic/Systems/Scene/Deferred/SSAORenderer.hpp", false,
           "a function of this frame's G-buffer" },
         { "SceneColorCopySystem", "Graphic/Systems/Scene/Deferred/CopyRenderer.hpp", false,
           "a copy of this frame's target" },
         { "HeightFogSystem", "Graphic/Systems/Scene/Fog/HeightFogRenderer.hpp", false,
           "SetFogSettings takes `present` and HeightFogECSSystem states the absent case explicitly" },
         { "VolumetricCloudSystem", "Graphic/Systems/Scene/Clouds/VolumetricCloudRenderer.hpp", true,
           "the temporal reconstruction's history — everything else here is already content-keyed" },
         { "ParticleSystem", "Graphic/Systems/Scene/Particles/ParticleRenderer.hpp", true,
           "per-emitter persistent SSBOs cached by the raw entt entity value, which a fresh registry "
           "re-issues from zero" },
         { "DeferredLightingSystem", "Graphic/Systems/Scene/Deferred/DeferredLightingRenderer.hpp", false,
           "a shade of this frame's G-buffer" },
         // The two LAZY ones. They are registered by EnsureGIResources / EnsureSSRResources on first use
         // rather than up front (a preview never enables either, and eager allocation multiplied six
         // full-screen RGBA32F targets by the preview count), which is why they are here and not in the
         // order the rest appear in. See the test below for what that laziness cost before Г11.
         { "GISystem", "Graphic/Systems/Scene/Deferred/GIResolveRenderer.hpp", true,
           "the temporally accumulated indirect light, blended at 0.92 against a reprojection of the "
           "previous frame" },
         { "SSRSystem", "Graphic/Systems/Scene/Deferred/SSRRenderer.hpp", true,
           "the temporally accumulated reflection, blended at 0.88 against a reprojection of the "
           "previous frame" },
         { "AutoExposureSystem", "Graphic/Systems/Scene/PostProcessing/AutoExposureRenderer.hpp", true,
           "the adapted luminance is a temporal history and would ramp out of the previous level's "
           "brightness" },
         { "FXAASystem", "Graphic/Systems/Scene/PostProcessing/FXAARenderer.hpp", false,
           "a filter of this frame's tonemapped image" },
         { "SMAASystem", "Graphic/Systems/Scene/PostProcessing/SMAARenderer.hpp", false,
           "a filter of this frame's tonemapped image" },
    };
    // clang-format on

    const std::string init = StripComments( EngineSource( "Graphic/SceneRenderer.cpp" ) );

    // 1. The census covers exactly the systems the renderer builds — no more, and above all no fewer. The
    //    names are taken from the RegisterSystem calls themselves, so a system added tomorrow reddens this
    //    before anybody remembers the file exists.
    std::set<std::string> registered;
    for ( std::size_t at = init.find( "RegisterSystem<" ); at != std::string::npos;
          at             = init.find( "RegisterSystem<", at + 1 ) )
    {
        const std::size_t open  = init.find( '"', at );
        const std::size_t close = open == std::string::npos ? std::string::npos : init.find( '"', open + 1 );
        if ( open == std::string::npos || close == std::string::npos )
            continue;
        registered.insert( init.substr( open + 1, close - open - 1 ) );
    }

    ASSERT_FALSE( registered.empty() ) << "no RegisterSystem<...>( \"Name\" ) calls found — the scan, not "
                                          "the engine, is what is wrong.";

    std::set<std::string> censused;
    for ( const System& s : systems )
        censused.insert( s.Name );

    for ( const std::string& name : registered )
        EXPECT_EQ( censused.count( name ), 1u )
             << "SceneRenderer builds '" << name
             << "' and this census does not name it. Answer the question in a row: does it hold a temporal "
                "history or a per-entity GPU cache that would survive into the next scene? See "
                "IRenderSystem::OnSceneReplaced for the two kinds that can.";

    for ( const std::string& name : censused )
        EXPECT_EQ( registered.count( name ), 1u )
             << "this census names '" << name
             << "', which SceneRenderer no longer builds. A row about a system that does not exist passes "
                "without checking anything.";

    // 2. Each row's claim matches the header. An override that appears without its row being updated is
    //    the same defect as a row without an override: the two must agree, and neither is the authority.
    for ( const System& s : systems )
    {
        const std::string header   = StripComments( EngineSource( s.Header ) );
        const bool        declared = header.find( "OnSceneReplaced()" ) != std::string::npos;

        if ( s.Overrides )
            EXPECT_TRUE( declared ) << s.Name << " is censused as holding scene-derived state (" << s.Why
                                    << ") but " << s.Header << " does not override OnSceneReplaced.";
        else
            EXPECT_FALSE( declared ) << s.Name
                                     << " overrides OnSceneReplaced but this census says it holds nothing ("
                                     << s.Why
                                     << "). One of the two is wrong, and a stale census is worse than none.";
    }
}

// RELATION: a "have I built this?" latch and the container it says something about must be cleared
// together, or not at all.
//
// FOUND BY THE CENSUS ABOVE, not by looking for it. `m_GIResourcesReady` and `m_SSRResourcesReady` latch
// true the first time their feature is switched on and are never cleared — while the pre-Г11 Init emptied
// m_RenderSystems on every scene load. So after ONE scene load in `dev`, EnsureGIResources answered "yes,
// already built", the GISystem entry was gone from the map, and the null-guarded call site at the RSM
// block simply skipped the pass: RSM global illumination and screen-space reflections stopped working, in
// silence, for the rest of the session. Two flags and one map, with two different lifetimes, under one
// function — the same shape this whole task is about, one level down.
//
// Г11 fixes it by removing the clear rather than by adding a reset, which is the right direction: there
// was never a reason to drop these. What this test pins is that nothing puts the clear back.
TEST( RendererSceneLifetime, TheLazyResourceLatchesAndTheSystemMapAgree )
{
    const std::string source = StripComments( EngineSource( "Graphic/SceneRenderer.cpp" ) );

    // Nothing anywhere in the renderer may empty the system map. Registering, replacing and forgetting one
    // by name all remain legal; emptying it is what breaks the agreement with the latches.
    EXPECT_EQ( source.find( "m_RenderSystems.clear()" ), std::string::npos )
         << "something clears m_RenderSystems wholesale again. m_GIResourcesReady / m_SSRResourcesReady are "
            "never cleared, so the GI and SSR systems would be reported as built and be absent from the "
            "map — the passes then skip in silence at their null-guarded call sites.";

    // ...and the latches themselves stay write-once, so the other direction of the fix is not reopened by
    // clearing THEM instead (which would rebuild six full-screen RGBA32F targets per scene load).
    for ( const char* latch : { "m_GIResourcesReady", "m_SSRResourcesReady" } )
    {
        const std::string assignFalse = std::string( latch ) + " = false";
        EXPECT_EQ( source.find( assignFalse ), std::string::npos )
             << latch
             << " is cleared somewhere. These targets are sized from the VIEWPORT and configured from the "
                "device's capabilities — neither changes because a different scene was loaded.";
    }
}

// ===================================================================================================
// The sky's own parameters as a rebake key
// ===================================================================================================

// RELATION: the environment on the device was baked from these inputs; rebake iff the inputs differ.
//
// The trigger had two keys — the sun, by angle, and the cloud field, by fingerprint — and the sky itself
// was not among them. This walks the packed block one FLOAT at a time and asserts that changing any of
// them changes the fingerprint, EXCEPT the three that are the sun's direction. It is written as a walk
// rather than as a list of named fields on purpose: a vec4 appended to SkyGpuPayload is covered here the
// moment PackSky writes it, which is the property a hand-typed field list cannot have — and losing that
// property is exactly how AtmosphereLutFingerprint (which is a typed list, deliberately, because it
// covers only the LUT passes' share of the block) and this trigger came to disagree.
TEST( RendererSceneLifetime, TheSkyFingerprintCoversEveryPackedFloatExceptTheSunDirection )
{
    using namespace Desert::Graphic;

    SkySettings sky;
    // Non-round values so no field's mutation can land on another field's default and hide.
    const glm::vec3 towardSun = glm::normalize( glm::vec3( 0.31f, 0.72f, -0.62f ) );

    const SkyGpuPayload base            = PackSky( towardSun, sky );
    const uint32_t      panorama        = 1;
    const uint64_t      baseFingerprint = SkyBakeFingerprint( base, panorama );

    constexpr std::size_t kFloats = sizeof( SkyGpuPayload ) / sizeof( float );
    static_assert( kFloats == kSkyPackedVec4Count * 4, "the block is not whole vec4s any more" );

    std::size_t covered = 0;
    for ( std::size_t i = 0; i < kFloats; ++i )
    {
        SkyGpuPayload mutated = base;
        // SCALED, not offset. The planet radius is 6.36e8 world units and `+= 0.37f` is simply lost in
        // float there — the first version of this test reported vec4 12 component 3 as uncovered when the
        // fingerprint was fine and the MUTATION was the thing that never happened. A relative change moves
        // every field of the block, whatever its magnitude, and still moves a field that is zero.
        float& field = reinterpret_cast<float*>( &mutated )[i];
        field        = field * 1.5f + 0.37f;

        const uint64_t moved = SkyBakeFingerprint( mutated, panorama );

        if ( i < kSkyBakeFingerprintSkippedFloats )
        {
            EXPECT_EQ( moved, baseFingerprint )
                 << "float " << i
                 << " is the sun's DIRECTION and must not be in this fingerprint. It moves every frame "
                    "under the time-of-day driver, so an exact comparison over it would demand a rebake "
                    "per frame — which is what RebakeSunAngleThreshold exists to prevent.";
        }
        else
        {
            EXPECT_NE( moved, baseFingerprint )
                 << "float " << i << " of SkyGpuPayload (vec4 " << ( i / 4 ) << ", component " << ( i % 4 )
                 << ") changes what the IBL bake reads and does not change the fingerprint. The world "
                    "would keep the ambient light of the sky it used to have.";
            ++covered;
        }
    }

    EXPECT_EQ( covered, kFloats - kSkyBakeFingerprintSkippedFloats );

    // The panorama's extent is the bake input that is NOT in the block. Without it, changing Environment
    // Resolution changed nothing until the sun happened to move.
    EXPECT_NE( SkyBakeFingerprint( base, panorama + 1 ), baseFingerprint )
         << "the panorama size is not part of the fingerprint.";

    // And the sun's INTENSITY is not its direction: it is the `w` of vec4 0, has no threshold of its own,
    // and must be covered. Asserted by name as well as by the walk, because it is the one component of
    // that vec4 a future reader is most likely to exclude along with its neighbours.
    SkySettings brighter = sky;
    brighter.SunIntensity += 1.0f;
    EXPECT_NE( SkyBakeFingerprint( PackSky( towardSun, brighter ), panorama ), baseFingerprint )
         << "the sun's intensity shares vec4 0 with its direction and was excluded with it.";
}

// RELATION: a sky whose own parameters differ is a stale environment, whatever the sun is doing.
//
// This is the case a scene LOAD produces — a different level arriving with its sun in much the same place —
// and it is also what an artist dragging Ground Albedo does.
TEST( RendererSceneLifetime, TheSkyFingerprintAloneForcesARebake )
{
    using namespace Desert::Graphic;

    const glm::vec3 sun( 0.0f, 1.0f, 0.0f );

    // Everything else agrees: same sun, same clouds, an environment already on the device, auto-rebake on,
    // no explicit request. Only the sky's own fingerprint differs.
    EXPECT_TRUE( ShouldRebakeSkyEnvironment( sun, sun, /*thresholdDeg=*/5.0f, /*autoRebake=*/true,
                                             /*hasEnvironment=*/true, /*explicitRequest=*/false,
                                             /*bakedCloudFingerprint=*/7u, /*currentCloudFingerprint=*/7u,
                                             /*bakedSkyFingerprint=*/1234u, /*currentSkyFingerprint=*/5678u ) )
         << "a different sky under an unmoved sun is not treated as stale — which is a level lit by the "
            "previous level's atmosphere.";

    // ...and with all three keys equal, nothing happens. The other half of the relation: a key that always
    // fires is not a key, it is a per-frame device idle.
    EXPECT_FALSE( ShouldRebakeSkyEnvironment( sun, sun, 5.0f, true, true, false, 7u, 7u, 1234u, 1234u ) )
         << "an unchanged sky rebakes anyway. The bake idles the whole device and rebuilds four cube "
            "images; doing it per frame is worse than the defect it would be fixing.";

    // The precedence the existing rule already had is not disturbed: the explicit request and the very
    // first bake still win over everything, and AutoRebake off still silences the fingerprint.
    EXPECT_TRUE( ShouldRebakeSkyEnvironment( sun, sun, 5.0f, false, true, true, 7u, 7u, 1234u, 1234u ) )
         << "the Bake button stopped working.";
    EXPECT_TRUE( ShouldRebakeSkyEnvironment( sun, sun, 5.0f, false, false, false, 7u, 7u, 1234u, 1234u ) )
         << "the FIRST bake stopped happening, which is a scene with no ambient light at all.";
    EXPECT_FALSE( ShouldRebakeSkyEnvironment( sun, sun, 5.0f, false, true, false, 7u, 7u, 1234u, 5678u ) )
         << "Auto Rebake off no longer stops the sky fingerprint from forcing a bake. 'Off' is a request "
            "to stop re-baking, and this key must obey it exactly as the other two do.";
}

// ===================================================================================================
// The producer's half: absence has to be sayable
// ===================================================================================================

// RELATION: a renderer that keeps its state across frames needs its producers to state ABSENCE, not to
// fall silent. The sky, the fog and the cloud layer all did; the HDR cubemap did not, which is why
// deleting a SkyboxComponent left the old sky drawing — a defect that predates Г11 and that survives a
// scene load only because Г11 stopped destroying the system that held it.
TEST( RendererSceneLifetime, TheSkyboxProducerStatesAbsenceInsteadOfFallingSilent )
{
    const std::string producer = StripComments( EngineSource( "ECS/System/SkyboxECSSystem.hpp" ) );

    std::size_t emits = 0;
    for ( std::size_t at = producer.find( "Emplace<Graphic::Render::SkyboxCommand>" ); at != std::string::npos;
          at             = producer.find( "Emplace<Graphic::Render::SkyboxCommand>", at + 1 ) )
        ++emits;

    EXPECT_EQ( emits, 1u )
         << "SkyboxECSSystem emits the skybox command from " << emits
         << " places. Exactly one unconditional emit per frame is what makes 'this scene has no HDR "
            "skybox' sayable; an emit inside the branch that FOUND one cannot say it.";

    // The unconditional emit must be outside the search loop — i.e. after the closing brace of the mode
    // check — and must be able to carry nothing.
    const std::size_t search = producer.find( "registry.view<ECS::SkyboxComponent>()" );
    const std::size_t emit   = producer.find( "Emplace<Graphic::Render::SkyboxCommand>" );
    ASSERT_NE( search, std::string::npos );
    ASSERT_NE( emit, std::string::npos );
    EXPECT_LT( search, emit ) << "the emit is inside the component search again; a frame that finds no "
                                 "cubemap must still say so.";

    // ...and the consumer must treat "nothing" as an instruction rather than as a missing argument.
    const std::string consumer =
         StripComments( EngineSource( "Graphic/Systems/Scene/Skybox/SkyboxRenderer.cpp" ) );
    const std::size_t prepare = consumer.find( "SkyboxRenderer::PrepareMaterial(" );
    ASSERT_NE( prepare, std::string::npos );
    const std::size_t end = consumer.find( "\n    }", prepare );
    ASSERT_NE( end, std::string::npos );

    EXPECT_EQ( consumer.substr( prepare, end - prepare ).find( "return" ), std::string::npos )
         << "PrepareMaterial returns early again. A null material is the producer SAYING there is no "
            "cubemap; returning without recording it is a silent no-op, and the previous scene's sky "
            "keeps drawing.";
}

// Only gtest is linked, not gtest_main — every suite in this tree brings its own entry point.
// RT1h: A VIEW IS BUILT AT ITS SURFACE'S SIZE, NOT THE WINDOW'S. The build used to read the window, so a
// 512 px thumbnail and a viewport panel not yet laid out were first built at 4112x2578 (2.1 GiB) and shrank
// a frame later. The build must read the extent the constructor was given, and Resize must record the new
// extent before its "not built yet" exit, or a size learned before Init would be built at the old one.
TEST( RendererSceneLifetime, TheBuildReadsTheViewExtentAndNeverTheWindow )
{
    const std::string source = StripComments( EngineSource( "Graphic/SceneRenderer.cpp" ) );

    const std::size_t ensure = source.find( "SceneRenderer::EnsureRendererResources()" );
    ASSERT_NE( ensure, std::string::npos );
    const std::size_t ensureEnd = source.find( "\n    }\n", ensure );
    ASSERT_NE( ensureEnd, std::string::npos );
    const std::string build = source.substr( ensure, ensureEnd - ensure );
    EXPECT_EQ( build.find( "GetWindow" ), std::string::npos ) << "EnsureRendererResources reads the window size.";
    EXPECT_NE( build.find( "m_ViewExtent.Width" ), std::string::npos );
    EXPECT_NE( build.find( "m_ViewExtent.Height" ), std::string::npos );

    const std::size_t resize = source.find( "SceneRenderer::Resize(" );
    ASSERT_NE( resize, std::string::npos );
    const std::size_t record  = source.find( "m_ViewExtent = ViewExtent{ width, height }", resize );
    const std::size_t unbuilt = source.find( "if ( !m_TargetFramebuffer )", resize );
    ASSERT_NE( record, std::string::npos ) << "Resize no longer records the extent the next build reads.";
    ASSERT_NE( unbuilt, std::string::npos );
    EXPECT_LT( record, unbuilt ) << "Resize leaves before recording the extent: a size learned before the "
                                    "build would be lost.";
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
