// "EVERY SLOT THAT SHOWS A RENDERED THUMBNAIL ALSO ASKS FOR ONE."
//
// THE DEFECT, EXACTLY. `StaticMeshComponentWidget::DrawMeshThumbnail` decoded the PNG at
// `ThumbnailCache::DiskPath(...)` and queued nothing at all. If the asset browser had never walked past
// that mesh, the file was never written, and the row showed a grey cube glyph for the rest of the
// project's life with nothing anywhere saying why. That is the same wart `ThumbnailService`'s own header
// declares REMOVED — "the Details material slot deliberately did no rendering at all ... A material could
// sit there as a coloured square forever" — a guarantee stated in prose that one of the two slots under
// it did not keep. Prose is not a gate; this file is.
//
// WHY IT IS A CENSUS AND NOT A TEST OF ONE SLOT. There are five places in this editor that draw a
// rendered thumbnail, they were written months apart, and four of them get it right. Checking the one
// that was wrong would have gone green the day it was fixed and said nothing ever again; the sixth slot,
// written next year, is the one this has to catch. So the question is asked of EVERY drawing site, the
// list of sites is DERIVED from the tree rather than typed, and a site that appears without an answer is
// a failure rather than a silence.
//
// WHY IT READS SOURCES. Every one of these functions needs ImGui, a Vulkan device and a live
// AssetManager to call; none of the five files is compiled by any suite in this repository. Reading them
// is the established alternative here — MaterialPreviewRoute next door pins the preview's route the same
// way, and for the same reason — and the text machinery is the ONE shared reader,
// Tests/Engine/SettingConsumers/setting_consumers_reader.hpp (Д33), never a sixth private copy of it.
//
// THE UNIT IS A FUNCTION, NOT A FILE, and that is load-bearing. "FileExplorerPanel.cpp mentions
// RequestMaterial somewhere" is the vacuous form of this question: that file has two independent drawing
// sites and a capture-from-viewport writer, so a file-level answer would let the mesh grid lose its
// request while the material grid kept the file green. This is the У3 lesson (a census that cannot see a
// setting die certifies nothing) applied one level down.
//
// WHAT WOULD MAKE THIS RED, and each is a real mistake:
//   * a drawing site that decodes a cached thumbnail and never queues one (the defect above);
//   * a new panel that grows a thumbnail row and reads the disk cache directly;
//   * somebody replacing a `ThumbnailService::Get().Request*` with a bare `ThumbnailCache::DiskPath`
//     because "the browser fills it anyway";
//   * a site deleted or renamed without this census being re-pointed — the row names the function, so it
//     cannot go quietly green over a function that no longer exists.

