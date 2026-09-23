// "EVERY FORMAT THE CONTENT BROWSER CAN SHOW EITHER HAS A THUMBNAIL PRODUCER OR A NAMED REASON WHY NOT."
//
// THE MEASUREMENT THIS IS BUILT ON. `ThumbnailService` knew exactly TWO formats — Material and Mesh. The
// four cloud formats (`.dclayout`, `.dcnv`, `.dcmv`, `.decloudtype`) had no producer at all, and until
// M11 the Content Browser did not even TYPE them: they fell through to FileType::Unknown, so four
// different assets drew one identical grey document glyph and the owner could not pick a cloud by
// looking at it. Nothing was broken. Nothing said anything either — which is the "empty successful
// answer" §1.4 of the contract forbids, wearing an icon instead of a return value.
//
// SO THE QUESTION IS ASKED OF EVERY FORMAT, AND THE LIST OF FORMATS IS DERIVED FROM THE TREE. A census
// whose subject list is typed here would go green the day the four cloud rows were added and say nothing
// ever again; the SEVENTH format, added next year by somebody who has never read ThumbnailFormats.hpp,
// is the one this has to catch. `s_FileTypes` in FileExplorerPanel.cpp is the browser's own answer to
// "which extensions do I show", so that literal is what this suite reads.
//
// AND A ROW IS NOT TAKEN ON TRUST. Every `Producer::Painted` row is PAINTED HERE, against the shipped
// asset library, and the result is asserted to be a picture rather than a flat square — because a
// producer that returns success and fills one colour is exactly the failure this subsystem keeps
// meeting, and it is invisible to any test that only checks the return value.
//
// WHAT WOULD MAKE THIS RED, and each is a real mistake:
//   * a format added to the browser with nobody deciding whether it has a picture;
//   * a `Producer::None` row whose reason is a deferral ("not yet", "TODO") rather than an argument —
//     that is a TODO wearing a table row, which §1.1 forbids outright;
//   * a painted producer that stops producing, or starts producing a uniform square;
//   * a row pointing at an extension the browser no longer shows.

#include <Editor/Widgets/CloudThumbnail.hpp>
#include <Editor/Widgets/ThumbnailFormats.hpp>
#include <Editor/Widgets/ThumbnailSubject.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    namespace fs = std::filesystem;
    namespace TF = Desert::Editor::ThumbnailFormats;

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

    /**
     * @brief The extensions the Content Browser types, read out of its own `s_FileTypes` initialiser.
     *
     * THE LITERALS ARE THE DATA HERE, which is why this does NOT go through the shared
     * StripCommentsAndLiterals reader that the other censuses use. That reader blanks string literals so
     * a file cannot certify a setting by mentioning it in a log line — sound everywhere else and exactly
     * wrong here, because the thing being read IS a table of string literals. The initialiser is bounded
     * first, so a `{ "x", FileType::Y }` pair written in a comment somewhere else in the file cannot get
     * in.
     */
    std::set<std::string> BrowserExtensions( const std::string& root )
    {
        const std::string code = ReadFile( root + kBrowserTable );
        if ( code.empty() )
            return {};

        const std::size_t begin = code.find( "s_FileTypes = {" );
        if ( begin == std::string::npos )
            return {};
        const std::size_t end = code.find( "};", begin );
        if ( end == std::string::npos )
            return {};

        const std::string table = code.substr( begin, end - begin );

        std::set<std::string> out;
        // A CUSTOM RAW-STRING DELIMITER, because the pattern itself contains `)"` — a capture group
        // closing just before a quote — and the default `R"( ... )"` would end the literal there.
        const std::regex pattern( R"re(\{\s*"([A-Za-z0-9_]+)"\s*,\s*FileType::)re" );
        for ( auto it = std::sregex_iterator( table.begin(), table.end(), pattern ); it != std::sregex_iterator();
              ++it )
            out.insert( ( *it )[1].str() );
        return out;
    }

    /// Every file under the shipped asset tree with this extension.
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
// 0. The table itself is well formed.
//
// The failure this guards is the one a census dies of quietly: a duplicate row, so `Find` answers with
// whichever came first and the second row is dead text nobody can see is dead.
// ---------------------------------------------------------------------------------------------------
TEST( ThumbnailFormats, EveryRowIsWellFormedAndUnique )
{
    ASSERT_GE( TF::kFormatCount, 30u ) << "the census has shrunk far below the formats known to exist";

    std::set<std::string_view> seen;
    for ( const TF::Format& format : TF::kFormats )
    {
        EXPECT_FALSE( format.Extension.empty() ) << "a row has no extension";
        EXPECT_EQ( format.Extension.find( '.' ), std::string_view::npos )
             << "row '" << format.Extension
             << "' spells its extension with a dot. The browser's own table is keyed without one, and two "
                "spellings of an extension are two sets that cannot be compared.";
        EXPECT_EQ( Lowered( format.Extension ), std::string( format.Extension ) )
             << "row '" << format.Extension << "' is not lower case; ExtensionOf lowers what it is given";
        EXPECT_FALSE( format.What.empty() )
             << "row '" << format.Extension
             << "' says nothing about what its picture is. Every row is a statement somebody has to be "
                "able to disagree with.";
        EXPECT_TRUE( seen.insert( format.Extension ).second )
             << "'" << format.Extension
             << "' appears twice — Find() answers with the first and the "
                "second row is text nobody can tell is dead";
    }
}

