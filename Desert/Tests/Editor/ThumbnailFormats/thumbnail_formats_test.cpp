// "EVERY FORMAT THE CONTENT BROWSER CAN SHOW EITHER HAS A THUMBNAIL PRODUCER OR A NAMED REASON WHY NOT."
//
// THE MEASUREMENT THIS IS BUILT ON. `ThumbnailService` knew exactly TWO formats — Material and Mesh. The
// four cloud formats (`.dclayout`, `.dcnv`, `.dcmv`, `.decloudtype`) had no producer at all, and until
// M11 the Content Browser did not even TYPE them: they fell through to FileType::Unknown, so four
// different assets drew one identical grey document glyph and the owner could not pick a cloud by
// looking at it. Nothing was broken. Nothing said anything either — which is the "empty successful
// answer" §1.4 of the contract forbids, wearing an icon instead of a return value.
//
// SO THE QUESTION IS ASKED OF EVERY FORMAT, ALONG ONE CHAIN (THM1n-3, UE's UThumbnailManager: one
// registration per asset class). extension -> FileType (FileType.hpp, kFileExtensions — the map the
// browser itself types files with) -> Producer (ThumbnailProducers.hpp). And the subject list is not typed
// here: every engine asset format is read out of Common's ContentKinds, so the format added next year by
// somebody who has never read this file is a red census, not a quiet grey glyph (`.detex` was exactly that
// until THM1n-3 — an imported texture drew a grey square).
//
// AND A ROW IS NOT TAKEN ON TRUST. Every `Producer::Painted` row is PAINTED HERE, against the shipped
// asset library, and the result is asserted to be a picture rather than a flat square — because a
// producer that returns success and fills one colour is exactly the failure this subsystem keeps
// meeting, and it is invisible to any test that only checks the return value.
//
// WHAT WOULD MAKE THIS RED, and each is a real mistake:
//   * an engine asset format the browser does not type, and that the register below does not name;
//   * a kind the browser types with no producer row;
//   * a second extension map in the panel, answering the question the chain answers;
//   * a painted producer that stops producing, or starts producing a uniform square.

#include <Common/Content/ContentKinds.hpp>

#include <Editor/Widgets/CloudThumbnail.hpp>
#include <Editor/Widgets/ThumbnailFormats.hpp>
#include <Editor/Widgets/ThumbnailProducers.hpp>
#include <Editor/Widgets/ThumbnailFreshness.hpp>
#include <Editor/Widgets/ThumbnailSubject.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    namespace fs = std::filesystem;
    namespace TF = Desert::Editor::ThumbnailFormats;
    namespace TP = Desert::Editor::ThumbnailProducers;
    using Desert::Editor::FileType;
    using Desert::Editor::FileTypeOf;
    using Desert::Editor::kFileExtensions;

    constexpr const char* kBrowserTable = "Editor/Source/Editor/Panels/FileExplorer/FileExplorerPanel.cpp";

    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            std::ifstream probe( prefix + "Editor/Source/Editor/Widgets/ThumbnailFormats.hpp" );
            if ( probe )
                return prefix;
            prefix += "../";
        }
        return {};
    }

    std::string ReadFile( const std::string& path )
    {
        std::ifstream in( path, std::ios::binary );
        if ( !in )
            return {};
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    std::vector<fs::path> ShippedAssets( const std::string& root, const std::string& extension )
    {
        std::vector<fs::path> out;
        std::error_code       ec;
        const fs::path        tree = fs::path( root ) / "Editor" / "Resources" / "Assets";

        for ( fs::recursive_directory_iterator it( tree, ec ), end; it != end && !ec; it.increment( ec ) )
        {
            if ( !it->is_regular_file( ec ) )
                continue;
            if ( TF::ExtensionOf( it->path().generic_string() ) == extension )
                out.push_back( it->path() );
        }
        std::sort( out.begin(), out.end() );
        return out;
    }

    std::string Lowered( std::string_view text )
    {
        std::string out( text );
        for ( char& c : out )
            c = static_cast<char>( std::tolower( static_cast<unsigned char>( c ) ) );
        return out;
    }
} // namespace