#include "../../Engine/SettingConsumers/setting_consumers_reader.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <utility>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    namespace fs = std::filesystem;
    namespace CT = Desert::Tests::ConsumerText;

    // ------------------------------------------------------------------------------------------------
    // The census
    // ------------------------------------------------------------------------------------------------

    enum class Role
    {
        // Draws a cached rendered thumbnail. MUST queue one through the shared service.
        Shows,
        // Reaches for the cache path for some other reason entirely (deleting a stale picture, writing a
        // new one). MUST NOT decode one, or it is a Shows row wearing the wrong label.
        Handles,
        // Draws a picture the site next to it in the same file already queued, and therefore can never be
        // the first to look. An exception, spelled out, with the argument for why it is one.
        Rereads,

        // Decodes a file that IS ALREADY THE PICTURE — a .png, .tga, .hdr the browser shows directly —
        // rather than a rendered thumbnail. It owes no request because there is nothing to render: the
        // asset is its own preview. Editor/Widgets/ThumbnailFormats.hpp calls the same thing
        // Producer::Decoded, and these rows are the sites that consume it.
        //
        // ADDED BY M11, AND FOUND BY THE TEST THAT ADDED IT. The function-level discovery below reported
        // FileExplorerPanel::DrawTextureThumbnail on its first run — a decoding site with no row, correct
        // and unclassified for as long as this census has existed, because the file-level sweep only ever
        // asked whether its FILE was listed.
        DecodesSource
    };

    struct Site
    {
        const char* File;
        const char* Function; ///< the function this is a statement about, not the file
        Role        What;
        const char* Request; ///< the entry point a Shows row must reach; empty otherwise
        const char* Why;     ///< printed on failure, so a red row explains itself
    };

    // EVERY PLACE IN THE EDITOR THAT TOUCHES A RENDERED-THUMBNAIL PATH, with what it is for. The list is
    // checked against the tree below (`TheCensusNamesEveryFileThatTouchesTheThumbnailCache`), so it can
    // neither shrink behind a deletion nor stay silent about an addition.
    constexpr Site kSites[] = {
         { "Editor/Source/Editor/Panels/SceneProperties/ComponentWidgets/StaticMeshComponent.cpp",
           "StaticMeshComponentWidget::DrawMeshThumbnail", Role::Shows, "RequestMesh",
           "the 3D Model row's mesh slot. THIS is the row the census was written for: it read the disk "
           "cache and queued nothing, so a mesh the asset browser had never shown had no picture here, "
           "ever, and no message said so" },

         { "Editor/Source/Editor/Panels/SceneProperties/ComponentWidgets/MaterialsPanelComponent.cpp",
           "MaterialComponentWidget::DrawSlotPreview", Role::Shows, "RequestMaterial",
           "the Details material slot — the row ThumbnailService was built for, and the one the mesh slot "
           "beside it was supposed to have been copying" },

         { "Editor/Source/Editor/Panels/FileExplorer/FileExplorerPanel.cpp",
           "FileExplorerPanel::DrawRenderedMaterialThumbnail", Role::Shows, "RequestMaterial",
           "the asset browser's material grid" },

         { "Editor/Source/Editor/Panels/FileExplorer/FileExplorerPanel.cpp",
           "FileExplorerPanel::DrawRenderedMeshThumbnail", Role::Shows, "RequestMesh",
           "the asset browser's mesh grid" },

         { "Editor/Source/Editor/Panels/Collections/CollectionsPanel.cpp", "CollectionsPanel::DrawCard",
           Role::Shows, "RequestMesh", "the Collections card grid" },

         { "Editor/Source/Editor/Panels/FileExplorer/FileExplorerPanel.cpp",
           "FileExplorerPanel::DrawPaintedThumbnail", Role::Shows, "RequestPainted",
           "the asset browser's tile for the four CLOUD formats, whose picture is PAINTED from the "
           "file's own bytes on a JobSystem worker rather than photographed by the renderer. It owes a "
           "request for exactly the same reason the two grids above it do — the difference between a "
           "capture and a paint is which queue it lands in, not whether the slot has to ask" },

         { "Editor/Source/Editor/Panels/FileExplorer/FileExplorerPanel.cpp",
           "FileExplorerPanel::DrawTextureThumbnail", Role::DecodesSource, "",
           "the asset browser's texture tile. It decodes the IMAGE FILE ITSELF — not a cached render — "
           "so there is nothing for it to queue: spending an offscreen capture here would produce a "
           "picture of the picture we started with. It is deliberately independent of the cook pipeline "
           "so that EVERY image previews, not only the already-cooked ones" },

         { "Editor/Source/Editor/Panels/FileExplorer/FileExplorerPanel.cpp",
           "FileExplorerPanel::EmitAssetDragSource", Role::Rereads, "",
           "the drag ghost. It shows the picture of the tile being dragged, and a tile cannot be dragged "
           "without having been drawn — DrawRenderedMaterialThumbnail / DrawRenderedMeshThumbnail queued "
           "it one call earlier, in this same file, this same frame. It is a re-read of a decided "
           "question, not an independent slot, and when the picture is missing it falls back to the type "
           "icon rather than to silence" },

         { "Editor/Source/Editor/Panels/MaterialEditor/MaterialEditorPanel.cpp",
           "MaterialEditorPanel::SaveSubject", Role::Handles, "",
           "DELETES this material's cached picture after a save, so every panel showing it re-renders. It "
           "draws nothing, and it pairs the removal with ThumbnailService::Invalidate — the write side of "
           "the same service the Shows rows read from" },

         { "Editor/Source/Editor/Panels/FileExplorer/FileExplorerPanel.cpp",
           "FileExplorerPanel::CaptureThumbnailFromViewport", Role::Handles, "",
           "WRITES a thumbnail from the viewport readback, under the same key the grid reads. A producer, "
           "not a consumer: its output is what the Shows rows would otherwise have had to queue" },
    };

    // ------------------------------------------------------------------------------------------------
    // Reading the tree
    // ------------------------------------------------------------------------------------------------

    // THE MECHANISM ITSELF — excluded from the discovery sweep by path, with the reason written down
    // beside each, because "not a slot" is a claim like any other. A row asserting that
    // ThumbnailService.cpp mentions ThumbnailService would be a tautology; a silent skip list would be a
    // place to hide a real slot, which is how the verify skill's own suite list lost three suites for a
    // whole programme.
    struct Mechanism
    {
        const char* Path;
        const char* Why;
    };

    constexpr Mechanism kMechanism[] = {
         { "Editor/Source/Editor/Widgets/ThumbnailService.cpp", "the queue every slot asks through" },
         { "Editor/Source/EditorLayer.cpp",
           "drives the service — one Tick() per frame and one Shutdown() at teardown. It owns no row and "
           "draws no picture; it is the clock, not a consumer" },

         // ThumbnailCache.cpp WAS excused here as "where DiskPath is DEFINED" and no longer is: M11 moved
         // the cache-path rule to Editor/Widgets/ThumbnailKey.hpp, beside the rule that names the file,
         // because ThumbnailCache.cpp cannot be linked without a renderer and the sweep's decision had to
         // become testable. The exemption went with the definition rather than being left to match
         // nothing — a skip list that matches nothing is a place to hide a real slot later.
    };

    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            std::ifstream probe( prefix + "Editor/Source/Editor/Widgets/ThumbnailService.hpp" );
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

    // Comments and literals blanked through the ONE shared reader. It matters here more than usual:
    // every file in this census discusses thumbnails at length in prose, several of them naming the very
    // calls asserted below, and a scanner that could not tell code from commentary would certify a site
    // for talking about its own defect.
    std::string CodeOf( const std::string& root, const std::string& relative )
    {
        return CT::StripCommentsAndLiterals( ReadFile( root + relative ) );
    }

    // The balanced body of one function definition, `{` to the matching `}`. Empty when the signature is
    // not in this file at all — which the callers treat as a failure, not as a pass.
    std::string FunctionBody( const std::string& code, const std::string& signature )
    {
        const std::size_t sig = code.find( signature );
        if ( sig == std::string::npos )
            return {};
        const std::size_t open = code.find( '{', sig );
        if ( open == std::string::npos )
            return {};

        int         depth = 0;
        std::size_t i     = open;
        for ( ; i < code.size(); ++i )
        {
            if ( code[i] == '{' )
                ++depth;
            else if ( code[i] == '}' && --depth == 0 )
                break;
        }
        return code.substr( open, i - open );
    }

    // Does this body DECODE a cached picture — i.e. call `Get(` on something whose name says it is a
    // thumbnail cache? Every one of the five drawing sites spells it one of these ways
    // (`m_Thumbnails->Get`, `m_Thumbs->Get`, `s_Thumbnails.Get`), because ThumbnailCache::Get is the only
    // way a PNG becomes an image in this editor.
    //
    // Derived from the receiver's NAME rather than from a list of spellings, so a sixth panel that calls
    // its member `m_AssetThumbs` is seen without this file being edited.
    bool DecodesAThumbnail( const std::string& body )
    {
        for ( std::size_t at : CT::WordPositions( body, "Get" ) )
        {
            std::size_t after = CT::SkipSpace( body, at + 3 );
            if ( after >= body.size() || body[after] != '(' )
                continue;

            // Walk back over `.` / `->` to the receiver identifier.
            std::size_t before = at;
            if ( before >= 2 && body[before - 1] == '.' )
                before -= 1;
            else if ( before >= 3 && body[before - 1] == '>' && body[before - 2] == '-' )
                before -= 2;
            else
                continue;

            std::size_t start = before;
            while ( start > 0 && CT::IsIdentChar( body[start - 1] ) )
                --start;
            const std::string receiver = body.substr( start, before - start );
            if ( receiver.find( "Thumb" ) != std::string::npos )
                return true;
        }
        return false;
    }

    // `ThumbnailService::Get().<entry>(` — the whole chain, not the bare name. A row that only checked
    // for `RequestMesh` would be satisfied by AssetThumbnailRenderer's own method of that name, which is
    // the SECOND route the service exists to prevent anyone from taking.
    bool QueuesThrough( const std::string& body, const std::string& entry )
    {
        for ( std::size_t at : CT::WordPositions( body, "ThumbnailService" ) )
        {
            std::size_t i = CT::SkipSpace( body, at + 16 );
            if ( i + 1 >= body.size() || body[i] != ':' || body[i + 1] != ':' )
                continue;
            i = CT::SkipSpace( body, i + 2 );
            if ( !CT::WordAt( body, i, "Get" ) )
                continue;
            i = CT::SkipSpace( body, i + 3 );
            if ( i >= body.size() || body[i] != '(' )
                continue;
            i = CT::SkipSpace( body, i + 1 );
            if ( i >= body.size() || body[i] != ')' )
                continue;
            i = CT::SkipSpace( body, i + 1 );
            if ( i >= body.size() || body[i] != '.' )
                continue;
            i = CT::SkipSpace( body, i + 1 );
            if ( !CT::WordAt( body, i, entry ) )
                continue;
            i = CT::SkipSpace( body, i + entry.size() );
            if ( i < body.size() && body[i] == '(' )
                return true;
        }
        return false;
    }

    // Every editor source that names a rendered-thumbnail path, either way it can be obtained: through
    // the cache's own key function, or as the return of a service request.
    std::set<std::string> FilesTouchingTheCache( const std::string& root )
    {
        std::set<std::string> out;
        std::error_code       ec;
        const fs::path        tree = fs::path( root ) / "Editor" / "Source";

        for ( fs::recursive_directory_iterator it( tree, ec ), end; it != end && !ec; it.increment( ec ) )
        {
            if ( !it->is_regular_file( ec ) )
                continue;
            const std::string ext = it->path().extension().string();
            if ( ext != ".cpp" && ext != ".hpp" )
                continue;

            // `ThumbnailKey::DiskPath` RATHER THAN `ThumbnailCache::DiskPath`, because M11 moved the
            // cache-path rule out of ThumbnailCache.cpp: that translation unit includes
            // Engine/Graphic/Image.hpp and so cannot be linked without a renderer, and the background
            // sweep — which decides what has no picture — had to be drivable by a test.
            //
            // `ThumbnailService::`, WITH the scope operator, and never the bare name: an
            // `#include <Editor/Widgets/ThumbnailService.hpp>` is not a literal and so survives the
            // stripper, and a panel that merely includes the header is not touching the cache. Measured:
            // CollectionsPanel.hpp includes it and does nothing else with it.
            const std::string code = CT::StripCommentsAndLiterals( ReadFile( it->path().string() ) );
            if ( code.find( "ThumbnailKey::DiskPath" ) == std::string::npos &&
                 code.find( "ThumbnailService::" ) == std::string::npos )
                continue;

            // Relative to the repository root, forward slashes, so it can be compared with the table.
            const std::string rel = fs::relative( it->path(), fs::path( root ), ec ).generic_string();
            out.insert( rel );
        }
        return out;
    }

    // ------------------------------------------------------------------------------------------------
    // Every `Class::Function` DEFINED in one file, with its body.
    //
    // M11 ADDED THIS AND THE REASON IS ITS OWN NEAR MISS. The discovery sweep below finds new FILES; it
    // cannot find a new FUNCTION inside a file that is already listed — and this census's own header says
    // the unit is a function, not a file. `FileExplorerPanel::DrawPaintedThumbnail` (the cloud tile) was
    // exactly that case: a sixth drawing site, in a file with four rows already, decoding a cached
    // picture. Every test here would have stayed green over it.
    //
    // So the file-level question is now asked one level down: whatever DECODES a thumbnail in a censused
    // file must have a row of its own.
    std::vector<std::pair<std::string, std::string>> DefinitionsIn( const std::string& code )
    {
        std::vector<std::pair<std::string, std::string>> out;

        for ( std::size_t at = code.find( "::" ); at != std::string::npos; at = code.find( "::", at + 2 ) )
        {
            // The qualifier behind the `::` and the name in front of the `(`.
            std::size_t qualifierStart = at;
            while ( qualifierStart > 0 && CT::IsIdentChar( code[qualifierStart - 1] ) )
                --qualifierStart;
            const std::string qualifier = code.substr( qualifierStart, at - qualifierStart );
            const std::string name      = CT::IdentAt( code, at + 2 );
            if ( qualifier.empty() || name.empty() )
                continue;

            std::size_t i = CT::SkipSpace( code, at + 2 + name.size() );
            if ( i >= code.size() || code[i] != '(' )
                continue;

            // Walk to the matching `)`, then past a trailing `const`/`noexcept`. A DEFINITION is the one
            // followed by `{`; a call and a declaration are not.
            int depth = 0;
            for ( ; i < code.size(); ++i )
            {
                if ( code[i] == '(' )
                    ++depth;
                else if ( code[i] == ')' && --depth == 0 )
                {
                    ++i;
                    break;
                }
            }
            i = CT::SkipSpace( code, i );
            while ( CT::WordAt( code, i, "const" ) || CT::WordAt( code, i, "noexcept" ) )
                i = CT::SkipSpace( code, i + ( code[i] == 'c' ? 5u : 8u ) );
            if ( i >= code.size() || code[i] != '{' )
                continue;

            const std::size_t open = i;
            depth                  = 0;
            for ( ; i < code.size(); ++i )
            {
                if ( code[i] == '{' )
                    ++depth;
                else if ( code[i] == '}' && --depth == 0 )
                    break;
            }
            out.emplace_back( qualifier + "::" + name, code.substr( open, i - open ) );
        }
        return out;
    }

    bool IsMechanism( const std::string& relative )
    {
        for ( const Mechanism& m : kMechanism )
            if ( relative == m.Path )
                return true;
        return false;
    }
} // namespace

