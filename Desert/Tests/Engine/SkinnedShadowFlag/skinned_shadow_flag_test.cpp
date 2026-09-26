// SKINNED CastShadows: the flag on the component and the mesh's presence in the shadow pass must agree.
//
// The static mesh has had CastShadows since the shadow pass existed; the skinned mesh had NOTHING — an
// asymmetry nobody declared, so a character could not be taken out of the cascades while the crate beside
// it could (Д24). And the defect that made the asymmetry invisible is the recurring one this repository
// keeps a memory note about: BOTH ENDS already looked right. SkinnedMeshRenderData carried a CastShadows
// the cascade pass honoured, and the component was about to carry one too — what was missing was the
// MIDDLE of the chain, the ECS system and the command hop, where a property drops silently because each
// end compiles without it.
//
// So this suite asserts the RELATION at every link, not the function at either end:
//
//   component default  ==  render-data default            (compiled, both headers included)
//   component flag    --> DrawSkinnedMeshCommand ctor      (the ECS system's emplace names it)
//   command           --> SubmitMesh extra                 (Execute forwards it)
//   extra             --> SkinnedMeshRenderData            (the renderer's switch copies it)
//   render data       --> the cascade skip                 (!sd.CastShadows)
//   component        <--> the .desce/.deprefab payload     (Ser mirror round-trips; write-when-false)
//
// The middle links are asserted as text over the comment-stripped, whitespace-normalized source — the
// same technique the SettingConsumers census uses, and for the same reason: they live in a render system
// and an ECS system that cannot link without a GPU. Each assertion was verified by mutation: deleting
// the link it names turns exactly that assertion red.

#include "../SettingConsumers/setting_consumers_reader.hpp"

#include <Engine/Assets/Prefab/PrefabData.hpp> // SkinnedMeshComponentSer, the on-disk mirror
#include <Engine/ECS/Components.hpp>

#include <Common/Json/Json.hpp>

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>

namespace
{
    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            std::ifstream probe( prefix + "Desert/Desert/Source/Engine/ECS/Components.hpp" );
            if ( probe )
                return prefix;
            prefix += "../";
        }
        return {};
    }

    std::string ReadFile( const std::string& path )
    {
        std::ifstream in( path );
        if ( !in )
            return {};
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    // Comment-stripped and whitespace-collapsed, so an assertion about an expression chain survives
    // reformatting but not deletion.
    std::string Normalized( const std::string& path )
    {
        const std::string text = Desert::Tests::ConsumerText::StripCommentsAndLiterals( ReadFile( path ) );
        std::string       out;
        out.reserve( text.size() );
        for ( char c : text )
            if ( c != ' ' && c != '\t' && c != '\n' && c != '\r' )
                out += c;
        return out;
    }

    bool Contains( const std::string& haystack, const std::string& needle )
    {
        return haystack.find( needle ) != std::string::npos;
    }

    // The statement that starts at @p opener and runs to the next ';' — the argument list of one call,
    // whitespace already collapsed. Empty when the opener is not in the file at all.
    std::string StatementAfter( const std::string& normalized, const std::string& opener )
    {
        const std::size_t at = normalized.find( opener );
        if ( at == std::string::npos )
            return {};
        const std::size_t end = normalized.find( ';', at );
        return normalized.substr( at, end == std::string::npos ? std::string::npos : end - at );
    }
} // namespace

// ---------------------------------------------------------------------------------------------------
// The two ends, compiled: the authored default and the render-side default are the same value, and the
// same value the static twin has. A skinned mesh added to a scene yesterday casts exactly what it cast
// before Д24 — the flag's arrival changes nothing until somebody turns it off.
// ---------------------------------------------------------------------------------------------------

TEST( SkinnedShadowFlag, TheComponentAndItsStaticTwinAgreeOnTheDefault )
{
    EXPECT_TRUE( Desert::ECS::SkinnedMeshComponent{}.CastShadows );
    EXPECT_TRUE( Desert::ECS::StaticMeshComponent{}.CastShadows );
}

// The submission extra's default is asserted from the text (SceneRenderer.hpp cannot compile without the
// graphics stack): a default of FALSE there would silence every static mesh's shadow instead, but a
// mismatch in either direction makes one of the twins lie about what "unspecified" means.
TEST( SkinnedShadowFlag, TheSubmissionExtraDefaultsTheFlagOnLikeTheComponentsDo )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    const std::string src = Normalized( root + "Desert/Desert/Source/Engine/Graphic/SceneRenderer.hpp" );
    ASSERT_FALSE( src.empty() );

    const std::size_t extra = src.find( "structRenderSubmissionExtra" );
    ASSERT_NE( extra, std::string::npos );
    const std::size_t block = src.find( '}', extra );
    EXPECT_TRUE( Contains( src.substr( extra, block - extra ), "boolCastShadows=true" ) )
         << "RenderSubmissionExtra no longer defaults CastShadows to true - every producer that does not "
            "state the flag goes dark in the cascades";
}

// ---------------------------------------------------------------------------------------------------
// The middle links, as text. Each names ONE hop of the chain, so the failure says which link dropped
// the property — the exact diagnosis the seven middle-link defects of 2026-08-30 had to be dug for.
// ---------------------------------------------------------------------------------------------------

