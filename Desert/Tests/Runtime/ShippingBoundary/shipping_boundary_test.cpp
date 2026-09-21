// THE SHIPPING BOUNDARY, PINNED AGAINST THE TREE IT IS DRAWN IN.
//
// WHAT THIS SUITE IS FOR. Until the `Shipping` configuration existed, every development instrument this
// engine had was compiled into the player binary unconditionally: the runtime frame capture (`--shot`),
// the Optick profiler and the GPU timestamp readout, the draw-call counter, the per-frame memory watch
// and the per-asset synchronous-load ledger. FOUR OF THOSE FIVE ARRIVED IN ONE WEEK. That is the fact
// this suite exists for — not the five that are there now, but the sixth, which will be added by
// somebody who has never read `BuildScripts/Configurations.lua` and has no reason to.
//
// "We will remember to cut it" is exactly the class of promise this project catches daily. The precedent
// is `PackagedContentTrees`, written after the packager forgot the fonts and a shipped game contained no
// `.ttf` at all — the first frame with text died on a player's machine. That census does not list what
// travels; it DERIVES it, so a new tree is shipped the day it exists. This one is built the same way
// wherever a relation can be derived, and where it cannot it pins a NAMED ROW rather than a count, so
// the gate cannot be satisfied by editing a number.
//
// THE FIVE RELATIONS, AND WHAT EACH WOULD HAVE CAUGHT:
//
//   1. The configuration exists, is declared once, and is what defines the macro. A `Shipping` that the
//      workspace lists and `Configurations.lua` says nothing about is a configuration in which every
//      instrument is back, silently, because `DESERT_CONFIG_SHIPPING` would simply not be defined.
//
//   2. The boundary macro has exactly one definition, and no site spells the configuration directly.
//      `#ifdef DESERT_CONFIG_SHIPPING`, `#if !defined(...)` and `#ifndef ...` are three spellings of one
//      question; a census can only pin the spelling it can grep for, and the fourth one somebody invents
//      is the one it stops seeing.
//
//   3. Every registered instrument is behind the boundary. One named row per instrument — the count is
//      derived from the rows, so removing a row is a diff somebody has to justify rather than a number
//      somebody can edit.
//
//   4. THE GROWTH GATE. Every PLAYER-SIDE file that includes a registered instrument's header, other
//      than the instrument's own files, must carry the boundary token. This is the one that reddens on
//      "a new call site was added to an existing instrument", which is how four of these five spread
//      through the engine in the first place.
//
//   5. Every profiling macro has a shipping twin, name for name — derived from the header, not listed
//      here. A seventh `DESERT_PROFILE_*` added without a no-op beside it would otherwise keep Optick in
//      the player binary, and nothing else in the tree would say so.
//
// COMMENTS ARE STRIPPED BEFORE ANYTHING IS SEARCHED FOR. A census that shoots at prose gets switched off
// — twice in one week here, once taking a real finding down with it — and this file's own paragraphs
// name every token it forbids.

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    // Walk up from wherever the binary was started, exactly as the other tree-reading suites do, so this
    // need not be run from one precise directory.
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
        std::ifstream in( file.string() );
        if ( !in )
            return {};
        std::ostringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }

    // Line and block comments out, string literals left alone. The point is narrow: every paragraph in
    // this repository quotes the tokens it is about, including the ones it forbids, so a search over raw
    // text finds the explanation and calls it the violation.
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

    size_t CountOf( const std::string& haystack, const std::string& needle )
    {
        size_t n = 0;
        for ( size_t at = haystack.find( needle ); at != std::string::npos;
              at        = haystack.find( needle, at + needle.size() ) )
            ++n;
        return n;
    }

    // THE PLAYER'S OWN SOURCE SET, derived from the three roots the `Runtime` premake compiles and links:
    // its own sources, the engine library and Common. The editor is deliberately NOT here — it is not
    // built in the Shipping configuration at all, so an editor file consuming a development instrument is
    // not a violation of anything and listing editor exceptions would be a register nobody could finish.
    const std::vector<std::string>& PlayerSourceRoots()
    {
        static const std::vector<std::string> roots = { "Desert/Desert/Source", "Desert/Common/Source",
                                                        "Runtime/Source" };
        return roots;
    }

    std::vector<fs::path> PlayerSources( const fs::path& root )
    {
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
        return files;
    }

    constexpr const char* kBoundaryToken = "DESERT_DEV_INSTRUMENTS";

    // ── THE REGISTER ────────────────────────────────────────────────────────────────────────────────
    //
    // ONE ROW PER INSTRUMENT, and the count is derived from the rows below rather than written anywhere.
    // A gate that pins a NUMBER can be satisfied by editing the number; a gate that pins named rows makes
    // a deletion a diff with an author.
    //
    // `Own` is the instrument's own files — the ones that ARE it. Everything else in the player's source
    // set that includes `Header` has to carry the boundary token, which is relation 4.
    // WHERE AN INSTRUMENT'S BOUNDARY IS DRAWN, and the two answers are not interchangeable.
    enum class Gating
    {
        // The `#if` is inside the instrument's own header: its API becomes empty inline no-ops, so every
        // call site compiles unchanged in both configurations and a NEW call site is cut automatically.
        // This is the stronger form and it is chosen wherever it is possible — twice it was also forced,
        // because DrawCounterFunnel and SyncLoadChokepoint are censuses over the CALL SITES' source text
        // and an `#if` there would have read to them as the funnel being removed.
        InItsOwnHeader,
        // The instrument cannot be emptied from its header, because the COST IS AT THE CALL SITE: the
        // memory watch's argument is the device query, the frame capture's whole reason to exist is a
        // command-line flag parsed in `main`. Every player-side consumer must then carry the boundary
        // itself — which is what relation 4 checks, and it is the only form that can grow silently.
        AtEveryCallSite,
    };

    struct Instrument
    {
        const char*              What;   ///< what a person would call it
        const char*              Header; ///< the header other files name it by (matched on basename)
        Gating                   Where;
        std::vector<std::string> Own; ///< the files that are the instrument itself
    };

    const std::vector<Instrument>& Register()
    {
        static const std::vector<Instrument> rows = {
             { "the runtime frame capture (--shot / --shot-frames)",
               "RuntimeShot.hpp",
               Gating::AtEveryCallSite,
               { "Runtime/Source/RuntimeShot.hpp" } },
             { "the draw-call counter",
               "DrawCounters.hpp",
               Gating::InItsOwnHeader,
               { "Desert/Desert/Source/Engine/Graphic/DrawCounters.hpp",
                 "Desert/Desert/Source/Engine/Graphic/DrawCounters.cpp" } },
             { "the memory readout and the per-frame memory watch",
               "MemoryReadout.hpp",
               Gating::AtEveryCallSite,
               { "Desert/Desert/Source/Engine/Graphic/MemoryReadout.hpp",
                 "Desert/Desert/Source/Engine/Graphic/MemoryReadout.cpp",
                 "Desert/Desert/Source/Engine/Graphic/MemoryReadoutSource.cpp" } },
             { "the synchronous-load ledger",
               "SyncLoadLedger.hpp",
               Gating::InItsOwnHeader,
               { "Desert/Desert/Source/Engine/Assets/SyncLoadLedger.hpp" } },
             { "the GPU timestamp profiler",
               "VulkanGpuProfiler.hpp",
               Gating::AtEveryCallSite,
               { "Desert/Desert/Source/Engine/Graphic/API/Vulkan/VulkanGpuProfiler.hpp",
                 "Desert/Desert/Source/Engine/Graphic/API/Vulkan/VulkanGpuProfiler.cpp" } },
        };
        return rows;
    }

    // A file includes a header when some `#include` line of its names that header's BASENAME. Both
    // spellings are live in this tree — `<Engine/Assets/SyncLoadLedger.hpp>` from elsewhere and
    // `"SyncLoadLedger.hpp"` from the same directory — and a census that knew only the first would have
    // walked straight past `AssetBase.hpp`, which is the single hottest consumer of the load ledger.
    bool Includes( const std::string& strippedText, const std::string& headerBasename )
    {
        std::istringstream lines( strippedText );
        std::string        line;
        while ( std::getline( lines, line ) )
        {
            if ( line.find( "#include" ) == std::string::npos )
                continue;
            if ( line.find( headerBasename ) != std::string::npos )
                return true;
        }
        return false;
    }

    // WHAT IS DELIBERATELY NOT CUT. Named here AND in Common/Core/DevInstruments.hpp, and relation 3b
    // asserts the two agree — a reason that lives in only one of the two places is a reason the next
    // reader will not find. Each is a task with an argument, not an oversight.
    const std::vector<std::string>& DeclaredExceptions()
    {
        static const std::vector<std::string> rows = {
             "Engine/Graphic/ResourceLedger.hpp", // the OWNERSHIP mechanism, not only a readout
             "Engine/Graphic/DebugViewState.hpp", // SceneRenderer chooses its render path from it
             "Engine/Graphic/Materials/Debug/MaterialDebugLine.hpp", // and four dev-only pipelines with it
             "Engine/Core/EngineStats.hpp",                          // frame time nobody in a player reads
        };
        return rows;
    }
} // namespace

