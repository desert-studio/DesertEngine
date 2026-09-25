// HOW A REFERENCE IS SPELLED IN A `.desce`, ASSERTED — which nothing could do until T2.4.
//
// WHAT THIS IS ABOUT. `Core::MakeAssetResolver::ToPath` used to answer two questions at once: WHICH
// FILE a handle names (twelve per-type lookups into the AssetManager) and HOW that file is spelled in
// the scene (a different string per type, folded into the same twelve branches). The cooked asset
// registry answers the first once, for every type; what is left is the second, and it is this table.
//
// WHY IT HAD TO BE EXTRACTED FROM ComponentRegistry.cpp. That file reaches the ResourceRegistry and
// through it the whole renderer, so NOTHING defined in it is reachable by any suite in this
// repository. The twelve branches were therefore never once executed by a test, and all three defects
// they carried reached `dev` that way: an absolute machine-local path in a committed scene, a silent
// zero on a miss, and an unhandled type looked up as a mesh. `TextureSlot.cpp` was carved out of the
// same lambda for the same reason and says so in its own header; this is the second half.
//
// THE PROPERTY UNDER TEST IS NOT "the forms are sensible". It is that each form reproduces, BYTE FOR
// BYTE, what that type's hand-written branch produced before the collapse — because the string in a
// `.desce` is a FORMAT, and changing it silently is a corpus migration performed by accident. 114
// scenes in this repository hold these strings.

#include <Engine/Core/Serialize/StoredAssetForm.hpp>

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/Constants.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

using Desert::Core::Serialize::RenderStoredForm;
using Desert::Core::Serialize::StoredAssetForm;
using Desert::Core::Serialize::StoredFormFor;

namespace
{
    // The forms are relative to the LIVE content roots, so a project has to be open for the answers to
    // mean anything — with no project the roots are the sandbox's, relative to the working directory,
    // and `assets:` would expand to something that depends on where the test was launched.
    class OpenProject
    {
    public:
        explicit OpenProject( const std::filesystem::path& dir )
             : m_Saved( Common::Constants::Path::CurrentProjectRoot() )
        {
            Common::Constants::Path::SetProjectRoot( dir, "Content" );
        }
        ~OpenProject()
        {
            Common::Constants::Path::SetProjectRoot( m_Saved.ProjectDir, m_Saved.AssetsRoot );
        }

    private:
        Common::Constants::Path::ProjectRootState m_Saved;
    };

    const std::filesystem::path kProject = std::filesystem::path( "/tmp/desert-stored-form/checkout" );
} // namespace

// ── EVERY TYPE HAS A FORM, AND THE ONES THAT DO NOT ARE REFUSED ─────────────────────────────────────

TEST( StoredAssetForm, EveryAssetTypeTheEngineSerializesHasAForm )
{
    // The twelve the branch chain used to handle, one at a time. Not a loop over a list defined in the
    // test: a list here would simply mirror the table and pass whatever the table said.
    EXPECT_EQ( StoredFormFor( "TextureAsset" ), StoredAssetForm::StableKey );
    EXPECT_EQ( StoredFormFor( "FontAsset" ), StoredAssetForm::StableKey );
    EXPECT_EQ( StoredFormFor( "VideoAsset" ), StoredAssetForm::StableKey );
    EXPECT_EQ( StoredFormFor( "IconAsset" ), StoredAssetForm::StableKey );

    EXPECT_EQ( StoredFormFor( "MaterialAsset" ), StoredAssetForm::AssetsRelative );
    EXPECT_EQ( StoredFormFor( "CloudModellingVolumeAsset" ), StoredAssetForm::AssetsRelative );
    EXPECT_EQ( StoredFormFor( "ControlRigAsset" ), StoredAssetForm::AssetsRelative );
    EXPECT_EQ( StoredFormFor( "AnimGraphAsset" ), StoredAssetForm::AssetsRelative );
    EXPECT_EQ( StoredFormFor( "UIThemeAsset" ), StoredAssetForm::AssetsRelative );

    EXPECT_EQ( StoredFormFor( "StaticMeshAsset" ), StoredAssetForm::MachinePath );
    EXPECT_EQ( StoredFormFor( "SkinnedMeshAsset" ), StoredAssetForm::MachinePath );
    EXPECT_EQ( StoredFormFor( "MeshAsset" ), StoredAssetForm::MachinePath );
    EXPECT_EQ( StoredFormFor( "SkyboxAsset" ), StoredAssetForm::ProjectKey );
}

