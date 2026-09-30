// EDIT THUMBNAIL'S PURE HALF, asserted as relations:
//   * a drag, a wheel and a palette step obey ONE set of rules (yaw wrapped to (-180, 180], pitch clamped to
//     +-89, zoom kept above -0.9, wheel forward = closer), because a step is a drag of a fixed length;
//   * the live preview captures the NEWEST orbit only: a request replaces the one waiting, and the orbit
//     already in flight or already on screen is never asked for twice;
//   * the preview's picture is filed apart from the cached thumbnail, so it can never be read as one.

#include <gtest/gtest.h>

#include <Editor/Import/CookPaths.hpp>
#include <Editor/Widgets/ThumbnailKey.hpp>
#include <Editor/Widgets/ThumbnailProducers.hpp>
#include <Editor/Widgets/ThumbnailOrbitEdit.hpp>
#include <Editor/Widgets/ThumbnailPreview.hpp>

namespace
{
    using Desert::Assets::ThumbnailOrbit;
    namespace TE = Desert::Editor::ThumbnailEdit;

    constexpr float kPixelsPerDegree = 1.0f / TE::kDegreesPerPixel;
} // namespace

TEST( ThumbnailOrbitEdit, YawWrapsAt180FromBothSides )
{
    const ThumbnailOrbit near{ 0.0f, 170.0f, 0.0f };
    EXPECT_FLOAT_EQ( TE::Orbited( near, 20.0f * kPixelsPerDegree, 0.0f, 0.0f ).Yaw, -170.0f );
    EXPECT_FLOAT_EQ( TE::Orbited( near, 10.0f * kPixelsPerDegree, 0.0f, 0.0f ).Yaw, 180.0f ); // (-180, 180]
    const ThumbnailOrbit farSide{ 0.0f, -170.0f, 0.0f };
    EXPECT_FLOAT_EQ( TE::Orbited( farSide, -10.0f * kPixelsPerDegree, 0.0f, 0.0f ).Yaw, 180.0f );
    for ( int turns = -3; turns <= 3; ++turns )
    {
        const float yaw =
             TE::Orbited( {}, static_cast<float>( turns ) * 400.0f * kPixelsPerDegree, 0.0f, 0.0f ).Yaw;
        EXPECT_GT( yaw, -180.0f );
        EXPECT_LE( yaw, 180.0f );
    }
}

TEST( ThumbnailOrbitEdit, PitchStopsAt89AndZoomAboveMinus09 )
{
    EXPECT_FLOAT_EQ( TE::Orbited( {}, 0.0f, 1000.0f, 0.0f ).Pitch, TE::kMaxPitch );
    EXPECT_FLOAT_EQ( TE::Orbited( {}, 0.0f, -1000.0f, 0.0f ).Pitch, -TE::kMaxPitch );
    EXPECT_FLOAT_EQ( TE::kMaxPitch, 89.0f );
    const ThumbnailOrbit in = TE::Orbited( {}, 0.0f, 0.0f, 1000.0f );
    EXPECT_FLOAT_EQ( in.Zoom, -0.9f );
    EXPECT_TRUE( Desert::Assets::IsValidThumbnailOrbit( in ) ); // the record accepts what a drag makes
}

TEST( ThumbnailOrbitEdit, WheelForwardComesCloser )
{
    EXPECT_LT( TE::Orbited( {}, 0.0f, 0.0f, 1.0f ).Zoom, 0.0f );
    EXPECT_GT( TE::Orbited( {}, 0.0f, 0.0f, -1.0f ).Zoom, 0.0f );
    EXPECT_LT( TE::Stepped( {}, TE::OrbitStep::ZoomIn ).Zoom, TE::Stepped( {}, TE::OrbitStep::ZoomOut ).Zoom );
}

