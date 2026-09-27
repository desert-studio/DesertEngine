// THE ENVIRONMENT A SURFACE IS SHADED BY BELONGS TO THE SCENE THAT SURFACE IS IN.
//
// That is the relation, and it is the one Г14 found broken. `CornellDemo -> Clouds_Protocol ->
// CornellDemo` came back from the round trip 100 % different: mean luminance 0.477 -> 0.794, green-wall
// saturation 0.626 -> 0.313, on a scene that has no sky component of any kind. Reloading CornellDemo
// three times in a row was byte-identical, and so was a round trip through a scene that also has no
// environment (M10_MeshSlot: 0 of 761 318 bytes). Only a visit to a scene that BAKES one did it.
//
// WHY A UNIT TEST CAN SEE IT AT ALL. Nothing was leaking and nothing was freed: the producer restates
// "this scene has no sky" every single frame (SkyboxECSSystem emits a SkyboxCommand carrying nothing;
// SkyboxRenderer::GetEnvironment answers nullopt; both shading paths therefore receive null cube
// pointers), and the descriptor was always defined. The sentence died at ONE link in between —
// `TextureCubeProperty::Apply` wrote the descriptor only `if ( m_Texture )` and then marked itself clean
// regardless. A slot told "nothing" wrote nothing, so it kept the last cube ANY scene had given it. Both
// ends of the chain were right; the middle dropped the property. That link is pure C++ and needs no GPU.
//
// WHAT IS NOT COVERED HERE, said plainly: turning that null into a DEFINED descriptor is the Vulkan
// uniform's job (VulkanUniformImageCube::SetImageCube points at the same fallback cube
// VulkanMaterialBackend seeds every declared cube binding with), and writing a descriptor needs a device
// this machine cannot give a test. What is asserted below is the property this file's name claims: the
// slot's state after a scene is shown depends on THAT scene and on nothing shown before it.

#include <gtest/gtest.h>

#include <Engine/Core/EngineContext.hpp>
#include <Engine/Core/FrameManager.hpp>
#include <Engine/Graphic/Materials/Properties/TextureCubeProperty.hpp>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using Desert::EngineContext;
using Desert::Engine::FrameManager;
using Desert::Graphic::ImageCube;
using Desert::Graphic::MaterialBackend;
using Desert::Graphic::MaterialProperty;
using Desert::Graphic::TextureCubeProperty;

namespace
{
    // THE TWO CUBES ARE NEVER DEREFERENCED, and that is what lets this run without a device. A cube
    // reaches the slot as an opaque pointer and leaves it as an opaque pointer; the identity is the whole
    // of what the slot is responsible for.
    const ImageCube* const kCloudScenesSky = reinterpret_cast<const ImageCube*>( 0x1000 );
    const ImageCube* const kAnotherSky     = reinterpret_cast<const ImageCube*>( 0x2000 );

    // What a scene STATES about its environment. `nullptr` is a value here, not a missing argument:
    // CornellDemo states it has no sky, every frame, deliberately.
    struct Scene
    {
        const char*      Name;
        const ImageCube* Sky;
    };

    /// Records what the slot was told, exactly as VulkanUniformImageCube would be told it.
    class RecordingUniformCube final : public Desert::ShaderResources::UniformImageCube
    {
    public:
        uint32_t GetBinding() const override
        {
            return 17;
        }

        void SetImageCube( const ImageCube* imageCube ) override
        {
            Current = imageCube;
            ++Writes;
        }

        // GetImageHash() was overridden here until Г12 deleted the pure virtual: nothing asked a uniform
        // for its image's hash, the descriptor cache keys off Image::GetHash() on the image itself. The
        // fake had to answer it only because the base declared it.

        // Deliberately NOT nullptr to begin with: a test whose "cleared" answer is also its initial
        // answer proves nothing about clearing.
        const ImageCube* Current = kAnotherSky;
        int              Writes  = 0;
    };

    class RecordingBackend final : public MaterialBackend
    {
    public:
        RecordingBackend() : MaterialBackend( nullptr )
        {
        }

        void ApplyUniformBuffer( MaterialProperty* ) override
        {
        }
        void ApplyStorageBuffer( MaterialProperty* ) override
        {
        }
        void ApplyTexture2D( MaterialProperty* ) override
        {
        }
        void ApplyTextureCube( MaterialProperty* ) override
        {
            ++CubeDescriptorWrites;
        }
        void FlushUpdates() override
        {
        }
        // ApplyPushConstants was overridden here with an empty body until Г12 deleted the pure virtual.
        // That is the argument for deleting it, stated by this file: the ONE production implementation
        // was empty too, so the surface existed only to be satisfied — by the backend and by this fake.