// ---------------------------------------------------------------------------------------------------
// 1. A REFUSAL IS AN ARGUMENT, NOT A DEFERRAL.
//
// `Producer::None` means "a picture would be the wrong answer for this format". It must never mean "not
// built yet" — that is a TODO in a table row, and the contract's §1.1 does not care what shape a TODO
// arrives in. The wording check is coarse on purpose: it cannot judge an argument, but it can catch the
// four words somebody reaches for when they have not made one.
// ---------------------------------------------------------------------------------------------------
TEST( ThumbnailFormats, EveryRefusalCarriesAReasonRatherThanADeferral )
{
    int refusals = 0;
    for ( const TF::Format& format : TF::kFormats )
    {
        if ( format.By != TF::Producer::None )
            continue;
        ++refusals;

        std::string why = Lowered( format.What );

        // A ROW MAY CITE ANOTHER ROW'S ARGUMENT INSTEAD OF REPEATING IT — five shader stages share one
        // reason, and five copies of it would be five places to edit and four to forget. What the
        // citation may NOT be is a dangling pointer, so it is FOLLOWED: the row it names must exist, must
        // itself be a refusal (a refusal cannot rest on a row that HAS a picture), and must carry the
        // argument. That turns "same as .shader" from a shorter answer into an asserted relation.
        if ( const std::size_t cite = why.find( "same as ." ); cite != std::string::npos )
        {
            std::string target;
            for ( std::size_t i = cite + 9; i < why.size() && std::isalnum( static_cast<unsigned char>( why[i] ) );
                  ++i )
                target += why[i];

            const TF::Format* cited = TF::Find( target );
            ASSERT_NE( cited, nullptr )
                 << "'" << format.Extension << "' cites '." << target << "' for its reason and no such row exists";
            EXPECT_EQ( cited->By, TF::Producer::None )
                 << "'" << format.Extension << "' is refused a picture because '." << target << "' is — and '."
                 << target
                 << "' HAS a producer. A citation that points at a row with a picture is an argument that "
                    "says the opposite of what it is being used for.";
            why = Lowered( cited->What );
        }

        EXPECT_GT( why.size(), 40u )
             << "'" << format.Extension
             << "' is refused a picture in a few words. The reason is what a future reader has to argue "
                "against before adding one, so it has to BE an argument.";

        for ( const char* deferral :
              { "todo", "not yet", "not built", "for now", "later", "unimplemented", "fixme", "hack" } )
        {
            EXPECT_EQ( why.find( deferral ), std::string::npos )
                 << "'" << format.Extension << "' is refused a picture because of '" << deferral
                 << "'. That is a deferral, not a reason — a TODO wearing a table row (contract §1.1). "
                    "Either build the producer or state why a picture would be the WRONG answer for this "
                    "format.";
        }
    }
    EXPECT_GT( refusals, 0 ) << "no format is refused a picture at all, which would be a surprise: a .lua "
                                "and a .wav have no useful 64-pixel square between them";
}

