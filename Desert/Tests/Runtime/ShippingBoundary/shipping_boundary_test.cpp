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
// THE SIX RELATIONS, AND WHAT EACH WOULD HAVE CAUGHT:
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
//   6. THE WIRING. Relations 1-5 and scripts/CI/ShippingSymbols.sh both ran in exactly one place for a
//      day after the configuration landed: a developer's laptop, by hand. This row pins that Windows
//      compiles the configuration, that macOS runs the symbol census, that the census still carries a
//      positive control, and that its two ends agree where the binary is. A gate nobody runs is the same
//      promise as "we will remember to cut it".
//
// Four further rows sit beside them and are not relations between instruments, so they are not
// numbered: every project the shipping configuration BUILDS must also select its third-party libraries
// in it (Editor/premake5.lua did not, and linked nothing at all); the packager's default configuration
// must be one the workspace declares; BOTH platform
// build wrappers must accept every configuration it declares (derived from Workspace.lua — Windows
// refused the word `Shipping` outright on the platform the game ships for); and the test projects must
// stay OUT of the Shipping configuration — a suite whose subject is the draw-call counter cannot be
// built in the configuration that removes the draw-call counter, and the cheapest way to make it build
// would be to weaken the test.
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
        const std::ifstream in( file.string() );
        if ( !in )
            return {};
        std::ostringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }

    // The body of scripts/MacOS/BuildMacOS.sh's argument `case ... esac`, with shell comments removed.
    // The comments have to go for the reason this whole file gives: that block's own prose names the
    // configurations it is about.
    std::string CaseBlockOf( const std::string& script )
    {
        const size_t at = script.find( "case \"$arg\" in" );
        if ( at == std::string::npos )
            return {};
        const size_t end = script.find( "esac", at );
        if ( end == std::string::npos )
            return {};

        std::string out;
        bool        comment = false;
        for ( size_t i = at; i < end; ++i )
        {
            const char c = script[i];
            if ( c == '\n' )
                comment = false;
            else if ( c == '#' )
                comment = true;
            if ( !comment )
                out.push_back( c );
        }
        return out;
    }

    // The contents of a job block's `config: [ ... ]` matrix line, or an empty string if it has none.
    // Narrow on purpose: this is the one line in ci.yml a census needs to read as DATA rather than as
    // prose, and parsing exactly it beats pulling a YAML library into a suite that compiles no engine.
    std::string MatrixConfigs( const std::string& jobBlock )
    {
        const size_t at = jobBlock.find( "config: [" );
        if ( at == std::string::npos )
            return {};
        const size_t open  = at + std::string( "config: [" ).size();
        const size_t close = jobBlock.find( ']', open );
        if ( close == std::string::npos )
            return {};
        return jobBlock.substr( open, close - open );
    }

    // YAML line comments out. Deliberately narrower than StripComments below and used only on
    // .github/workflows/ci.yml: a `#` there is always a comment (no anchor, tag or colour literal in
    // the file), so "cut at the first # that starts a token" is the whole rule. The reason it is needed
    // at all is the same one the header gives — ci.yml's prose names the configuration, the script and
    // the sentence it used to carry, so raw text would match the explanation instead of the wiring.
    std::string StripYamlComments( const std::string& source )
    {
        std::string out;
        out.reserve( source.size() );
        bool comment = false;
        char prev    = '\n';
        for ( const char c : source )
        {
            if ( c == '\n' )
            {
                comment = false;
                out.push_back( c );
            }
            else if ( !comment && c == '#' && ( prev == '\n' || prev == ' ' || prev == '\t' ) )
            {
                comment = true;
            }
            else if ( !comment )
            {
                out.push_back( c );
            }
            prev = c;
        }
        return out;
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
             "Engine/Core/EngineStats.hpp",       // frame time nobody in a player reads
             // MaterialDebugLine.hpp STOOD HERE and was removed by В12, which is the one direction a row
             // may leave this list: the exception was retired by cutting the thing it excepted. The four
             // pipelines it carried (DebugLinePipeline, StaticMeshWireframe, OverdrawPipeline,
             // OverdrawResolvePipeline) are now behind the boundary inside MeshRenderer, the register
             // that keeps them there is Desert/Tests/Runtime/ShippingPipelines, and the linked-artifact
             // half is the six mangled rows in scripts/CI/ShippingSymbols.sh. The HEADER still exists —
             // the editor's collider pass uses it, and the editor is not what this boundary is about.
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
        const std::set<std::string> own( row.Own.begin(), row.Own.end() );
        size_t                      rowConsumers = 0;

        for ( const fs::path& file : PlayerSources( root ) )
        {
            const std::string rel = fs::relative( file, root ).generic_string();
            if ( own.contains( rel ) )
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
            while (
                 nameEnd < region.size() &&
                 ( std::isalnum( static_cast<unsigned char>( region[nameEnd] ) ) != 0 || region[nameEnd] == '_' ) )
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
        if ( !shipping.contains( name ) )
            missing += "\n  " + name;

    EXPECT_TRUE( missing.empty() )
         << "these profiling macros have no no-op twin in the Shipping branch, so a shipping build still "
            "pays for them and still drags Optick into the binary:"
         << missing;

    std::string stale;
    for ( const std::string& name : shipping )
        if ( !development.contains( name ) )
            stale += "\n  " + name;
    EXPECT_TRUE( stale.empty() ) << "these macros exist only in the Shipping branch — a no-op with nothing "
                                    "to be a no-op of:"
                                 << stale;
}

// ── RELATION 6: THE BOUNDARY IS GUARDED BY A MACHINE, NOT BY THIS MACHINE ───────────────────────────
//
// Relations 1-5 and `scripts/CI/ShippingSymbols.sh` are two halves of one proof, and for one day after
// the configuration landed BOTH of them ran in exactly one place: a developer's laptop, by hand. A gate
// that only a person remembers to run is the same promise as "we will remember to cut it", which is the
// promise this whole suite exists because nobody keeps.
//
// So the wiring itself is a relation. The rows below are NAMED, never counted — a count can be satisfied
// by editing the count. Each one is a sentence somebody would have to delete on purpose:
//
//   * Windows compiles the shipping configuration. Windows is the platform the game ships for and the
//     one that diverges most from the machine this file was written on; the configuration had been
//     compiled exactly once, on macOS, when this row was added.
//   * macOS runs the symbol census, and the census still refuses to speak without a positive control.
//     Dropping the control turns "the instruments are gone" and "I could not read the binary" into the
//     same green, on the one gate whose entire job is to say what is present.
//   * The two ends of that census agree about WHERE the binary is. The script defaults to a path; the
//     workspace decides the path. A middle link silently dropping that agreement is this project's most
//     frequent defect shape, and here it degrades to the script's exit 2 rather than a false pass —
//     which is survivable, and still worth naming before it happens.
//
// YAML COMMENTS ARE STRIPPED FIRST, for the reason the header of this file gives: ci.yml explains at
// length what it forbids and what it used to say, so a search over its raw text finds the explanation
// and calls it the wiring.
TEST( ShippingBoundary, TheBoundaryIsCheckedByCIAndNotOnlyByHand )
{
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    const std::string ci = StripYamlComments( Read( root / ".github/workflows/ci.yml" ) );
    ASSERT_FALSE( ci.empty() ) << ".github/workflows/ci.yml is missing or empty";

    // The job blocks, so a row can say WHICH platform carries it. Two-space indent is the job level.
    const size_t macosAt   = ci.find( "\n  macos:" );
    const size_t windowsAt = ci.find( "\n  windows:" );
    ASSERT_NE( macosAt, std::string::npos ) << "ci.yml no longer has a macos job";
    ASSERT_NE( windowsAt, std::string::npos ) << "ci.yml no longer has a windows job";
    ASSERT_LT( macosAt, windowsAt ) << "the macos job must precede the windows job for this census to "
                                       "split them; if they were reordered, fix this line, not the file.";
    const std::string macosJob   = ci.substr( macosAt, windowsAt - macosAt );
    const std::string windowsJob = ci.substr( windowsAt );

    // THE MATRIX LINE AND NOT THE JOB, because the job text contains the word either way: the guard on
    // the test step is spelled `matrix.config != 'Shipping'` and would keep a bare find() green on a job
    // that had stopped building the configuration entirely.
    const std::string windowsMatrix = MatrixConfigs( windowsJob );
    EXPECT_NE( windowsMatrix.find( "Shipping" ), std::string::npos )
         << "the Windows job's matrix is [" << windowsMatrix
         << "] — it no longer builds the Shipping configuration. Windows is the platform the game ships "
            "for, and MSVC had never compiled this configuration at all until that leg existed: "
            "`runtime \"Release\"` / `optimize \"Full\"` / `symbols \"Off\"` being valid MSVC settings is "
            "not the same statement as `it compiles`.";

    const std::string macosMatrix = MatrixConfigs( macosJob );
    EXPECT_NE( macosMatrix.find( "Shipping" ), std::string::npos )
         << "the macOS job's matrix is [" << macosMatrix
         << "] — without a Shipping build there is no linked binary for the symbol census below to read, "
            "and it would answer exit 2 on every push.";

    EXPECT_NE( macosJob.find( "scripts/CI/ShippingSymbols.sh" ), std::string::npos )
         << "nothing in CI reads the linked shipping binary any more. The source census you are reading "
            "cannot see an archive member pulled in by a reference nobody wrote down; that is the half "
            "the symbol dump owns, and it now runs nowhere.";

    const std::string symbols = Read( root / "scripts/CI/ShippingSymbols.sh" );
    ASSERT_FALSE( symbols.empty() ) << "scripts/CI/ShippingSymbols.sh is missing — the CI step above "
                                       "would fail, but this says why before it does";
    EXPECT_NE( symbols.find( "CONTROL=" ), std::string::npos )
         << "the symbol census lost its positive control. `nm` on a stripped or unreadable binary prints "
            "nothing, and nothing passes every absence check ever written: without a symbol that MUST be "
            "found, a green from that script is indistinguishable from a binary it failed to read.";

    // WHERE THE BINARY IS, ASSERTED AS A RELATION AND NOT AS A STRING IN TWO PLACES. The workspace owns
    // the directory; the script defaults to a path inside it. `%{cfg.buildcfg}` is `Shipping` in that
    // configuration, and `Runtime` is the project name.
    const std::string workspace = Read( root / "BuildScripts/Workspace.lua" );
    ASSERT_FALSE( workspace.empty() );
    EXPECT_NE( workspace.find( "/build/Bin/%{cfg.buildcfg}" ), std::string::npos )
         << "the workspace's targetdir moved. scripts/CI/ShippingSymbols.sh defaults to "
            "build/Bin/Shipping/Runtime and will answer exit 2 (`the check could not run`) rather than a "
            "false pass — but it will answer it on every push until somebody reads this message.";
    EXPECT_NE( symbols.find( "build/Bin/Shipping/Runtime" ), std::string::npos )
         << "the symbol census no longer defaults to the path the workspace builds into, and the CI step "
            "passes it no argument.";
}

// ── THE TEST SUITES ARE NOT IN THE SHIPPING CONFIGURATION, AND THAT IS LOAD-BEARING ─────────────────
//
// A suite whose subject IS one of the instruments — DrawCounterFunnel, MemoryDetector,
// GpuTimestampLayout, SyncLoadChokepoint — cannot be built in the one configuration that removes what it
// is about. The cheapest way to make it build would be to weaken the test, which is the wrong direction
// for a gate to push. So they are removed from the configuration instead, and ci.yml's two "Run tests"
// steps are guarded to match. This row exists because those are THREE places that have to agree; when
// they stop agreeing the CI leg goes red on a missing directory, which is survivable and still a whole
// Windows leg spent to say what this line says in a second.
TEST( ShippingBoundary, TheTestProjectsAreRemovedFromTheShippingConfiguration )
{
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    const std::string tests = Read( root / "Desert/Tests/premake5.lua" );
    ASSERT_FALSE( tests.empty() );
    EXPECT_GE( CountOf( tests, "removeconfigurations { \"Shipping\" }" ), 3u )
         << "the test projects are back in the Shipping configuration: the per-suite loop, BuildAllTests "
            "and RunAllTests each need the removal, and the last of those is what writes run_tests.bat. "
            "With them back, every suite that tests an instrument has to compile without it.";

    const std::string ci = StripYamlComments( Read( root / ".github/workflows/ci.yml" ) );
    ASSERT_FALSE( ci.empty() );
    EXPECT_GE( CountOf( ci, "if: matrix.config != 'Shipping'" ), 2u )
         << "both `Run tests` steps must be guarded — the Shipping build creates no "
            "build/Bin/Tests/Shipping at all, so those steps have nothing to run. Unguarded they go red "
            "on a missing directory and a missing run_tests.bat, which is a whole CI leg spent saying so.";
}

// ── EVERY PROJECT THAT SHIPS LINKS ITS LIBRARIES IN THE SHIPPING CONFIGURATION ──────────────────────
//
// WHAT THIS COST, MEASURED RATHER THAN IMAGINED. The `Shipping` configuration arrived with the
// `filter "configurations:Release or Shipping"` arm taught to Desert/Desert/premake5.lua,
// Runtime/premake5.lua and exactly ONE of the 258 test scripts — and not to Editor/premake5.lua,
// whose arm still read `filter "configurations:Release"`. A premake filter that matches no configuration
// contributes nothing and says nothing, so the Editor project in `Shipping` linked against NO third-party library
// at all. `make config=shipping` — which is what `scripts/MacOS/BuildMacOS.sh Shipping` runs, and what the
// packager's own "Runtime binary not found" message sends people to — died on the whole of Vulkan, the
// whole of shaderc and the whole of spirv-cross.
//
// AND NOTHING IN THE SOURCES COULD HAVE POINTED HERE: the entire tree COMPILED. Only the link failed,
// on the one project the shipping configuration had never been asked to produce. This is the shape the
// project files it under "a middle link drops a property" — both ends look right, the link in between
// loses something — and the answer is the same: assert the RELATION.
//
// DERIVED, NOT LISTED. Every premake5.lua outside Desert/Tests/ that selects a per-configuration library
// set must name Shipping on the filter that governs the Release set. The test scripts are excluded from
// the scan for the same reason they are excluded from the configuration (see
// TheTestProjectsAreRemovedFromTheShippingConfiguration): a filter naming a configuration its project
// does not have is inert, so requiring it there would be noise rather than a check.
TEST( ShippingBoundary, EveryShippedProjectSelectsItsLibrariesInTheShippingConfiguration )
{
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    std::vector<std::string> offenders;
    std::size_t              scanned = 0;

    for ( const fs::directory_entry& entry : fs::recursive_directory_iterator( root ) )
    {
        if ( !entry.is_regular_file() || entry.path().filename() != "premake5.lua" )
            continue;

        const std::string rel = fs::relative( entry.path(), root ).generic_string();
        if ( rel.find( "ThirdParty/" ) != std::string::npos || rel.starts_with( "build/" ) ||
             rel.starts_with( "Desert/Tests/" ) )
            continue;

        const std::string text = Read( entry.path() );

        // EVERY occurrence, not the first — and that distinction is not hypothetical. Editor/premake5.lua
        // selects a Release library set TWICE: once from its own (currently empty) list and once, the one
        // that carries Vulkan on macOS, from the engine's. The first draft of this loop stopped at the
        // first hit, passed on a tree whose Editor still could not link, and had to be caught by building
        // it. A census that examines one of two identical constructs is a census that reports on half a
        // file.
        for ( size_t use = text.find( "Libraries.Release" ); use != std::string::npos;
              use        = text.find( "Libraries.Release", use + 1 ) )
        {
            ++scanned;

            // The filter in force at that line is the nearest one ABOVE it. Both spellings count:
            // `filter "configurations:..."` and the table form `filter { "system:...", "configurations:..." }`.
            const size_t governing = text.rfind( "configurations:", use );
            if ( governing == std::string::npos )
            {
                offenders.push_back( rel + " (no configuration filter governs a Release library set)" );
                continue;
            }
            const size_t eol = text.find( '\n', governing );
            if ( text.substr( governing, eol - governing ).find( "Shipping" ) == std::string::npos )
                offenders.push_back( rel + "  (the arm at offset " + std::to_string( governing ) + ")" );
        }
    }

    ASSERT_GT( scanned, 0u ) << "this census found no project scripts to read — it is checking nothing";

    std::string report;
    for ( const std::string& o : offenders )
        report += "\n  " + o;
    EXPECT_TRUE( offenders.empty() )
         << "these projects select their third-party libraries for Release and NOT for Shipping, so in "
            "the shipping configuration they link nothing and fail at the linker with the whole of "
            "Vulkan undefined:"
         << report;
}

// ── BOTH BUILD WRAPPERS ACCEPT EVERY CONFIGURATION THE WORKSPACE DECLARES ───────────────────────────
//
// DERIVED FROM Workspace.lua, NOT LISTED HERE. A fourth configuration added tomorrow is covered the day
// it exists, which is the shape PackagedContentTrees established and the reason this file prefers it.
//
// WHAT IT CAUGHT THE DAY IT WAS WRITTEN. `Shipping` landed with scripts/MacOS/BuildMacOS.sh taught the
// word and scripts/Windows/BuildWindows.bat not, so on the platform the game actually ships for the
// documented wrapper answered "[ERROR] Unknown argument: Shipping" and exited 1. The packager's own
// "Runtime binary not found" message is what sends a person to that wrapper, so the two dead ends were
// in series: the editor told you to run a command that refused to run.
//
// A configuration the workspace declares but no wrapper will build is a configuration that gets built
// by nobody, and a configuration built by nobody rots in exactly the way this suite exists to prevent.
TEST( ShippingBoundary, BothPlatformWrappersAcceptEveryDeclaredConfiguration )
{
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    const std::string workspace = Read( root / "BuildScripts/Workspace.lua" );
    ASSERT_FALSE( workspace.empty() );

    // The declaration line, not the prose around it: Workspace.lua explains at length what the third
    // configuration is for and names it repeatedly while doing so.
    const size_t at = workspace.find( "configurations {" );
    ASSERT_NE( at, std::string::npos ) << "BuildScripts/Workspace.lua no longer declares configurations";
    const size_t close = workspace.find( '}', at );
    ASSERT_NE( close, std::string::npos );

    std::vector<std::string> declared;
    for ( size_t i = at; i < close; ++i )
    {
        if ( workspace[i] != '"' )
            continue;
        const size_t end = workspace.find( '"', i + 1 );
        if ( end == std::string::npos || end > close )
            break;
        declared.push_back( workspace.substr( i + 1, end - i - 1 ) );
        i = end;
    }
    ASSERT_GE( declared.size(), 3u ) << "expected at least Debug, Release and Shipping";

    const std::string unix    = Read( root / "scripts/MacOS/BuildMacOS.sh" );
    const std::string windows = Read( root / "scripts/Windows/BuildWindows.bat" );
    ASSERT_FALSE( unix.empty() );
    ASSERT_FALSE( windows.empty() );

    // WHAT "ACCEPTS" MEANS IS EACH WRAPPER'S OWN ARGUMENT PARSER, NOT ITS USAGE TEXT. The usage line is
    // prose: it can agree with the workspace while the parser rejects the word, and that is exactly the
    // half of this defect a comment-shaped check would have walked past — BuildWindows.bat's usage line
    // and its parser disagreed for a day. So the unix side is read out of the `case ... esac` block with
    // its shell comments cut, and the Windows side out of its `if /I "%~1"=="..."` arms.
    //
    // THE FIRST DRAFT OF THIS LOOP WAS WRONG AND THE SUITE SAID SO: it looked for `Debug)` and
    // BuildMacOS.sh spells all three in ONE arm, `Debug|Release|Shipping)`. A census that only ever ran
    // against a tree it already agreed with would have shipped that.
    const std::string cases = CaseBlockOf( unix );
    ASSERT_FALSE( cases.empty() ) << "scripts/MacOS/BuildMacOS.sh has no `case \"$arg\" in` block any more";

    for ( const std::string& config : declared )
    {
        // An arm alternative ends in `)` when it is last and `|` when another follows.
        const bool unixAccepts =
             cases.find( config + ")" ) != std::string::npos || cases.find( config + "|" ) != std::string::npos;
        EXPECT_TRUE( unixAccepts ) << "scripts/MacOS/BuildMacOS.sh has no case arm for the '" << config
                                   << "' configuration, so it exits 1 on a configuration the workspace "
                                      "declares.";
        EXPECT_NE( windows.find( "\"%~1\"==\"" + config + "\"" ), std::string::npos )
             << "scripts/Windows/BuildWindows.bat has no argument arm for the '" << config
             << "' configuration. Windows is the platform the game ships for; a wrapper that refuses the "
                "shipping configuration there is the same as not having one.";
    }
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