// ---------------------------------------------------------------------------------------------------
// 0. The census is not empty, and every row points at code that exists.
//
// The failure this guards is the one a census dies of: a rename leaves every row unmatched, every
// assertion below is made of an empty string, and the suite reports a clean sweep of nothing.
// ---------------------------------------------------------------------------------------------------
TEST( ThumbnailRequesters, EveryRowPointsAtAFunctionThatExists )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "repository root not found from the test's working directory";

    ASSERT_GE( std::size( kSites ), 5u ) << "the census has shrunk below the sites known to exist";

    for ( const Site& site : kSites )
    {
        const std::string code = CodeOf( root, site.File );
        ASSERT_FALSE( code.empty() ) << "could not read " << site.File;

        EXPECT_FALSE( FunctionBody( code, site.Function ).empty() )
             << site.Function << " is not in " << site.File
             << " any more. Re-point this row at wherever that drawing site went — do not delete it. What "
                "it is: "
             << site.Why;
    }
}

// ---------------------------------------------------------------------------------------------------
// 1. THE CENSUS ITSELF: a slot that SHOWS a rendered thumbnail QUEUES one.
// ---------------------------------------------------------------------------------------------------
TEST( ThumbnailRequesters, EverySlotThatShowsAThumbnailAlsoRequestsIt )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    int shown = 0;
    for ( const Site& site : kSites )
    {
        if ( site.What != Role::Shows )
            continue;
        ++shown;

        const std::string body = FunctionBody( CodeOf( root, site.File ), site.Function );
        ASSERT_FALSE( body.empty() ) << site.Function << " not found — see the row-integrity test above";

        // The positive half of the row's own label: it really is a drawing site. Without this the
        // assertion below could be satisfied by a function that neither shows nor needs a thumbnail.
        EXPECT_TRUE( DecodesAThumbnail( body ) )
             << site.Function
             << " no longer decodes a cached thumbnail at all. If this slot stopped "
                "showing one, take its row out and say why; if it moved, re-point the row. What it is: "
             << site.Why;

        EXPECT_TRUE( QueuesThrough( body, site.Request ) )
             << site.Function << " draws a cached thumbnail and never asks for one.\n"
             << "  It reads a picture that only exists if some OTHER panel happened to walk past this "
                "asset first. When none has, the slot shows a placeholder forever and nothing anywhere "
                "says why — which is exactly the defect ThumbnailService was built to end, and which its "
                "header already claims to have ended.\n"
             << "  Call ThumbnailService::Get()." << site.Request
             << "( handle, assetPath ) and draw the path it returns. Do not add a second route: the "
                "service owns the one renderer in the editor, deduplicates across panels, keeps the "
                "picture across restarts and never retries an asset that failed.\n"
             << "  This slot is: " << site.Why;
    }

    EXPECT_GE( shown, 5 ) << "fewer showing slots than this census was written against — a silent "
                             "shrinkage is the one thing a per-row report cannot say on its own";
}