// ── RELATION 1 ──────────────────────────────────────────────────────────────────────────────────────

TEST( ShippingBoundary, TheWorkspaceDeclaresTheConfigurationAndTheConfigurationDefinesTheMacro )
{
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "could not locate the repository root from the test's cwd";

    const std::string workspace = Read( root / "BuildScripts/Workspace.lua" );
    ASSERT_FALSE( workspace.empty() ) << "BuildScripts/Workspace.lua is missing or empty";
    EXPECT_NE( workspace.find( R"(configurations { "Debug", "Release", "Shipping" })" ), std::string::npos )
         << "the workspace no longer declares a Shipping configuration; every #if in the tree then takes "
            "its development branch and the player gets every instrument back, silently.";

    const std::string configs = Read( root / "BuildScripts/Configurations.lua" );
    ASSERT_FALSE( configs.empty() ) << "BuildScripts/Configurations.lua is missing or empty";
    EXPECT_EQ( CountOf( configs, "filter \"configurations:Shipping\"" ), 1u )
         << "there must be exactly one Shipping block, and it is the one that defines the macro.";
    EXPECT_NE( configs.find( "DESERT_CONFIG_SHIPPING" ), std::string::npos )
         << "the Shipping configuration must define DESERT_CONFIG_SHIPPING, or the boundary is not drawn "
            "at all.";
}