// ---------------------------------------------------------------------------------------------------
// 0. The extension map is well formed: one row per extension, spelled the way ExtensionOf answers.
// ---------------------------------------------------------------------------------------------------
TEST( ThumbnailFormats, EveryExtensionIsWellFormedAndUnique )
{
    std::set<std::string_view> seen;
    for ( const auto& row : kFileExtensions )
    {
        EXPECT_FALSE( row.Extension.empty() );
        EXPECT_EQ( Lowered( row.Extension ), row.Extension ) << "'" << row.Extension << "' is not lower case";
        EXPECT_EQ( row.Extension.find( '.' ), std::string_view::npos ) << "'" << row.Extension << "' has a dot";
        EXPECT_NE( row.Type, FileType::Unknown ) << "'." << row.Extension << "' is mapped to Unknown";
        EXPECT_TRUE( seen.insert( row.Extension ).second )
             << "'." << row.Extension << "' appears twice — FileTypeOf answers with the first";
    }
}

// ---------------------------------------------------------------------------------------------------
// 1. THE CHAIN REACHES A PRODUCER FOR EVERY EXTENSION THE BROWSER TYPES.
//
// Asked through ProducerOfPath, the one entry point a path is asked by, and held against the kind's own row
// so the chain cannot answer differently from its last link.
// ---------------------------------------------------------------------------------------------------
TEST( ThumbnailFormats, EveryExtensionTheBrowserTypesReachesAProducer )
{
    for ( const auto& row : kFileExtensions )
    {
        const std::string path = "Assets/Some/File." + std::string( row.Extension );
        EXPECT_EQ( FileTypeOf( TF::ExtensionOf( path ) ), row.Type ) << path;
        const std::optional<TP::Producer> chained = TP::ProducerOfPath( path );
        ASSERT_TRUE( chained.has_value() )
             << "'." << row.Extension
             << "' is typed by the browser, but its kind has no row in ThumbnailProducers::kTable — nobody "
                "decided what its picture is. Add the row: a producer, or TypeIcon with the reason.";
        EXPECT_EQ( chained, TP::ProducerOf( row.Type ) ) << path;
    }
}

// ---------------------------------------------------------------------------------------------------
// 2. EVERY ENGINE ASSET FORMAT IS TYPED BY THE BROWSER.
//
// The subject list is Common's ContentKinds, the engine's own list of asset formats. There is no excuse
// register any more (THM1n-4 typed the last ten kinds): an engine format the browser draws as Unknown is red.
// ---------------------------------------------------------------------------------------------------
TEST( ThumbnailFormats, EveryEngineAssetFormatIsTypedByTheBrowser )
{
    for ( const auto& spec : Common::Content::ContentKinds() )
    {
        if ( spec.Extension.empty() )
            continue; // stated only inside another file (Redirector): there is no file to show
        ASSERT_EQ( spec.Extension.front(), '.' ) << spec.Name;
        const FileType type = FileTypeOf( spec.Extension.substr( 1 ) );
        EXPECT_NE( type, FileType::Unknown )
             << "the engine asset kind " << spec.Name << " ('" << spec.Extension
             << "') is in no row of FileType.hpp's kFileExtensions, so the browser draws it as Unknown with "
                "a grey glyph. Type it there (and give its kind a ThumbnailProducers row).";
        EXPECT_TRUE( TP::ProducerOfPath( std::string( "Assets/x" ) + std::string( spec.Extension ) )
                          .has_value() )
             << spec.Name << ": typed, but its kind has no producer row";
    }
}