// ---------------------------------------------------------------------------------------------------
// 2. The other direction: a row labelled as NOT a drawing site must not quietly become one.
//
// Both non-showing labels are claims about what the code does NOT do, and a claim like that rots the
// moment somebody adds four lines. `SaveSubject` growing a preview, or the viewport capture starting to
// draw what it wrote, would each turn a Handles row into an unasserted Shows row.
// ---------------------------------------------------------------------------------------------------
TEST( ThumbnailRequesters, ASiteLabelledAsNotDrawingDoesNotDraw )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    for ( const Site& site : kSites )
    {
        if ( site.What != Role::Handles )
            continue;

        const std::string body = FunctionBody( CodeOf( root, site.File ), site.Function );
        ASSERT_FALSE( body.empty() ) << site.Function << " not found — see the row-integrity test above";

        EXPECT_FALSE( DecodesAThumbnail( body ) )
             << site.Function
             << " now decodes a cached thumbnail, so it is a SHOWING slot and owes a "
                "request like every other one. Change its row to Role::Shows, name the entry point it "
                "must reach, and make it reach it. Its old job was: "
             << site.Why;
    }
}

// ---------------------------------------------------------------------------------------------------
// 3. The exception is exactly one, and it stays paid for.
//
// `EmitAssetDragSource` is excused because the tiles beside it, IN THE SAME FILE, queue the picture it
// re-reads. That argument is only true while those two functions are still there and still requesting —
// so the exception is asserted against them rather than granted on trust. If the browser ever stops
// queueing, the drag ghost stops being a re-read and becomes a slot with no request, and this says so.
// ---------------------------------------------------------------------------------------------------
TEST( ThumbnailRequesters, TheOnlyExceptionIsBackedByTheSiteThatQueuesForIt )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    int exceptions = 0;
    for ( const Site& site : kSites )
    {
        if ( site.What != Role::Rereads )
            continue;
        ++exceptions;

        const std::string code = CodeOf( root, site.File );

        // Whoever re-reads must live in a file that DOES queue — both kinds, because the ghost is drawn
        // for materials and for meshes alike.
        bool queuesSomewhere = false;
        for ( const Site& other : kSites )
        {
            if ( other.What != Role::Shows || std::string( other.File ) != site.File )
                continue;
            queuesSomewhere =
                 queuesSomewhere || QueuesThrough( FunctionBody( code, other.Function ), other.Request );
        }

        EXPECT_TRUE( queuesSomewhere )
             << site.Function
             << " is excused from requesting because a drawing site in the same file queues the picture "
                "it re-reads — and no such site does any more. The excuse has expired: either restore "
                "the request next door or make this site ask for its own. What it is: "
             << site.Why;
    }

    EXPECT_EQ( exceptions, 1 ) << "the number of sites excused from asking has changed. One is a "
                                  "documented re-read; two is a habit. Read the new row's argument and "
                                  "decide, then update this number deliberately.";
}