// ---------------------------------------------------------------------------------------------------
// 2. THE CENSUS AND THE BROWSER AGREE, IN BOTH DIRECTIONS.
//
// This is what makes it a census. Without it the tests above are statements about a list somebody once
// wrote, and the format added next year is invisible to all of them.
// ---------------------------------------------------------------------------------------------------
TEST( ThumbnailFormats, EveryExtensionTheBrowserShowsHasARowAndEveryRowIsShown )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "repository root not found from the test's working directory";

    const std::set<std::string> browser = BrowserExtensions( root );
    ASSERT_FALSE( browser.empty() ) << "the browser's s_FileTypes table could not be read from " << kBrowserTable
                                    << " — the parse is broken, not the tree";
    ASSERT_GE( browser.size(), 30u ) << "only " << browser.size()
                                     << " extensions were parsed out of the browser's table; the "
                                        "initialiser's shape must have changed under this regex";

    for ( const std::string& extension : browser )
    {
        EXPECT_NE( TF::Find( extension ), nullptr )
             << "the Content Browser shows '." << extension
             << "' and Editor/Widgets/ThumbnailFormats.hpp has no row for it.\n"
                "  Add one, and the row IS the decision: name the producer (Decoded if the file is "
                "already a picture, RenderedMaterial/RenderedMesh if it needs the offscreen renderer, "
                "Painted if its bytes can be turned into a picture on the CPU, Authored if only a person "
                "can frame it) — or Producer::None with a written argument for why a picture would be the "
                "wrong answer.\n"
                "  What must NOT happen is what happened to the four cloud formats: a new format arriving "
                "with nobody asking the question, and four different assets drawing one grey glyph for a "
                "year with nothing anywhere saying why.";
    }

    for ( const TF::Format& format : TF::kFormats )
    {
        EXPECT_TRUE( browser.count( std::string( format.Extension ) ) != 0 )
             << "'." << format.Extension
             << "' has a row in the census and the Content Browser does not show it any more. Remove the "
                "row deliberately rather than leaving it to pass on nothing.";
    }
}

// ---------------------------------------------------------------------------------------------------
// 2b. THE ROW NAMES A PRODUCER; THE PANEL MUST ACTUALLY ROUTE THE FILE TO IT.
//
// THIS IS THE HALF THAT WAS MISSING, AND IT WAS MISSING WHILE THE HEADER SAID "THE ROW IS A CLAIM AND
// THE CLAIM IS CHECKED". What was checked was the extension SET; what the row mostly says — who makes
// the picture — was checked by nobody. Measured on 2026-09-23: `.hdr` had carried
// `Producer::Decoded` since the census was written, and the browser routed only `FileType::Texture` to
// `DrawTextureThumbnail`. An `.hdr` is `FileType::Cubemap`, so it reached no draw function at all and
// showed its type icon — a row asserting a producer the tree did not honour, which is the shape this
// project has now closed a dozen instances of.
//
// The mapping is derived, never typed: extension -> FileType out of the browser's own `s_FileTypes`,
// FileType -> draw function out of the browser's own routing lines. Both come from one file, so a
// renamed function or a re-routed type moves the test with the code instead of leaving it behind.
// ---------------------------------------------------------------------------------------------------
TEST( ThumbnailFormats, EveryRowsProducerIsTheDrawFunctionTheBrowserActuallyCalls )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    const std::string panel = ReadFile( root + kBrowserTable );
    ASSERT_FALSE( panel.empty() ) << kBrowserTable << " could not be read";

    // Which draw function each producer is: the one line of this test that is a decision rather than a
    // derivation. `Authored` and `None` route to nothing on purpose — a level's picture is captured by a
    // person and a `.lua` has none — so they are not checked for a call.
    struct Route
    {
        TF::Producer By;
        const char*  Function;
    };
    const Route routes[] = {
         { TF::Producer::Decoded, "DrawTextureThumbnail" },
         { TF::Producer::RenderedMaterial, "DrawRenderedMaterialThumbnail" },
         { TF::Producer::RenderedMesh, "DrawRenderedMeshThumbnail" },
         { TF::Producer::Painted, "DrawPaintedThumbnail" },
    };

    // extension -> FileType, from the browser's own table.
    std::map<std::string, std::string> typeOf;
    {
        const std::size_t begin = panel.find( "s_FileTypes = {" );
        ASSERT_NE( begin, std::string::npos );
        const std::size_t end = panel.find( "};", begin );
        ASSERT_NE( end, std::string::npos );
        const std::string table = panel.substr( begin, end - begin );

        const std::regex pattern( R"re(\{\s*"([A-Za-z0-9_]+)"\s*,\s*FileType::([A-Za-z0-9_]+))re" );
        for ( auto it = std::sregex_iterator( table.begin(), table.end(), pattern ); it != std::sregex_iterator();
              ++it )
            typeOf[( *it )[1].str()] = ( *it )[2].str();
    }
    ASSERT_GE( typeOf.size(), 30u );

    // FileType -> the draw functions the panel guards on it. Read out of the routing lines rather than
    // listed here, so the two grid layouts (tile and detail row) are both covered by construction.
    std::map<std::string, std::set<std::string>> drawnBy;
    {
        // A LOOKAHEAD, not a consuming match, and the difference is a defect this test found in itself:
        // `( A || B ) && DrawX(` is one routing line naming TWO types, and a consuming pattern swallows
        // the second type on its way to the function name — so B silently reads as "routed nowhere".
        // Zero-width means the iterator resumes just after each type name and sees every one of them.
        const std::regex pattern(
             R"re(entry->Type == FileType::([A-Za-z0-9_]+)(?=[^;]{0,200}?(Draw[A-Za-z]*Thumbnail)\())re" );
        for ( auto it = std::sregex_iterator( panel.begin(), panel.end(), pattern ); it != std::sregex_iterator();
              ++it )
            drawnBy[( *it )[1].str()].insert( ( *it )[2].str() );
    }
    ASSERT_FALSE( drawnBy.empty() ) << "no `entry->Type == FileType::X ... DrawYThumbnail(` routing was "
                                       "parsed; the panel's shape changed under this regex";

    for ( const TF::Format& format : TF::kFormats )
    {
        const char* wanted = nullptr;
        for ( const Route& route : routes )
        {
            if ( route.By == format.By )
                wanted = route.Function;
        }
        if ( wanted == nullptr )
            continue; // Authored and None have no automatic draw, by decision

        const auto type = typeOf.find( std::string( format.Extension ) );
        ASSERT_NE( type, typeOf.end() ) << '.' << format.Extension << " has no FileType in the browser";

        const auto drawn = drawnBy.find( type->second );
        ASSERT_NE( drawn, drawnBy.end() )
             << '.' << format.Extension << " is FileType::" << type->second
             << ", which the Content Browser routes to NO draw function — so the row's promise of \""
             << format.What << "\" reaches no pixel and the file shows its type icon.";

        EXPECT_TRUE( drawn->second.count( wanted ) != 0 )
             << '.' << format.Extension << " is FileType::" << type->second << ", whose row promises "
             << wanted << ", but the browser routes that type to "
             << *drawn->second.begin() << " instead.";
    }
}