// ---------------------------------------------------------------------------------------------------
// 3. THE BROWSER HAS NO SECOND MAP. The panel types a file through FileTypeOf and draws through
// ThumbnailProducers; a private extension table there would be the third answer this task removed.
// ---------------------------------------------------------------------------------------------------
TEST( ThumbnailFormats, TheBrowserTypesAndDrawsThroughTheOneChain )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );
    const std::string panel = ReadFile( root + kBrowserTable );
    ASSERT_FALSE( panel.empty() ) << kBrowserTable;
    EXPECT_EQ( panel.find( "std::unordered_map<std::string, FileType>" ), std::string::npos )
         << "FileExplorerPanel.cpp has its own extension map again; kFileExtensions is the one map";
    EXPECT_NE( panel.find( "FileTypeOf(" ), std::string::npos ) << "the panel no longer types through FileTypeOf";
    EXPECT_NE( panel.find( "ThumbnailProducers::ProducerOf" ), std::string::npos )
         << "the panel no longer dispatches its thumbnails through ThumbnailProducers";
}

// ---------------------------------------------------------------------------------------------------
// 3b. THE FOUR CLOUD FORMATS, BY NAME — the owner's requirement rather than a property of the mechanism
// ("владелец не может выбрать облака картинкой"); a generic census would stay green if their kind became
// TypeIcon.
// ---------------------------------------------------------------------------------------------------
TEST( ThumbnailFormats, EveryCloudFormatHasAPicture )
{
    for ( const char* extension : { "dclayout", "dcnv", "dcmv", "decloudtype" } )
    {
        const std::string path = std::string( "Clouds/X." ) + extension;
        EXPECT_EQ( TP::ProducerOfPath( path ), TP::Producer::Painted )
             << "'." << extension << "' no longer has a picture: the owner picks a cloud by looking at it.";
    }
}

// ---------------------------------------------------------------------------------------------------
// 4. A PAINTED ROW REALLY PAINTS — over the shipped library, and the result is a PICTURE.
//
// THE UNIFORMITY CHECK IS THE POINT OF THIS TEST. A producer that decoded the file, filled the square
// with the backdrop and returned success would satisfy every check that only reads the return value,
// and it would look — in the grid — exactly like the grey glyph this whole task is about replacing. So
// the assertion is that the square has structure in it.
// ---------------------------------------------------------------------------------------------------
TEST( ThumbnailFormats, EveryPaintedFormatPaintsTheShippedLibraryAndNotAFlatSquare )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    int painted = 0;
    for ( const auto& row : kFileExtensions )
    {
        if ( TP::ProducerOf( row.Type ) != TP::Producer::Painted )
            continue;

        const std::string           extension = std::string( row.Extension );
        const std::vector<fs::path> assets    = ShippedAssets( root, extension );
        ASSERT_FALSE( assets.empty() )
             << "no '." << extension
             << "' exists under Editor/Resources/Assets, so this row's producer is asserted against "
                "nothing. Point the row at a format the repository ships, or ship one.";

        int paintedThisFormat = 0;
        for ( const fs::path& asset : assets )
        {
            const auto pixels = Desert::Editor::CloudThumbnail::Paint( asset.generic_string() );

            // A REASONED REFUSAL IS A LEGAL ANSWER, and it is the RIGHT one for a file that has nothing
            // in it: two shipped layouts (O4_MaskNeutral, O4_MaskAddRemove) are fixtures whose pattern is
            // all zeros, and one of them says nothing at all. A flat square for those would be
            // indistinguishable from a producer that failed — and the freshness rule would then call that
            // flat square a good picture of the asset for ever. What is NOT legal is a refusal with no
            // reason, or a success that paints nothing.
            if ( !pixels.IsSuccess() )
            {
                EXPECT_GT( pixels.GetError().size(), 30u )
                     << asset.generic_string()
                     << " was refused without an argument. A refusal has to say what about THIS file made "
                        "a picture the wrong answer, or the next reader cannot tell it from a defect.";
                continue;
            }

            const std::vector<unsigned char>& rgba = pixels.GetValue();
            const std::size_t                 side = Desert::Editor::CloudThumbnail::kSize;
            ASSERT_EQ( rgba.size(), side * side * 4u )
                 << asset.generic_string() << " painted " << rgba.size() << " bytes, not " << side << "x" << side
                 << " RGBA8. Every producer must write at the ONE size ThumbnailCache uploads at, "
                    "or the grid shows two of them at two sharpnesses.";

            std::set<std::array<unsigned char, 3>> tones;
            for ( std::size_t i = 0; i + 3 < rgba.size(); i += 4 )
            {
                tones.insert( { rgba[i], rgba[i + 1], rgba[i + 2] } );
                if ( tones.size() > 4 )
                    break;
                EXPECT_EQ( rgba[i + 3], 255u ) << asset.generic_string()
                                               << " painted a transparent pixel. The grid composites over "
                                                  "whatever colour it happens to sit on, so a tile with "
                                                  "alpha reads differently in two panels.";
            }
            EXPECT_GT( tones.size(), 4u )
                 << asset.generic_string() << " painted a square with " << tones.size()
                 << " distinct colours in it — that is a flat fill, not a picture.\n"
                    "  A producer that returns success and paints one colour passes every check that "
                    "reads only its return value, and in the grid it is indistinguishable from the grey "
                    "glyph this whole feature replaces. If the FILE is genuinely empty, refuse it by name "
                    "instead of painting nothing.";
            ++painted;
            ++paintedThisFormat;
        }

        EXPECT_GT( paintedThisFormat, 0 )
             << "not one shipped '." << extension
             << "' could be painted. A producer that refuses its own format's entire library is a "
                "TypeIcon row wearing a Painted label.";
    }

    EXPECT_GE( painted, 15 ) << "only " << painted
                             << " shipped assets were painted; the library has more than that and a "
                                "sudden shrinkage is the one thing a per-file report cannot say";
}