// ---------------------------------------------------------------------------------------------------
// 3b. WHO OWNS THE PICTURES — a decision, not a label.
//
// Every thumbnail image in this editor is created on one line of ThumbnailCache::Get, and it is claimed
// there as ResourceOwner::EditorTool (Engine/Graphic/ResourceLedger.hpp). The alternative that would look
// plausible to a future reader is AssetService, because a thumbnail is a picture OF an asset — and it is
// the one label that must never be used here: AssetService is the ONLY category asset eviction may
// release, on the grounds that the asset's file is the recipe and re-reading it is cheap. A thumbnail's
// recipe is a 370 ms offscreen RENDER. Mis-filing it would hand eviction a lever that turns a memory
// reclaim into a re-capture, silently.
//
// Asserted here rather than in the ledger's own suite because it is a statement about THIS subsystem's
// place in that taxonomy, and because a test that needs a Vulkan device to observe the row cannot be
// written at all — nothing in the editor prints the ledger in production yet.
// ---------------------------------------------------------------------------------------------------
TEST( ThumbnailRequesters, ThumbnailImagesAreClaimedAsEditorToolAndNeverAsAssetService )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    const std::string body =
         FunctionBody( CodeOf( root, "Editor/Source/Editor/Widgets/ThumbnailCache.cpp" ), "ThumbnailCache::Get" );
    ASSERT_FALSE( body.empty() ) << "ThumbnailCache::Get not found — it is the ONE place a thumbnail image "
                                    "is created, which is why one claim there covers every panel. If it "
                                    "moved, re-point this test at wherever the creation went.";

    EXPECT_NE( body.find( "ResourceAttributionScope" ), std::string::npos )
         << "the thumbnail images are no longer claimed in the GPU resource ledger, so every one of them "
            "counts as Unclaimed — the figure that ledger exists to make trustworthy, and the one "
            "device-loss recovery has to plan against.";
    EXPECT_NE( body.find( "ResourceOwner::EditorTool" ), std::string::npos )
         << "the claim is no longer EditorTool. That is the category for the editor's own pictures.";
    EXPECT_EQ( body.find( "ResourceOwner::AssetService" ), std::string::npos )
         << "thumbnails are claimed as AssetService — the one owner asset eviction is allowed to release. "
            "Eviction would then drop a picture whose 'recipe' is a 370 ms render rather than a file read, "
            "and rebuild it by re-rendering, quietly, whenever memory got tight.";
}