// SCNE 29: a skybox is stored as a project key or not at all - the absolute path cannot be produced.
TEST( StoredAssetForm, AProjectKeyIsTheTaggedKeyAndAnythingElseRendersEmpty )
{
    EXPECT_EQ( RenderStoredForm( StoredAssetForm::ProjectKey, "assets:Textures/HDR/Sky.detex" ),
               "assets:Textures/HDR/Sky.detex" );
    EXPECT_EQ( RenderStoredForm( StoredAssetForm::ProjectKey, "/Users/somebody/Sky.detex" ), "" );
    EXPECT_EQ( RenderStoredForm( StoredAssetForm::ProjectKey, "Textures/HDR/Sky.detex" ), "" );
}

TEST( StoredAssetForm, AnUnknownTypeHasNoFormRatherThanFallingThroughToOne )
{
    // THE DEFECT THIS REPLACES, NAMED. The mesh lookup used to END the branch chain unconditionally,
    // so `PROPERTY( Asset<AudioAsset> )` added tomorrow was looked up as a MESH, found nothing, and
    // returned an empty string — a working Details picker over a slot that saves as "" and loads as
    // unset, with nothing anywhere saying so.
    EXPECT_FALSE( StoredFormFor( "AudioAsset" ).has_value() );
    EXPECT_FALSE( StoredFormFor( "" ).has_value() );
    EXPECT_FALSE( StoredFormFor( "PrefabAsset" ).has_value() );
    // Case matters: the metadata string is generated from the `Asset<...>` annotation verbatim.
    EXPECT_FALSE( StoredFormFor( "textureasset" ).has_value() );
}

// ── THE THREE RENDERINGS, AGAINST WHAT THE OLD BRANCHES PRODUCED ────────────────────────────────────

TEST( StoredAssetForm, ATaggedKeyIsWrittenVerbatimAndIsTheOnlyFormThatCanNameBothRoots )
{
    const OpenProject open( kProject );

    // A cooked texture lives under COOKED_PATH, a SIBLING of the assets root. That is why the texture
    // slot stores the tag: relative-to-the-assets-root gives `../Cooked/...` for this file and falls
    // back to the absolute spelling, i.e. to a developer's home directory in a committed scene.
    EXPECT_EQ( RenderStoredForm( StoredAssetForm::StableKey, "cooked:Textures/T_Probe.tex" ),
               "cooked:Textures/T_Probe.tex" );
    EXPECT_EQ( RenderStoredForm( StoredAssetForm::StableKey, "assets:Textures/T_Content.tex" ),
               "assets:Textures/T_Content.tex" );
    EXPECT_EQ( RenderStoredForm( StoredAssetForm::StableKey, "engine:Fonts/Roboto-Regular.ttf" ),
               "engine:Fonts/Roboto-Regular.ttf" );
}

TEST( StoredAssetForm, AssetsRelativeStripsTheTagAndReproducesWhatRelativeToTheAssetsRootGave )
{
    const OpenProject open( kProject );

    const std::filesystem::path file = kProject / "Content" / "Materials" / "M_Rock.demat";
    const std::string           key  = Common::AssetHandle::StableKeyForPath( file );
    ASSERT_EQ( key, "assets:Materials/M_Rock.demat" ) << "the fixture does not sit under the assets root";

    // THE RELATION, and it is the one that matters: the rendering must equal what the branch this
    // replaces computed — `std::filesystem::relative( filepath, ASSETS_PATH ).generic_string()`.
    std::error_code   ec;
    const std::string wasProducedBefore =
         std::filesystem::relative( file, Common::Constants::Path::ASSETS_PATH, ec ).generic_string();
    ASSERT_FALSE( ec );

    EXPECT_EQ( RenderStoredForm( StoredAssetForm::AssetsRelative, key ), wasProducedBefore );
    EXPECT_EQ( RenderStoredForm( StoredAssetForm::AssetsRelative, key ), "Materials/M_Rock.demat" );
}