// ---------------------------------------------------------------------------------------------------
// 5. THE PAINTER REFUSES WHAT IT CANNOT PAINT, AND SAYS WHY.
//
// The other half of §1.4: an unpaintable file must be distinguishable from a paintable one BY THE
// CALLER. If a corrupt container painted a blank square and returned success, the freshness rule would
// then call that blank square fresh for ever — the one state it cannot repair.
// ---------------------------------------------------------------------------------------------------
TEST( ThumbnailFormats, ThePainterRefusesWithAReasonRatherThanPaintingNothing )
{
    const auto missing = Desert::Editor::CloudThumbnail::Paint( "no/such/file.dcnv" );
    EXPECT_FALSE( missing.IsSuccess() ) << "a file that does not exist was painted anyway";
    EXPECT_FALSE( missing.GetError().empty() ) << "the refusal carries no reason";

    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );
    const auto wrongKind =
         Desert::Editor::CloudThumbnail::Paint( root + "Editor/Source/Editor/Widgets/CloudThumbnail.cpp" );
    EXPECT_FALSE( wrongKind.IsSuccess() )
         << "a C++ source file was accepted by the cloud painter. The dispatch is on the extension and "
            "an unclaimed one must be refused by name, not fall through to a default.";
}

// ---------------------------------------------------------------------------------------------------
// 6. `ExtensionOf` is the one spelling of "which format is this".
//
// Two of its three cases are mistakes somebody makes once: an upper-case extension from a filesystem
// that preserves case, and a dot inside a DIRECTORY name, which would let a folder decide a file's
// format ("Clouds/v1.2/Layout" has no extension at all).
// ---------------------------------------------------------------------------------------------------
TEST( ThumbnailFormats, ExtensionOfReadsTheFileAndNotItsFolder )
{
    EXPECT_EQ( TF::ExtensionOf( "Materials/Oak.demat" ), "demat" );
    EXPECT_EQ( TF::ExtensionOf( "Materials/Oak.DEMAT" ), "demat" );
    EXPECT_EQ( TF::ExtensionOf( "C:\\Project\\Assets\\Oak.demat" ), "demat" );
    EXPECT_EQ( TF::ExtensionOf( "Clouds/v1.2/Layout" ), "" )
         << "a dot in a directory name was read as the file's extension, so a folder decides a file's "
            "format";
    EXPECT_EQ( TF::ExtensionOf( "README" ), "" );
    EXPECT_EQ( TF::ExtensionOf( "" ), "" );
}