        int CubeDescriptorWrites = 0;
    };

    // The singletons the dirty mechanism reads. Created once for the process; both are plain header-only
    // value holders, so this costs nothing and needs no device.
    void EnsureEngineSingletons()
    {
        FrameManager::CreateInstance().Initialize( 2 );
        EngineContext::CreateInstance();
    }

    /// Draws @p scene for @p frames frames, the way SceneRenderer does: the producer restates the scene's
    /// environment every frame — absence included — and the frame counter advances.
    void Present( const Scene& scene, TextureCubeProperty& slot, RecordingBackend& backend, int frames )
    {
        for ( int frame = 0; frame < frames; ++frame )
        {
            slot.SetTexture( scene.Sky );
            slot.Apply( &backend );
            FrameManager::GetInstance().NextFrame();
        }
    }

    // Long enough to outlast the dirty window (frames in flight x open views), so a failure cannot be
    // "it just had not caught up yet".
    constexpr int kFramesPerVisit = 40;
} // namespace

// A -> B -> A. The protocol the defect was measured with, at the link that lost it.
TEST( EnvironmentViewMemory, ReturningToASceneRestoresThatScenesEnvironment )
{
    EnsureEngineSingletons();

    const Scene sceneWithoutSky{ "CornellDemo", nullptr };
    const Scene sceneWithSky{ "Clouds_Protocol", kCloudScenesSky };

    const auto          uniform = std::make_shared<RecordingUniformCube>();
    TextureCubeProperty slot{ uniform };
    RecordingBackend    backend;

    Present( sceneWithoutSky, slot, backend, kFramesPerVisit );
    const ImageCube* firstVisit = uniform->Current;

    Present( sceneWithSky, slot, backend, kFramesPerVisit );
    ASSERT_EQ( uniform->Current, kCloudScenesSky ) << "the scene that HAS an environment must get it";

    Present( sceneWithoutSky, slot, backend, kFramesPerVisit );

    EXPECT_EQ( uniform->Current, firstVisit )
         << "A scene shown after another scene is shaded by the other scene's environment. The slot kept "
            "what it was last given because a null was treated as 'nothing to do' rather than as a value. "
            "Measured in the editor before this was fixed: CornellDemo -> Clouds_Protocol -> CornellDemo "
            "changed 761318 of 761318 pixels, mean 0.477 -> 0.794.";
}

// The same statement without the "A -> B -> A" shape: the slot is a function of the CURRENT scene alone.
// Stronger than the sequence above, because it forbids the defect in every interleaving rather than in
// the one that was noticed.
TEST( EnvironmentViewMemory, TheSlotIsAFunctionOfTheCurrentSceneAlone )
{
    EnsureEngineSingletons();

    const std::vector<Scene> tour = {
         { "CornellDemo", nullptr },
         { "Clouds_Protocol", kCloudScenesSky },
         { "CornellDemo", nullptr },
         { "Sky_Other", kAnotherSky },
         { "Clouds_Protocol", kCloudScenesSky },
         { "CornellDemo", nullptr },
         { "Sky_Other", kAnotherSky },
         { "CornellDemo", nullptr },
    };

    const auto          uniform = std::make_shared<RecordingUniformCube>();
    TextureCubeProperty slot{ uniform };
    RecordingBackend    backend;

    for ( const Scene& scene : tour )
    {
        Present( scene, slot, backend, kFramesPerVisit );
        EXPECT_EQ( uniform->Current, scene.Sky )
             << "after showing '" << scene.Name << "' the environment slot holds something else";
    }
}

// §1.4: an empty answer must be DISTINGUISHABLE from no answer. A cleared slot has to reach the backend,
// or the descriptor is never rewritten and the clearing is a no-op with a clean conscience.
TEST( EnvironmentViewMemory, ClearingASlotIssuesADescriptorWrite )
{
    EnsureEngineSingletons();

    const auto          uniform = std::make_shared<RecordingUniformCube>();
    TextureCubeProperty slot{ uniform };
    RecordingBackend    backend;

    Present( Scene{ "Clouds_Protocol", kCloudScenesSky }, slot, backend, kFramesPerVisit );
    const int afterSky = backend.CubeDescriptorWrites;
    ASSERT_GT( afterSky, 0 );

    Present( Scene{ "CornellDemo", nullptr }, slot, backend, kFramesPerVisit );

    EXPECT_GT( backend.CubeDescriptorWrites, afterSky )
         << "a slot that was told 'no environment' asked the backend for nothing, so the descriptor still "
            "points at the previous scene's cube";
}