TEST( StoredAssetForm, AFileOutsideTheAssetsRootKeepsItsOwnSpellingInsteadOfEscapingUpwards )
{
    const OpenProject open( kProject );

    // The branches said it out loud — "outside the project — say so plainly" — and the reason is that
    // relative-to-the-assets-root produces `../Cooked/...` here, which resolves against whatever the
    // reader's assets root happens to be. A body under the COOKED root, and a file under no root at
    // all, both take the path rather than a `..` chain.
    const std::string cooked = RenderStoredForm( StoredAssetForm::AssetsRelative, "cooked:Volumes/B.dcmv" );
    EXPECT_EQ( cooked.rfind( "..", 0 ), std::string::npos ) << cooked;
    EXPECT_EQ( cooked, ( Common::Constants::Path::COOKED_PATH / "Volumes/B.dcmv" ).lexically_normal().string() );

    const std::string outside = RenderStoredForm( StoredAssetForm::AssetsRelative, "/elsewhere/X.demat" );
    EXPECT_EQ( outside, "/elsewhere/X.demat" );
}

TEST( StoredAssetForm, AMachinePathIsTheFileAsThisMachineSpellsIt )
{
    const OpenProject open( kProject );

    // Meshes and skyboxes. This is what `GetMetadata().Filepath.string()` gave, and the equality is
    // asserted through the round trip rather than assumed: the preloader creates a mesh shell at
    // exactly `PathForStableKey( row.Key )`, so the two must be the same string.
    const std::filesystem::path mesh =
         ( Common::Constants::Path::COOKED_PATH / "Meshes/Probe.stmesh" ).lexically_normal();
    const std::string key = Common::AssetHandle::StableKeyForPath( mesh );
    ASSERT_EQ( key, "cooked:Meshes/Probe.stmesh" );

    EXPECT_EQ( RenderStoredForm( StoredAssetForm::MachinePath, key ), mesh.string() );
}

TEST( StoredAssetForm, TheMachinePathFormIsTheONEThatStillCarriesTheCheckoutDirectory )
{
    // A DEFECT RECORDED WHERE IT WILL BE FOUND, not fixed here. The material branch was repaired in
    // I13 after 22 distinct `/Users/<somebody>/.../Materials/*.demat` were found in 42 of 51 scenes;
    // the MESH and SKYBOX branches were never given the same treatment and still write the absolute
    // path. Collapsing the twelve branches preserved that behaviour deliberately — changing it rewrites
    // every scene that holds a mesh — so the property is asserted as it IS, and this test is the thing
    // that will have to be edited by whoever fixes it.
    const OpenProject open( kProject );

    // BOTH SIDES NORMALISED, because the rendered path is MIXED on Windows and neither pure spelling
    // occurs in it. `path( "/tmp/x" ).string()` keeps the forward slashes it was given, while the `/`
    // operator that appends the relative part inserts the PREFERRED separator — so the result reads
    // `/tmp/desert-stored-form/checkout\Content\...`. Searching it for the all-forward spelling fails,
    // and so does searching for the all-backward one; the first fix here swapped one for the other and
    // was still wrong. What the test actually means is "this string contains the checkout directory",
    // and that question only has an answer once both sides are spelled the same way.
    const std::string machine = RenderStoredForm( StoredAssetForm::MachinePath, "cooked:Meshes/Probe.stmesh" );
    const std::string mesh    = std::filesystem::path( machine ).generic_string();
    EXPECT_NE( mesh.find( kProject.generic_string() ), std::string::npos )
         << "the mesh form no longer carries the checkout directory. That is an IMPROVEMENT and a "
            "format change: every `.desce` holding a mesh or skybox reference has to be migrated in the "
            "same commit, and this test updated to assert the new form.";

    // While the two forms beside it do not.
    EXPECT_EQ( RenderStoredForm( StoredAssetForm::StableKey, "cooked:Textures/T.tex" ).find( kProject.string() ),
               std::string::npos );
    EXPECT_EQ( RenderStoredForm( StoredAssetForm::AssetsRelative, "assets:Materials/M.demat" )
                    .find( kProject.generic_string() ),
               std::string::npos );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