// ── RELATION 2 ──────────────────────────────────────────────────────────────────────────────────────

TEST( ShippingBoundary, TheBoundaryMacroHasExactlyOneDefinitionAndNoSiteSpellsTheConfiguration )
{
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    const fs::path home = root / "Desert/Common/Source/Common/Core/DevInstruments.hpp";
    ASSERT_TRUE( fs::exists( home ) ) << "Common/Core/DevInstruments.hpp is where the boundary lives";

    std::vector<std::string> definers;
    std::vector<std::string> spellers;
    for ( const fs::path& file : PlayerSources( root ) )
    {
        const std::string text = StripComments( Read( file ) );
        const std::string rel  = fs::relative( file, root ).generic_string();

        if ( text.find( std::string( "#define " ) + kBoundaryToken ) != std::string::npos )
            definers.push_back( rel );

        // The configuration's own name may be read ONLY where the boundary macro is derived from it.
        if ( text.find( "DESERT_CONFIG_SHIPPING" ) != std::string::npos &&
             rel != "Desert/Common/Source/Common/Core/DevInstruments.hpp" )
            spellers.push_back( rel );
    }

    ASSERT_EQ( definers.size(), 1u ) << "the boundary macro must be defined in exactly one file";
    EXPECT_EQ( definers.front(), "Desert/Common/Source/Common/Core/DevInstruments.hpp" );

    std::string offenders;
    for ( const std::string& s : spellers )
        offenders += "\n  " + s;
    EXPECT_TRUE( spellers.empty() )
         << "these files ask which CONFIGURATION they are in instead of asking the one question the "
            "census can count ("
         << kBoundaryToken << "):" << offenders;
}

// ── RELATION 3 ──────────────────────────────────────────────────────────────────────────────────────