// ------------------------------------------------------------------------------------------------
// The two CALL SITES, which the seam above cannot reach: an applier that hides absence behind an `if`
// never gives the slot the chance to clear itself. Read as text for the same reason
// Desert/Tests/Engine/RendererSceneLifetime does — there is no device here to run them on.
// ------------------------------------------------------------------------------------------------
namespace
{
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

    // Comments become spaces; newlines survive. Without it every assertion below would be satisfied by
    // the prose in the file it is reading, and the prose in these two files is mostly about this defect.
    std::string StripComments( const std::string& src )
    {
        std::string out;
        out.reserve( src.size() );
        for ( std::size_t i = 0; i < src.size(); )
        {
            if ( src.compare( i, 2, "//" ) == 0 )
            {
                while ( i < src.size() && src[i] != '\n' )
                    ++i;
            }
            else if ( src.compare( i, 2, "/*" ) == 0 )
            {
                i += 2;
                while ( i + 1 < src.size() && src.compare( i, 2, "*/" ) != 0 )
                    out += ( src[i++] == '\n' ) ? '\n' : ' ';
                i += 2;
            }
            else
            {
                out += src[i++];
            }
        }
        return out;
    }

    /// The body of @p function in @p source, comments already stripped: from its name to the matching
    /// closing brace. Empty when the function is not there, which the callers assert on.
    std::string BodyOf( const std::string& source, const std::string& function )
    {
        const std::size_t at = source.find( function );
        if ( at == std::string::npos )
            return {};
        const std::size_t open = source.find( '{', at );
        if ( open == std::string::npos )
            return {};

        int depth = 0;
        for ( std::size_t i = open; i < source.size(); ++i )
        {
            if ( source[i] == '{' )
                ++depth;
            else if ( source[i] == '}' && --depth == 0 )
                return source.substr( open, i - open + 1 );
        }
        return {};
    }
} // namespace

TEST( EnvironmentViewMemory, TheForwardApplierStatesAbsence )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    const std::string body =
         BodyOf( StripComments(
                      ReadAll( root + "Desert/Desert/Source/Engine/Graphic/Materials/SceneLightingBinding.hpp" ) ),
                 "inline void SceneEnvironmentBind" );
    ASSERT_FALSE( body.empty() ) << "SceneEnvironmentBind is not where this suite expects it";

    EXPECT_EQ( body.find( "if ( irradiance )" ), std::string::npos )
         << "SceneEnvironmentBind binds the irradiance cube only when it exists, so a scene with no sky "
            "leaves the previous scene's cube in the descriptor";
    EXPECT_EQ( body.find( "if ( prefiltered )" ), std::string::npos ) << "same for the prefiltered cube";
    EXPECT_NE( body.find( "SetTexture( irradiance )" ), std::string::npos );
    EXPECT_NE( body.find( "SetTexture( prefiltered )" ), std::string::npos );
}

TEST( EnvironmentViewMemory, TheDeferredCompositeStatesAbsence )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    const std::string source = StripComments(
         ReadAll( root + "Desert/Desert/Source/Engine/Graphic/Materials/Deferred/MaterialDeferredLighting.hpp" ) );
    const std::string body = BodyOf( source, "void BindInputs" );
    ASSERT_FALSE( body.empty() ) << "MaterialDeferredLighting::BindInputs is not where this suite expects it";

    EXPECT_EQ( body.find( "IsComplete()" ), std::string::npos )
         << "the deferred composite binds the environment only when the whole set is present. That gate "
            "reads as caution and is how one scene's sky survives into the next: a slot that is not "
            "written keeps what it had. Completeness is reported by "
            "DeferredLightingRenderer::ReportEnvironmentGap; it must not decide whether to write.";
    EXPECT_NE( body.find( "SetTexture( environment.Irradiance )" ), std::string::npos );
    EXPECT_NE( body.find( "SetTexture( environment.Prefiltered )" ), std::string::npos );
}

// The slot property itself, read as text: the guard that was removed must not come back wearing a
// different spelling. The behavioural tests above would catch it, and this says WHY in the place a
// reader of the diff will be standing.
TEST( EnvironmentViewMemory, TheCubeSlotWritesEvenWhenItHasNothing )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    const std::string body = BodyOf(
         StripComments( ReadAll(
              root + "Desert/Desert/Source/Engine/Graphic/Materials/Properties/TextureCubeProperty.hpp" ) ),
         "void Apply(" );
    ASSERT_FALSE( body.empty() );

    EXPECT_EQ( body.find( "if ( m_Texture )" ), std::string::npos )
         << "TextureCubeProperty::Apply is guarding its descriptor write on having a texture again";
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