// ---------------------------------------------------------------------------------------------------
// THE SECOND CENSUS, ONE LEVEL DOWN: `.demat` is ONE extension and SIX domains
//
// The table above answers "which FILE TYPES have a producer". It cannot answer the question that put
// three refusals in this repository's startup log, because a material's domain is a property of its
// CONTENT: one `.demat` row, and the mesh path executes exactly one of the six domains it may name.
// Handing it any other is refused by name at MeshRenderer::DrawGenericMeshes — a frame after the
// thumbnail queue has already committed, with the empty frame still written to disk and filed as the
// picture of the material.
//
// So the routing is asserted as a RELATION rather than as a list: ThumbnailSubject::PreviewForDomain
// produces a picture for exactly the domains whose own draw-path predicate says they can be drawn. Both
// directions fail, which is the half that matters — a seventh domain, or a producer added for one of the
// four that have none, must move BOTH sides or this goes red.
// ---------------------------------------------------------------------------------------------------

namespace
{
    // Every enumerator, spelled out because the enum carries no count and a range-for over an enum is not
    // a thing. ShaderDomainName's switch has no default, so a domain ADDED to the enum is a -Wswitch
    // warning in the engine; this array is the second place that has to grow, and the assertion below
    // fails until it does.
    constexpr std::array kAllDomains = {
         Desert::Core::Formats::ShaderDomain::Unspecified, Desert::Core::Formats::ShaderDomain::Surface,
         Desert::Core::Formats::ShaderDomain::Terrain,     Desert::Core::Formats::ShaderDomain::Skybox,
         Desert::Core::Formats::ShaderDomain::PostProcess, Desert::Core::Formats::ShaderDomain::Volume,
    };
} // namespace

TEST( ThumbnailMaterialDomains, APictureExistsForExactlyTheDomainsADrawPathCanExecute )
{
    namespace TS = Desert::Editor::ThumbnailSubject;
    namespace F  = Desert::Core::Formats;

    for ( const F::ShaderDomain domain : kAllDomains )
    {
        const bool drawable = F::DrawnByMeshPath( domain ) || F::DrawnByVolumePath( domain );
        EXPECT_EQ( TS::PreviewForDomain( domain ).has_value(), drawable )
             << "domain " << F::ShaderDomainName( domain )
             << ": the thumbnail router and the draw paths disagree about whether this can be drawn at "
                "all. Either a capture is queued that MeshRenderer will refuse (and its empty frame "
                "written to disk as the material's picture), or a material that CAN be photographed is "
                "being skipped.";
    }
}

TEST( ThumbnailMaterialDomains, EachDrawableDomainGetsThePictureItsOwnPathProduces )
{
    namespace TS = Desert::Editor::ThumbnailSubject;
    namespace F  = Desert::Core::Formats;

    // The mesh path: the ball. Every surface material, a masked one included.
    EXPECT_EQ( TS::PreviewForDomain( F::kMeshPathDomain ), TS::Preview::Sphere );

    // The volume path: the sky the material authors.
    EXPECT_EQ( TS::PreviewForDomain( F::kVolumePathDomain ), TS::Preview::SkyDome );

    // The terrain path has its own renderer and no thumbnail producer. Named here rather than left to the
    // loop above so that adding one is a deliberate edit of this line.
    EXPECT_FALSE( TS::PreviewForDomain( F::kTerrainPathDomain ).has_value() );
}