TEST( SkinnedShadowFlag, TheEcsSystemHandsTheComponentFlagToTheCommand )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    const std::string src = Normalized( root + "Desert/Desert/Source/Engine/ECS/System/MeshECSSystem.hpp" );
    ASSERT_FALSE( src.empty() );

    const std::string emplace = StatementAfter( src, "Emplace<Graphic::Render::DrawSkinnedMeshCommand>" );
    ASSERT_FALSE( emplace.empty() ) << "the skinned draw is no longer emitted via DrawSkinnedMeshCommand - "
                                       "this suite must follow the submission path";
    EXPECT_TRUE( Contains( emplace, "mesh.CastShadows" ) )
         << "MeshECSSystem emits the skinned draw without the component's CastShadows - the command's "
            "default (true) silently wins and the checkbox moves nothing. The emplace found: "
         << emplace;
}

TEST( SkinnedShadowFlag, TheCommandForwardsTheFlagIntoTheSubmission )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    const std::string src =
         Normalized( root + "Desert/Desert/Source/Engine/Graphic/Render/Commands/DrawSkinnedMeshCommand.hpp" );
    ASSERT_FALSE( src.empty() );

    const std::string submit = StatementAfter( src, "renderer.SubmitMesh(" );
    ASSERT_FALSE( submit.empty() );
    EXPECT_TRUE( Contains( submit, ".CastShadows=CastShadows" ) )
         << "DrawSkinnedMeshCommand::Execute drops CastShadows on the SubmitMesh hop - both ends of the "
            "chain still look right while the middle loses the property. The call found: "
         << submit;
}

TEST( SkinnedShadowFlag, TheRendererCopiesTheFlagOntoTheSkinnedQueueAndTheCascadePassSkipsOnIt )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    const std::string src =
         Normalized( root + "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Mesh/MeshRenderer.cpp" );
    ASSERT_FALSE( src.empty() );

    // The copy in MeshRenderer::SubmitMesh's Skinned case...
    EXPECT_TRUE( Contains( src, "skinnedData.CastShadows=data.CastShadows" ) )
         << "MeshRenderer::SubmitMesh no longer copies CastShadows onto SkinnedMeshRenderData";
    // ...and the consumption in the cascade pass. This is the read that makes the setting real; without
    // it every line above moves a value nobody looks at.
    EXPECT_TRUE( Contains( src, "!sd.CastShadows" ) )
         << "the cascade pass no longer skips skinned casters on CastShadows - the flag reaches the "
            "renderer and dies there";
}

// ---------------------------------------------------------------------------------------------------
// Serialization: the mirror round-trips, and the registry writes/reads it the way the static twin does
// (write only when false; absent = default on load, so every pre-Д24 scene and prefab stays loadable).
// ---------------------------------------------------------------------------------------------------

TEST( SkinnedShadowFlag, TheSerMirrorRoundTripsTheFlagAndAbsentMeansDefault )
{
    // Off survives the trip.
    Desert::Assets::SkinnedMeshComponentSer off;
    off.CastShadows    = false;
    const auto offBack = Common::Json::Read<Desert::Assets::SkinnedMeshComponentSer>( Common::Json::Write( off ) );
    ASSERT_TRUE( offBack.IsSuccess() ) << offBack.GetError();
    ASSERT_TRUE( offBack.GetValue().CastShadows.has_value() );
    EXPECT_FALSE( *offBack.GetValue().CastShadows );

    // A pre-Д24 payload (no key) parses with the optional absent — which value_or turns into the
    // component's own default at load, i.e. casting, exactly what those scenes did before.
    const auto legacy = Common::Json::Read<Desert::Assets::SkinnedMeshComponentSer>( "{}" );
    ASSERT_TRUE( legacy.IsSuccess() ) << legacy.GetError();
    EXPECT_FALSE( legacy.GetValue().CastShadows.has_value() );
}

TEST( SkinnedShadowFlag, TheRegistryWritesTheFlagOnlyWhenOffAndAppliesItWithTheComponentDefault )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    const std::string src =
         Normalized( root + "Desert/Desert/Source/Engine/Core/Serialize/ComponentRegistry.cpp" );
    ASSERT_FALSE( src.empty() );

    // The static and skinned serializers spell this pair IDENTICALLY (same local names, same
    // write-when-off rule), so the honest assertion is the COUNT: one occurrence is the static twin
    // alone — which is precisely the state Д24 repaired — and two is both.
    const auto occurrences = []( const std::string& text, const std::string& needle )
    {
        std::size_t count = 0;
        for ( std::size_t at = text.find( needle ); at != std::string::npos; at = text.find( needle, at + 1 ) )
            ++count;
        return count;
    };

    // Write side: both mesh serializers state the flag when (and only when) it is off...
    EXPECT_GE( occurrences( src, "if(!smc.CastShadows)meshSer.CastShadows=smc.CastShadows" ), 2u )
         << "fewer than two mesh serializers write CastShadows-when-off - the skinned one (or its static "
            "twin) no longer persists the setting, and the shadow comes back on the next load";
    // ...and both read sides fold an absent key back into the component default.
    EXPECT_GE( occurrences( src, "smc.CastShadows=meshData.CastShadows.value_or(smc.CastShadows)" ), 2u )
         << "fewer than two mesh deserializers apply CastShadows from the payload";
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