TEST( ShippingBoundary, EveryRegisteredInstrumentIsBehindTheBoundary )
{
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() );
    ASSERT_FALSE( Register().empty() ) << "an empty register is a census that checks nothing";

    for ( const Instrument& row : Register() )
    {
        ASSERT_FALSE( row.Own.empty() ) << row.What << ": a row with no files cannot be checked";
        bool gated = false;
        for ( const std::string& own : row.Own )
        {
            const fs::path file = root / own;
            ASSERT_TRUE( fs::exists( file ) )
                 << row.What << ": the register names " << own << ", which is not in the tree. Either the "
                 << "instrument moved and this row must move with it, or it is gone and the row must go.";
            if ( StripComments( Read( file ) ).find( kBoundaryToken ) != std::string::npos )
                gated = true;
        }

        // ONLY THE SELF-GATED FORM IS CHECKED HERE, and the first run of this suite is the reason the
        // distinction exists at all: it demanded the token inside MemoryReadout's own files and went red
        // on an instrument that is correctly cut. An `AtEveryCallSite` instrument's own sources are
        // untouched on purpose — they stay compilable, they simply have no caller left in a shipping
        // build, so the archive member is never pulled and the name does not reach the binary (proved by
        // scripts/CI/ShippingSymbols.sh). What has to be true for that row is that EVERY consumer carries
        // the boundary, which is relation 4's whole subject.
        if ( row.Where == Gating::InItsOwnHeader )
        {
            EXPECT_TRUE( gated ) << row.What << ": its boundary is supposed to be in its own header, and "
                                 << "none of its files mentions " << kBoundaryToken << ".";
        }
    }
}

TEST( ShippingBoundary, WhatIsDeliberatelyNotCutIsNamedInBothPlacesAndStillExists )
{
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    // Read WITH comments here, and on purpose: the exception register in DevInstruments.hpp is prose,
    // because its content is a REASON. What is checked is not the prose but the relation — every file the
    // reason names exists, and every row this suite knows about is named there too. A reason that drifts
    // from the tree is how "we also meant to remove those" becomes folklore.
    const std::string home = Read( root / "Desert/Common/Source/Common/Core/DevInstruments.hpp" );
    ASSERT_FALSE( home.empty() );

    for ( const std::string& named : DeclaredExceptions() )
    {
        EXPECT_NE( home.find( named ), std::string::npos )
             << named << " is an exception this suite knows about and DevInstruments.hpp does not name. "
             << "The reason has to travel with the mechanism.";

        const fs::path file = root / "Desert/Desert/Source" / named;
        EXPECT_TRUE( fs::exists( file ) )
             << named << " is named as a deliberate exception but no longer exists — the exception "
             << "outlived the thing it excepted.";
    }
}

// ── RELATION 4: THE GROWTH GATE ─────────────────────────────────────────────────────────────────────

TEST( ShippingBoundary, EveryPlayerSideConsumerOfAnInstrumentCarriesTheBoundary )
{
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    std::string offenders;
    std::string stale;
    size_t      consumersSeen = 0;

    for ( const Instrument& row : Register() )
    {
        std::set<std::string> own( row.Own.begin(), row.Own.end() );
        size_t                rowConsumers = 0;

        for ( const fs::path& file : PlayerSources( root ) )
        {
            const std::string rel = fs::relative( file, root ).generic_string();
            if ( own.count( rel ) != 0 )
                continue;

            const std::string text = StripComments( Read( file ) );
            if ( !Includes( text, row.Header ) )
                continue;

            ++consumersSeen;
            ++rowConsumers;
            // An instrument gated in its own header has nothing to ask of its consumers — that is the
            // whole advantage of that form, and demanding the token here would forbid it.
            if ( row.Where == Gating::AtEveryCallSite && text.find( kBoundaryToken ) == std::string::npos )
            {
                offenders += "\n  " + rel + "  includes " + row.Header + "  (" + row.What + ")";
            }
        }

        // A ROW WITH NO CONSUMER IS A ROW THAT CHECKS NOTHING, and it would sit green for ever: an
        // instrument that was renamed, moved or deleted leaves its register row behind, and from then on
        // this relation walks past whatever replaced it. Per row, not in total — one live instrument
        // would otherwise cover for four dead rows.
        if ( rowConsumers == 0 )
            stale += std::string( "\n  " ) + row.What + "  (header " + row.Header + ")";
    }

    // A POSITIVE CONTROL, because the loop above passes trivially if it finds nothing: a typo in a header
    // path, a root that moved, a `#include` spelled some third way — each of those turns this test into
    // an assertion about the empty set, which is precisely the silent green this project keeps finding.
    EXPECT_GT( consumersSeen, 0u )
         << "no consumer of any registered instrument was found anywhere in the player's source set. "
            "That is not plausible; the search is broken, not the tree.";

    EXPECT_TRUE( stale.empty() )
         << "these registered instruments have no consumer anywhere in the player's source set, so their "
            "rows check nothing. Either the instrument is gone and the row must go with it, or its header "
            "was renamed and the row did not follow:"
         << stale;

    EXPECT_TRUE( offenders.empty() )
         << "these player-side files use a development instrument with no boundary around the use, so "
            "the instrument is in the shipping binary:"
         << offenders << "\n\nPut the use behind `#if " << kBoundaryToken
         << "`, or move the gate into the instrument's own header where its call sites need not change.";
}