TEST( ThumbnailOrbitEdit, StepsObeyTheDragsRulesAndResetIsTheDefault )
{
    const ThumbnailOrbit edge{ 80.0f, 170.0f, -0.8f };
    EXPECT_EQ( TE::Stepped( edge, TE::OrbitStep::YawPlus ), TE::Orbited( edge, 45.0f * kPixelsPerDegree, 0, 0 ) );
    EXPECT_FLOAT_EQ( TE::Stepped( edge, TE::OrbitStep::YawPlus ).Yaw, -145.0f );
    EXPECT_FLOAT_EQ( TE::Stepped( edge, TE::OrbitStep::PitchPlus ).Pitch, 89.0f );
    EXPECT_FLOAT_EQ( TE::Stepped( edge, TE::OrbitStep::ZoomIn ).Zoom, -0.9f );
    EXPECT_EQ( TE::Stepped( edge, TE::OrbitStep::Reset ), ThumbnailOrbit{} );
}

TEST( ThumbnailOrbitEdit, EveryStepHasItsOwnName )
{
    for ( const auto a : TE::kOrbitSteps )
    {
        EXPECT_NE( TE::OrbitStepName( a ), "unknown step" );
        for ( const auto b : TE::kOrbitSteps )
            if ( a != b )
                EXPECT_NE( TE::OrbitStepName( a ), TE::OrbitStepName( b ) );
    }
}

TEST( ThumbnailPreviewSlot, TheLastRequestWins )
{
    Desert::Editor::ThumbnailPreview::Slot<int> slot;
    EXPECT_TRUE( slot.Put( "a", { 0, 10, 0 }, 1 ) );
    EXPECT_TRUE( slot.Put( "a", { 0, 20, 0 }, 2 ) );
    const auto taken = slot.Take();
    if ( !taken.has_value() )
        FAIL() << "a waiting orbit was not handed out";
    EXPECT_EQ( taken->What, 2 );
    EXPECT_FALSE( slot.Waiting() );
    // One capture at a time: a newer orbit waits behind the one in flight, and replaces any other waiting one.
    EXPECT_TRUE( slot.Put( "a", { 0, 30, 0 }, 3 ) );
    EXPECT_TRUE( slot.Put( "a", { 0, 40, 0 }, 4 ) );
    EXPECT_FALSE( slot.Take() );
    slot.Land();
    EXPECT_EQ( slot.LandedOrbit( "a" ), ( ThumbnailOrbit{ 0, 20, 0 } ) );
    const auto next = slot.Take();
    if ( !next.has_value() )
        FAIL() << "the orbit waiting behind the landed one was not handed out";
    EXPECT_EQ( next->What, 4 );
}

TEST( ThumbnailPreviewSlot, AnOrbitInFlightOrOnScreenIsNotAskedTwice )
{
    Desert::Editor::ThumbnailPreview::Slot<int> slot;
    slot.Put( "a", { 0, 10, 0 }, 1 );
    slot.Take();
    EXPECT_TRUE( slot.Put( "a", { 0, 20, 0 }, 2 ) );
    EXPECT_FALSE( slot.Put( "a", { 0, 10, 0 }, 3 ) ); // back to the one in flight: the waiting one is dropped
    EXPECT_FALSE( slot.Waiting() );
    slot.Land();
    EXPECT_FALSE( slot.Put( "a", { 0, 10, 0 }, 4 ) ); // already on screen
    EXPECT_TRUE( slot.Put( "b", { 0, 10, 0 }, 5 ) );  // same orbit, another asset
}

TEST( ThumbnailPreviewSlot, EndingTheGestureForgetsItsPictureNotAnotherAssets )
{
    Desert::Editor::ThumbnailPreview::Slot<int> slot;
    slot.Put( "a", { 0, 10, 0 }, 1 );
    slot.Take();
    slot.Land();
    slot.Put( "a", { 0, 20, 0 }, 2 );
    slot.End( "b" );
    EXPECT_TRUE( slot.Waiting() );
    EXPECT_TRUE( slot.LandedOrbit( "a" ) );
    slot.End( "a" );
    EXPECT_FALSE( slot.Waiting() );
    EXPECT_FALSE( slot.LandedOrbit( "a" ) );
}