// ---------------------------------------------------------------------------------------------------
// 3. THE FOUR CLOUD FORMATS, BY NAME.
//
// Pinned separately from the derived check above because they are the owner's requirement rather than a
// property of the mechanism: "владелец не может выбрать облака картинкой". A generic census would stay
// green if all four rows became Producer::None with well-argued reasons — which is a legal state of the
// mechanism and a broken state of the feature.
// ---------------------------------------------------------------------------------------------------
TEST( ThumbnailFormats, EveryCloudFormatHasAPicture )
{
    for ( const char* extension : { "dclayout", "dcnv", "dcmv", "decloudtype" } )
    {
        const TF::Format* format = TF::Find( extension );
        ASSERT_NE( format, nullptr ) << "'." << extension << "' has no row at all";
        EXPECT_EQ( format->By, TF::Producer::Painted )
             << "'." << extension
             << "' no longer has a picture. This is the requirement M11 exists for: the owner picks a "
                "cloud by looking at it, and before this task all four formats drew one identical grey "
                "document glyph.";
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
    for ( const TF::Format& format : TF::kFormats )
    {
        if ( format.By != TF::Producer::Painted )
            continue;

        const std::string           extension = std::string( format.Extension );
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
                "Producer::None row wearing a Painted label.";
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
        EXPECT_EQ( TS::PreviewForDomain( domain, false ).has_value(), drawable )
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

    // The mesh path: a ball, or the camera-facing card a cutout needs. The cutout choice is INSIDE the
    // mesh path and nowhere else — a medium has no alpha-tested silhouette to flatten.
    EXPECT_EQ( TS::PreviewForDomain( F::kMeshPathDomain, false ), TS::Preview::Sphere );
    EXPECT_EQ( TS::PreviewForDomain( F::kMeshPathDomain, true ), TS::Preview::Card );

    // The volume path: the sky the material authors. The cutout flag must not reach it.
    EXPECT_EQ( TS::PreviewForDomain( F::kVolumePathDomain, false ), TS::Preview::SkyDome );
    EXPECT_EQ( TS::PreviewForDomain( F::kVolumePathDomain, true ), TS::Preview::SkyDome );

    // The terrain path has its own renderer and no thumbnail producer. Named here rather than left to the
    // loop above so that adding one is a deliberate edit of this line.
    EXPECT_FALSE( TS::PreviewForDomain( F::kTerrainPathDomain, false ).has_value() );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