// ---------------------------------------------------------------------------------------------------
// 4. DISCOVERY: the table is derived from the tree, not typed and hoped over.
//
// This is what makes the census a census. Without it the four tests above are statements about five
// files somebody once knew about, and the sixth panel — the one written next year, by someone who has
// never read ThumbnailService.hpp — is invisible to all of them.
// ---------------------------------------------------------------------------------------------------
TEST( ThumbnailRequesters, TheCensusNamesEveryFileThatTouchesTheThumbnailCache )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    const std::set<std::string> found = FilesTouchingTheCache( root );
    ASSERT_FALSE( found.empty() ) << "the sweep of Editor/Source found nothing at all — the walk is "
                                     "broken, not the tree";

    std::set<std::string> census;
    for ( const Site& site : kSites )
        census.insert( site.File );

    for ( const std::string& file : found )
    {
        if ( IsMechanism( file ) )
            continue;
        EXPECT_TRUE( census.count( file ) != 0 )
             << file
             << " reaches for a rendered-thumbnail path and is in no row of this census.\n"
                "  Add a row naming the FUNCTION and what it is for: Role::Shows if it draws a cached "
                "picture (it then owes a ThumbnailService request), Role::Handles if it writes or deletes "
                "one, Role::Rereads only with an argument for why it can never be the first to look.\n"
                "  Deciding which of the three it is IS the point of this test.";
    }

    // And the other way, so a deleted panel cannot leave a row asserting nothing.
    for ( const std::string& file : census )
    {
        EXPECT_TRUE( found.count( file ) != 0 )
             << file
             << " is a row of this census but no longer touches the thumbnail cache at all. "
                "Remove the row deliberately rather than leaving it to pass on an empty string.";
    }

    // The skip list has to be paid for too. An entry that no longer matches anything is a licence
    // somebody could later move a real slot under without a word — the exact way the verify skill's own
    // suite list came to hide three suites for a whole programme.
    for ( const Mechanism& m : kMechanism )
    {
        EXPECT_TRUE( found.count( m.Path ) != 0 )
             << m.Path << " is excused from this census as mechanism (" << m.Why
             << ") and no longer touches the thumbnail cache. Drop the exemption: a skip that matches "
                "nothing is a place to hide something later.";
    }
}