// ── RELATION 5: DERIVED, NOT LISTED ─────────────────────────────────────────────────────────────────

TEST( ShippingBoundary, EveryProfilingMacroHasAShippingTwinNameForName )
{
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    const std::string profiler = StripComments( Read( root / "Desert/Common/Source/Common/Core/Profiler.hpp" ) );
    ASSERT_FALSE( profiler.empty() );

    const std::string guard = std::string( "#if !" ) + kBoundaryToken;
    const size_t      open  = profiler.find( guard );
    ASSERT_NE( open, std::string::npos )
         << "Profiler.hpp no longer has a shipping branch — every DESERT_PROFILE_* then expands to an "
            "Optick event and a timer in the player.";
    const size_t split = profiler.find( "\n#else", open );
    ASSERT_NE( split, std::string::npos ) << "the shipping branch has no #else half to compare against";

    auto macroNames = [&]( const std::string& region )
    {
        std::set<std::string> names;
        const std::string     key = "#define DESERT_PROFILE_";
        for ( size_t at = region.find( key ); at != std::string::npos; at = region.find( key, at + key.size() ) )
        {
            const size_t nameStart = at + std::string( "#define " ).size();
            size_t       nameEnd   = nameStart;
            while ( nameEnd < region.size() &&
                    ( std::isalnum( static_cast<unsigned char>( region[nameEnd] ) ) || region[nameEnd] == '_' ) )
                ++nameEnd;
            names.insert( region.substr( nameStart, nameEnd - nameStart ) );
        }
        return names;
    };

    const std::set<std::string> shipping    = macroNames( profiler.substr( open, split - open ) );
    const std::set<std::string> development = macroNames( profiler.substr( split ) );

    ASSERT_FALSE( development.empty() ) << "no DESERT_PROFILE_* macro was found at all; the parse is wrong";

    std::string missing;
    for ( const std::string& name : development )
        if ( shipping.count( name ) == 0 )
            missing += "\n  " + name;

    EXPECT_TRUE( missing.empty() )
         << "these profiling macros have no no-op twin in the Shipping branch, so a shipping build still "
            "pays for them and still drags Optick into the binary:"
         << missing;

    std::string stale;
    for ( const std::string& name : shipping )
        if ( development.count( name ) == 0 )
            stale += "\n  " + name;
    EXPECT_TRUE( stale.empty() ) << "these macros exist only in the Shipping branch — a no-op with nothing "
                                    "to be a no-op of:"
                                 << stale;
}

// ── THE PACKAGER'S DEFAULT IS A CONFIGURATION THAT EXISTS ───────────────────────────────────────────

TEST( ShippingBoundary, ThePackagerDefaultsToAConfigurationTheWorkspaceDeclares )
{
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    const std::string packager = StripComments( Read( root / "Editor/Source/Editor/Packaging/GamePackager.hpp" ) );
    ASSERT_FALSE( packager.empty() );

    EXPECT_NE( packager.find( "kPackageConfigs" ), std::string::npos )
         << "the configurations a package can be cut from must be ONE list the panel and the packager "
            "share; two literals in two files is how the panel kept offering two configurations after a "
            "third existed.";
    EXPECT_NE( packager.find( R"("Shipping")" ), std::string::npos )
         << "the packager cannot offer the only configuration without the development instruments in it.";
    EXPECT_NE( packager.find( R"(Config = "Shipping")" ), std::string::npos )
         << "the DEFAULT matters more here than anywhere else in the editor: this is the one button whose "
            "product a stranger runs.";

    const std::string workspace = Read( root / "BuildScripts/Workspace.lua" );
    EXPECT_NE( workspace.find( R"("Shipping")" ), std::string::npos )
         << "the packager offers a configuration the workspace does not declare — packaging it can only "
            "ever fail with 'Runtime binary not found'.";
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