TEST( ThumbnailPreviewKey, ThePreviewIsFiledApartFromTheCachedThumbnail )
{
    namespace Key           = Desert::Editor::ThumbnailKey;
    const std::string asset = "Materials/M_Wood.demat";
    const std::string other = "Materials/M_Stone.demat";
    EXPECT_NE( Key::PreviewPath( asset ), Key::DiskPath( asset ) );
    EXPECT_NE( Key::PreviewPath( asset ), Key::PreviewPath( other ) );
    EXPECT_EQ( Key::PreviewPath( asset ), Key::PreviewPath( asset ) );
    const auto cacheDir = std::filesystem::path( Key::DiskPath( asset ) ).parent_path();
    EXPECT_NE( std::filesystem::path( Key::PreviewPath( asset ) ).parent_path(), cacheDir );
}

// MCP-CMD2: UE offers Edit Thumbnail on every class whose picture is shot through an orbit camera — a skeletal
// mesh, a skeleton and an animation as much as a static mesh or a material. The live check was refused on
// Fox.skmesh as "not a model".
TEST( ThumbnailOrbitKinds, EveryRenderedPictureHasAnOrbitAndNoOtherDoes )
{
    using Desert::Editor::FileType;
    namespace TP = Desert::Editor::ThumbnailProducers;
    for ( const FileType type : { FileType::Model, FileType::Material, FileType::SkinnedMesh, FileType::Skeleton,
                                  FileType::Animation, FileType::FoliageType } )
        EXPECT_TRUE( TP::HasThumbnailOrbit( type ) ) << static_cast<int>( type );
    for ( const FileType type : { FileType::Texture, FileType::Cloud, FileType::Skybox, FileType::Scene } )
        EXPECT_FALSE( TP::HasThumbnailOrbit( type ) ) << static_cast<int>( type );
}

// A posed picture's orbit lives in the record of the skinned source whose import wrote the file
// (MeshThumbnailHome): the name rule that finds it.
TEST( ThumbnailOrbitKinds, ASkinnedImportsFilesAreFoundByTheirSourcesName )
{
    namespace CP = Desert::Editor::CookPaths;
    const std::filesystem::path fox = "Assets/Meshes/Fox.glb";
    EXPECT_TRUE( CP::IsSkinnedAssetOf( fox, "Assets/Meshes/Fox.skmesh" ) );
    EXPECT_TRUE( CP::IsSkinnedAssetOf( fox, "Assets/Meshes/Fox.skeleton" ) );
    EXPECT_TRUE( CP::IsSkinnedAssetOf( fox, "Assets/Meshes/Fox_Walk.anim" ) );
    EXPECT_FALSE( CP::IsSkinnedAssetOf( fox, "Assets/Meshes/Fox_.anim" ) ) << "a clip has a name";
    EXPECT_FALSE( CP::IsSkinnedAssetOf( fox, "Assets/Meshes/Foxy.skmesh" ) );
    EXPECT_FALSE( CP::IsSkinnedAssetOf( fox, "Assets/Other/Fox.skmesh" ) ) << "written beside the source only";
    EXPECT_FALSE( CP::IsSkinnedAssetOf( fox, "Assets/Meshes/Fox.stmesh" ) );
    EXPECT_TRUE( CP::IsSkinnedAssetFile( "a/Fox.skmesh" ) );
    EXPECT_TRUE( CP::IsSkinnedAssetFile( "a/Fox_Walk.anim" ) );
    EXPECT_FALSE( CP::IsSkinnedAssetFile( "a/Fox.stmesh" ) );
    EXPECT_FALSE( CP::IsSkinnedAssetFile( "a/Fox.glb" ) );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