// ---------------------------------------------------------------------------------------------------
// 4b. AND THE DISCOVERY GOES ONE LEVEL DOWN: a new drawing FUNCTION in an already-censused file.
//
// The sweep above finds new FILES. It is blind to a sixth drawing site added to a file that already has
// four rows — and this census's own header says the unit is a function rather than a file, precisely
// because FileExplorerPanel.cpp holds several independent sites. M11 added one (the cloud tile) and every
// test here would have stayed green over it, which is what this closes.
// ---------------------------------------------------------------------------------------------------
TEST( ThumbnailRequesters, NoCensusedFileHidesAnUndeclaredDrawingSite )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    std::set<std::string> censused;
    for ( const Site& site : kSites )
        censused.insert( site.Function );

    std::set<std::string> files;
    for ( const Site& site : kSites )
        files.insert( site.File );

    int drawing = 0;
    for ( const std::string& file : files )
    {
        const std::string code = CodeOf( root, file );
        ASSERT_FALSE( code.empty() ) << "could not read " << file;

        for ( const auto& [name, body] : DefinitionsIn( code ) )
        {
            if ( !DecodesAThumbnail( body ) )
                continue;
            ++drawing;

            EXPECT_TRUE( censused.count( name ) != 0 )
                 << name << " (in " << file
                 << ") decodes a cached thumbnail and has no row in this census.\n"
                    "  It is a drawing site like any other and owes a ThumbnailService request. Add a row: "
                    "Role::Shows with the entry point it must reach, or Role::Rereads with an argument for "
                    "why it can never be the first to look.";
        }
    }

    EXPECT_GE( drawing, 5 ) << "only " << drawing
                            << " drawing sites were FOUND by reading the censused files, which is fewer "
                               "than the census claims exist — the definition scanner has stopped seeing "
                               "them, and a scanner that finds nothing certifies nothing";
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