// A MASKED material goes on the ball, like UE's material thumbnail. The rule this replaces flattened any
// material with AlphaCutoff > 0 onto a camera-facing card, so a grass atlas previewed as a flat rectangle
// and the mask — which the mesh path honours by discard (StaticMeshPBR.shader) — was never seen on a
// shape. The material's cutoff is not an input of the routing at all now: the answer is the surface
// domain's, and the surface domain's answer is the sphere.
TEST( ThumbnailMaterialDomains, AMaskedSurfaceMaterialPreviewsOnTheSphere )
{
    namespace TS = Desert::Editor::ThumbnailSubject;
    namespace F  = Desert::Core::Formats;

    EXPECT_EQ( TS::PreviewForDomain( F::ShaderDomain::Surface ), TS::Preview::Sphere )
         << "a surface material, AlphaCutoff > 0 or not, must preview on the sphere cut by its mask";
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}

// THM1a4: an imported mesh has no `.stmesh` on disk (AF4h), so a picture judged against that path could never be
// fresh - its hash was nullopt and every session re-rendered it. The raw source beside it is the freshness source;
// a hand-authored `.stmesh` on disk is its own.
TEST( ThumbnailFormats, AnImportedMeshPictureIsJudgedAgainstItsSourceAndStaysFreshAcrossSessions )
{
    namespace fs        = std::filesystem;
    namespace TF        = Desert::Editor::ThumbnailFreshness;
    const fs::path  dir = fs::temp_directory_path() / "desert_thm1a5_mesh_freshness";
    std::error_code ec;
    fs::remove_all( dir, ec );
    fs::create_directories( dir, ec );
    const fs::path cooked = dir / "grass.stmesh";
    const fs::path source = dir / "grass.fbx";
    const fs::path png    = dir / "grass.png";
    std::ofstream( source, std::ios::binary ) << "fbx bytes";
    std::ofstream( png, std::ios::binary ) << "png bytes";

    ASSERT_EQ( TF::MeshFreshnessSource( cooked ), source ) << "no .stmesh on disk: the source beside it decides";
    const auto hash = TF::ContentHash( TF::MeshFreshnessSource( cooked ) );
    ASSERT_TRUE( hash.has_value() ) << "an imported mesh's picture could never be recorded as fresh";
    ASSERT_TRUE( TF::Record( png, *hash ).IsSuccess() );
    EXPECT_EQ( TF::Judge( TF::Observe( png, TF::MeshFreshnessSource( cooked ) ) ), TF::Verdict::Show )
         << "the next session re-renders a picture of an unchanged mesh";

    // A hand-authored `.stmesh` on disk is its own freshness source.
    std::ofstream( cooked, std::ios::binary ) << "stmesh bytes";
    EXPECT_EQ( TF::MeshFreshnessSource( cooked ), cooked );
    fs::remove_all( dir, ec );
}

// THM1e: A SURFACE MATERIAL NAMING A PREVIEW MESH IS PHOTOGRAPHED ON IT (UE's ThumbnailInfo); without one, the
// sphere; a Volume material stays the sky whatever it names - the mesh path would refuse it.
TEST( ThumbnailSubject, APreviewMeshRoutesASurfaceMaterialToTheMeshAndNothingElse )
{
    namespace TS = Desert::Editor::ThumbnailSubject;
    namespace F  = Desert::Core::Formats;
    EXPECT_EQ( TS::PreviewForMaterial( F::kMeshPathDomain, true ), TS::Preview::Mesh );
    EXPECT_EQ( TS::PreviewForMaterial( F::kMeshPathDomain, false ), TS::Preview::Sphere );
    EXPECT_EQ( TS::PreviewForMaterial( F::kVolumePathDomain, true ), TS::Preview::SkyDome );
}
