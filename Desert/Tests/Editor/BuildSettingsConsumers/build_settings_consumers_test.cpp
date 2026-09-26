// DOES THE PACKAGER READ EVERY CHOICE THE BUILD SETTINGS PANEL OFFERS?
//
// The defect this suite was written from (П6): the panel's "Target platform" radio group wrote
// `m_Platform`, and NOTHING read it. `PackageOptions` had three fields — OutputDir, Config,
// MacAppBundle — and no platform among them. So a person could select "Windows x64" on a macOS host,
// press Build, and receive a macOS package with not one line of log saying the choice had been dropped.
// That is two contract violations at once: §1.3, a knob that moves nothing, and the worse §1.4, a
// result that is not the one asked for and is silent about it. A silently wrong package can ship.
//
// The comment in BuildSettingsPanel.hpp said so in prose ("Platform selection beyond the host is still
// a placeholder"), which is exactly the state this project keeps paying for: TEXT THAT NAMES A GAP IS
// NOT A GATE. A comment cannot go red. This suite can.
//
// ===================================================================================================
// THE RELATION, AND WHY IT IS ASSERTED RATHER THAN EACH SIDE
// ===================================================================================================
//
// Both sides of the packaging seam are individually correct and always were. The panel really does draw
// a radio group and really does store the answer; the packager really does read every field of the
// struct it is handed. What was wrong is the RELATION between them — the set the panel offers and the
// set the packager consumes were not the same set, and nothing anywhere compared them. This is the
// defect shape the verify skill's §4 table is made of, and the fix is the same: assert the agreement.
//
//   LEFT  — what the panel OFFERS. Derived, never typed: an editing widget in BuildSettingsPanel.cpp
//           (RadioButton, Checkbox, InputText, ...) naming a data member of the panel or ANY field of
//           EditorPreferences. Somebody who adds a widget cannot avoid landing here — and the
//           preference half is taken against the whole struct rather than a `Package*` prefix,
//           because a naming convention nobody is obliged to follow is not a gate.
//   RIGHT — what the packager READS. Derived from rfl::fields<PackageOptions>(), the same mechanism
//           the struct is passed by, and each field must have an ANCHORED READ in GamePackager.cpp
//           (setting_consumers_reader.hpp: a member access on a receiver this file binds to
//           PackageOptions, not a mere mention of the word).
//
// The table in the middle is the only hand-written part, and every row of it is checked from both ends,
// so a wrong row fails rather than passes.
//
// WHY IT IS TEXTUAL. The panel is ImGui: there is no object to interrogate, no reflection over a widget
// tree, and building one to answer this question would be a subsystem where a census does. The reader
// this borrows (Д33's rebuild) already distinguishes a read from a write and from a word inside a
// comment or a literal, which is the whole of what is needed here — and reusing it is deliberate: it is
// the sixth census on this reader and the first that would have had a reason to fork it.
//
// WHAT THIS DOES NOT CLAIM. It does not prove the packager USES the value well, only that it reads it.
// PackageOptions::Config additionally reaches the cook's SPIR-V profile, and Desert/Tests/Editor/
// PackagedContent is what exercises the produced package end to end.

#include "../../Engine/SettingConsumers/setting_consumers_reader.hpp"

#include <Editor/Core/EditorPreferences.hpp>
#include <Editor/Packaging/GamePackager.hpp>
#include <Editor/Packaging/PackageTarget.hpp>

#include <rflcpp/rfl/fields.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    using namespace Desert::Tests::ConsumerText;

    constexpr const char* kPanelHeader = "Editor/Source/Editor/Panels/Build/BuildSettingsPanel.hpp";
    constexpr const char* kPanelSource = "Editor/Source/Editor/Panels/Build/BuildSettingsPanel.cpp";
    constexpr const char* kPackager    = "Editor/Source/Editor/Packaging/GamePackager.cpp";
    // PK2: the chunk panel is the second half of the packaging UI and is censused as one with the first —
    // its members are read into the same Header/Panel texts, so every test below covers both panels.
    constexpr const char* kChunksHeader = "Editor/Source/Editor/Panels/Build/ContentChunksPanel.hpp";
    constexpr const char* kChunksSource = "Editor/Source/Editor/Panels/Build/ContentChunksPanel.cpp";

    // ------------------------------------------------------------------------------------------------
    // The table. One row per thing the panel can put in front of a person.
    // ------------------------------------------------------------------------------------------------

    struct Row
    {
        // The panel's data member (`m_OutputDir`) or a packaging field of EditorPreferences, spelled as
        // it is declared.
        const char* Name;

        // Exactly one of the two must be set.
        //
        // Option    — the PackageOptions field this choice fills. The panel must WRITE it and
        //             GamePackager.cpp must READ it; both are checked.
        // Machinery — not a setting: panel bookkeeping (async handshake, cached lists, last result).
        //             The reason is mandatory, and a member that an editing widget names may not claim
        //             it.
        //
        // There is no third kind for "the choice goes somewhere other than PackageOptions". One was
        // written and then removed unused, which is the stub §1.2 forbids; the day a choice really does
        // leave by another door, adding the kind back is three lines and the census will demand it.
        const char* Option    = nullptr;
        const char* Machinery = nullptr;
        // Scheme    — the ChunkSchemeSession method the text is HANDED to (PK2). The choice leaves by the
        //             chunk scheme file, not PackageOptions: the panel must pass the member to that method,
        //             and GamePackager.cpp must read the file (LoadChunkScheme). Both are checked.
        const char* Scheme = nullptr;
    };

    constexpr Row kRows[] = {
         // ---- The four packaging choices ----------------------------------------------------------
         //
         // Three of them live in EditorPreferences rather than in the panel, and that is not a detail of
         // this table: they are per-USER answers (an output path on one person's disk, which Runtime
         // that person wants bundled, whether they want a .app), so by the К1 procedure's first question
         // they belong to editor.json. The panel edits the preference in place — it keeps no copy, which
         // is what EditorPreferences.hpp's own header demands and what К6 had to undo elsewhere.
         { "PackageOutputDir", "OutputDir" },
         { "PackageConfig", "Config" },
         { "PackageAppBundle", "MacAppBundle" },

         // ---- Panel bookkeeping -------------------------------------------------------------------
         { "m_Building", nullptr, "the async handshake: set on submit, cleared by the worker. Not a choice." },
         { "m_HasResult", nullptr,
           "the other half of that handshake — whether the strings below are readable yet." },
         { "m_LastSuccess", nullptr, "the finished job's outcome, displayed and nothing else." },
         { "m_LastComplete", nullptr,
           "whether the finished package shipped everything the cook was asked to produce — the THIRD "
           "state (I12). Displayed as a colour and nothing else; not a choice anybody makes." },
         { "m_LastCookFailures", nullptr,
           "how many assets the cook could not read, displayed under the message. Not a choice." },
         { "m_LastCookUnwritten", nullptr,
           "how many cooked artifacts did not reach the disk, displayed likewise. Not a choice." },
         { "m_LastMessage", nullptr, "the finished job's message, displayed and nothing else." },
         { "m_LastPackageDir", nullptr,
           "where the finished job wrote, so Reveal in Finder has somewhere to open." },
         { "m_LastManifestPath", nullptr,
           "where PackageGame recorded this release's patch baseline (П7), displayed so whoever built "
           "the game knows there is a file to keep. Empty for Build pak only — a dev archive is not a "
           "release. Not a choice anybody makes." },
         { "m_Scenes", nullptr,
           "the CANDIDATE list for the startup-scene combo. The chosen value is not held here at all: it "
           "goes straight into the .deproj through ProjectContext::SetDefaultScene, where "
           "Desert/Tests/Engine/ConfigOwnership censuses it as DefaultScene." },
         { "m_ScenesScanned", nullptr, "whether that list has been filled yet." },

         // ---- The Content Chunks panel (PK2) --------------------------------------------------------
         { "m_ChunkNameInput", nullptr, nullptr, "AddChunk" },
         { "m_KeyInput", nullptr, nullptr, "AddRoot" },
         { "m_PreviewRevision", nullptr,
           "the session revision the preview below was derived at, so it is re-derived when that moves. "
           "Not a choice." },
         { "m_PreviewNames", nullptr, "the derived plan's chunk names, drawn as the table's columns only." },
         { "m_PreviewFolders", nullptr,
           "SummarizeChunkFolders' answer for the draft — the packager's ChunkFor, displayed only." },
         { "m_PreviewUnresolved", nullptr, "how many dependency handles named no registry row, displayed only." },
         { "m_PreviewError", nullptr, "BuildChunkPlan's refusal of the draft, displayed only. Not a choice." },
    };

    // The ImGui calls through which a person CHANGES something. A member or preference named inside one
    // of these is a choice being offered, whatever the table would like to call it.
    //
    // Display calls (Text, TextDisabled, TextColored, BeginDisabled, ...) are deliberately absent: they
    // are how the panel reports, and reporting a value is not offering it. The list has to grow when the
    // panel grows a new kind of widget, and `EveryEditingWidgetInThePanelIsOneThisSuiteKnows` is what
    // makes forgetting that fail here instead of passing quietly.
    constexpr const char* kEditingWidgets[] = {
         "RadioButton", "Checkbox",   "InputText", "InputInt",   "InputFloat",  "InputDouble",
         "SliderFloat", "SliderInt",  "DragFloat", "DragInt",    "DragFloat2",  "DragFloat3",
         "ColorEdit3",  "ColorEdit4", "Combo",     "Selectable", "SliderAngle",
    };

    // ------------------------------------------------------------------------------------------------
    // Reading the tree
    // ------------------------------------------------------------------------------------------------

    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            std::ifstream probe( prefix + kPanelHeader );
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

    void AddUnique( std::vector<std::string>& out, const std::string& name )
    {
        if ( name.empty() )
            return;
        if ( std::find( out.begin(), out.end(), name ) == out.end() )
            out.push_back( name );
    }

    // Every `m_`-prefixed identifier the panel's HEADER states — which, in a header whose only bodies are
    // a constructor and a one-line size getter, is exactly its data members. Derived rather than typed so
    // that a member added tomorrow is censused without anybody remembering this file exists.
    std::vector<std::string> DeclaredMembers( const std::string& headerText )
    {
        std::vector<std::string> out;
        for ( std::size_t i = 0; i < headerText.size(); )
        {
            const std::string id = IdentAt( headerText, i );
            if ( id.empty() )
            {
                ++i;
                continue;
            }
            i += id.size();
            if ( id.rfind( "m_", 0 ) == 0 )
                AddUnique( out, id );
        }
        return out;
    }

    // The text between the parentheses of `call(` at `at`, balanced.
    std::string ArgumentsAt( const std::string& s, std::size_t at )
    {
        std::size_t i = SkipSpace( s, at );
        if ( i >= s.size() || s[i] != '(' )
            return {};
        const std::size_t open  = i;
        int               depth = 0;
        for ( ; i < s.size(); ++i )
        {
            if ( s[i] == '(' )
                ++depth;
            else if ( s[i] == ')' && --depth == 0 )
                return s.substr( open + 1, i - open - 1 );
        }
        return {};
    }

    // Every identifier that appears inside the argument list of an editing widget in `s`.
    std::vector<std::string> NamesInEditingWidgets( const std::string& s )
    {
        std::vector<std::string> out;
        for ( const char* widget : kEditingWidgets )
        {
            for ( std::size_t at : WordPositions( s, widget ) )
            {
                const std::string args = ArgumentsAt( s, at + std::string( widget ).size() );
                for ( std::size_t i = 0; i < args.size(); )
                {
                    const std::string id = IdentAt( args, i );
                    if ( id.empty() )
                    {
                        ++i;
                        continue;
                    }
                    i += id.size();
                    AddUnique( out, id );
                }
            }
        }
        return out;
    }

    // `receiver.Field =` — the panel filling the option in, which is the opposite of what MemberReadAt
    // accepts and exactly what is wanted on this side of the seam.
    bool ReceiverWritesField( const std::string& s, const std::string& receiver, const std::string& field )
    {
        for ( std::size_t at : WordPositions( s, receiver ) )
        {
            std::size_t i = SkipSpace( s, at + receiver.size() );
            if ( i < s.size() && s[i] == '.' )
                ++i;
            else if ( i + 1 < s.size() && s[i] == '-' && s[i + 1] == '>' )
                i += 2;
            else
                continue;

            i = SkipSpace( s, i );
            if ( !WordAt( s, i, field ) )
                continue;

            const std::size_t after = SkipSpace( s, i + field.size() );
            if ( after < s.size() && s[after] == '=' && ( after + 1 >= s.size() || s[after + 1] != '=' ) )
                return true;
        }
        return false;
    }

    bool FileReadsOption( const std::string& text, const std::string& field )
    {
        const std::vector<std::string> receivers = DeriveReceivers( text, { "PackageOptions" } );
        for ( const std::string& recv : receivers )
            if ( ReceiverReadsField( text, recv, field ) )
                return true;
        return AnchorReadsField( text, "PackageOptions", field );
    }

    bool FileWritesOption( const std::string& text, const std::string& field )
    {
        const std::vector<std::string> receivers = DeriveReceivers( text, { "PackageOptions" } );
        for ( const std::string& recv : receivers )
            if ( ReceiverWritesField( text, recv, field ) )
                return true;
        return false;
    }

    const Row* FindRow( const std::string& name )
    {
        for ( const Row& r : kRows )
            if ( name == r.Name )
                return &r;
        return nullptr;
    }

    std::vector<std::string> AllPreferenceFields()
    {
        std::vector<std::string> out;
        for ( const auto& meta : rfl::fields<Desert::Editor::EditorPreferences>() )
            out.push_back( meta.name() );
        return out;
    }

    // The packaging half of editor.json, by prefix. The prefix is a NAMING CONVENTION and is used in one
    // direction only — `EveryPackagingPreferenceIsOfferedByThePanel`, which stops a `Package*` field
    // being added to editor.json and never drawn anywhere.
    //
    // Coverage in the other direction deliberately does NOT rest on it: what the panel OFFERS is derived
    // from the widget calls against the WHOLE preference struct, so a packaging setting somebody names
    // something else is censused all the same. A prefix nobody is obliged to use is not a gate.
    std::vector<std::string> PackagingPreferenceFields()
    {
        std::vector<std::string> out;
        for ( const std::string& name : AllPreferenceFields() )
            if ( name.rfind( "Package", 0 ) == 0 )
                out.push_back( name );
        return out;
    }

    std::vector<std::string> PackageOptionFields()
    {
        std::vector<std::string> out;
        for ( const auto& meta : rfl::fields<Desert::Editor::PackageOptions>() )
            out.push_back( meta.name() );
        return out;
    }

    // The OTHER direction of the same relation, and it had none until I12. This suite censused what the
    // packager is TOLD; nothing censused what it ANSWERS — so `CookStats::Failures` was counted, logged
    // and dropped, and a package with an unbakeable font came back indistinguishable from a clean one.
    // A result field nobody reads is a dead setting with the blast radius of a shipped build.
    std::vector<std::string> PackageResultFields()
    {
        std::vector<std::string> out;
        for ( const auto& meta : rfl::fields<Desert::Editor::PackageResult>() )
            out.push_back( meta.name() );
        return out;
    }

    struct Sources
    {
        std::string Header;
        std::string Panel;
        std::string Packager;
    };

    Sources ReadSources()
    {
        const std::string root = RepoRoot();
        if ( root.empty() )
            return {};
        return {
             StripCommentsAndLiterals( ReadAll( root + kPanelHeader ) + "\n" + ReadAll( root + kChunksHeader ) ),
             StripCommentsAndLiterals( ReadAll( root + kPanelSource ) + "\n" + ReadAll( root + kChunksSource ) ),
             StripCommentsAndLiterals( ReadAll( root + kPackager ) ) };
    }
} // namespace

// ---------------------------------------------------------------------------------------------------
// 0. THE SUITE CAN SEE WHAT IT CLAIMS TO CHECK
// ---------------------------------------------------------------------------------------------------
//
// Without this every loop below runs over an empty set and reports green — the failure that looks
// exactly like a census which found nothing wrong. ConfigOwnership and SceneDebugFields both carry the
// same guard for the same reason.
TEST( BuildSettingsConsumers, TheSourcesThisSuiteReadsAreWhereItThinksTheyAre )
{
    ASSERT_FALSE( RepoRoot().empty() ) << "repository root not found from the test's working directory";

    const Sources src = ReadSources();
    EXPECT_FALSE( src.Header.empty() ) << kPanelHeader << " is missing or empty";
    EXPECT_FALSE( src.Panel.empty() ) << kPanelSource << " is missing or empty";
    EXPECT_FALSE( src.Packager.empty() ) << kPackager << " is missing or empty";

    EXPECT_FALSE( DeclaredMembers( src.Header ).empty() )
         << "the panel declares no data members at all — it was renamed, moved, or this suite is reading "
            "the wrong file";
    EXPECT_FALSE( NamesInEditingWidgets( src.Panel ).empty() )
         << "no editing widget was found in the panel — either the panel stopped offering anything, or "
            "kEditingWidgets no longer names the calls it uses";
    EXPECT_FALSE( PackageOptionFields().empty() ) << "PackageOptions enumerated to no fields";
    EXPECT_FALSE( PackageResultFields().empty() ) << "PackageResult enumerated to no fields";
}

// ---------------------------------------------------------------------------------------------------
// 1a. WHAT THE PACKAGER ANSWERS IS READ BY THE PANEL THAT SHOWS IT
// ---------------------------------------------------------------------------------------------------
//
// The mirror of `EveryPackageOptionIsOfferedBySomething`, and the direction that was missing. Packaging
// is the LAST step before a build reaches a player, so a fact the packager reports and the panel drops
// is a fact nobody learns until somebody runs the game: `Failures` was exactly that, and the panel
// painted an incomplete package the same green as a complete one.
//
// Read against the panel's SOURCE, because that is where a result is consumed — the panel copies the
// fields it cares about into its own members on a worker thread. `Complete()` is a member FUNCTION and
// so is not in this census; it is covered by the panel having to read it to pick a colour, which the
// assertion below states as its own row.
TEST( BuildSettingsConsumers, EveryPackageResultFieldIsReadByThePanel )
{
    const Sources src = ReadSources();
    ASSERT_FALSE( src.Panel.empty() );

    const std::vector<std::string> fields = PackageResultFields();
    ASSERT_FALSE( fields.empty() );

    for ( const std::string& field : fields )
    {
        EXPECT_TRUE( AnchorReadsField( src.Panel, "result", field ) )
             << "PackageResult::" << field
             << " is reported by the packager and read by nothing in the Build Settings panel. A result "
                "field with no reader is a fact the person who built the game never sees - which is the "
                "defect I12 removed, arriving again under a new name.";
    }

    // The named third state, which is what makes the counts actionable rather than decorative: the panel
    // has to ASK it, or it is back to painting one colour for two different outcomes.
    EXPECT_NE( src.Panel.find( "result.Complete()" ), std::string::npos )
         << "the panel does not ask whether the package is COMPLETE, so it cannot distinguish a package "
            "that shipped everything from one that did not.";
}

// ---------------------------------------------------------------------------------------------------
// 1. THE TABLE COVERS THE PANEL, IN BOTH DIRECTIONS
// ---------------------------------------------------------------------------------------------------

// What the table must cover is DERIVED from two facts about the tree and neither is a list anybody
// types: every data member the panel declares, and every editor PREFERENCE one of its widgets edits.
// The second half is taken against the whole EditorPreferences struct rather than against a `Package*`
// prefix on purpose — a naming convention is not a gate, and a packaging setting called something else
// has to land here too.
TEST( BuildSettingsConsumers, EveryPanelMemberAndPreferenceItEditsIsCensusedExactlyOnce )
{
    const Sources src = ReadSources();
    ASSERT_FALSE( src.Header.empty() );
    ASSERT_FALSE( src.Panel.empty() );

    const std::vector<std::string> offered  = NamesInEditingWidgets( src.Panel );
    const std::vector<std::string> allPrefs = AllPreferenceFields();

    std::vector<std::string> expected = DeclaredMembers( src.Header );
    for ( const std::string& pref : allPrefs )
        if ( std::find( offered.begin(), offered.end(), pref ) != offered.end() )
            AddUnique( expected, pref );

    std::vector<std::string> fromTable;
    for ( const Row& r : kRows )
        fromTable.push_back( r.Name );

    std::sort( expected.begin(), expected.end() );
    std::sort( fromTable.begin(), fromTable.end() );

    EXPECT_EQ( expected, fromTable )
         << "the census and what the Build Settings panel actually holds disagree. A member or packaging "
            "preference the table does not name is a setting nobody has decided the consumer of; a row "
            "naming something that no longer exists is a stale row.";
}

TEST( BuildSettingsConsumers, EveryRowNamesExactlyOneKindOfConsumer )
{
    for ( const Row& r : kRows )
    {
        SCOPED_TRACE( r.Name );
        const int kinds = ( r.Option != nullptr ) + ( r.Machinery != nullptr ) + ( r.Scheme != nullptr );
        EXPECT_EQ( kinds, 1 ) << "a row must name exactly one of: the PackageOptions field it fills, or "
                                 "the reason it is not a setting at all";

        if ( r.Machinery != nullptr )
            EXPECT_GE( std::string( r.Machinery ).size(), 20u )
                 << "say what the member is for; a one-word reason is unreadable in a month";
    }
}

// ---------------------------------------------------------------------------------------------------
// 2. THE ASSERTION П6 EXISTS FOR
// ---------------------------------------------------------------------------------------------------
//
// A member the panel puts inside an editing widget is a CHOICE, and a choice must reach something. This
// is the leg that goes red on the tree П6 started from: `m_Platform` sits inside an `ImGui::RadioButton`
// and no row can honestly name a consumer for it, because `PackageOptions` has no platform at all.
//
// It is derived from the widget call and not from the table, which is what makes it ungameable: writing
// `Machinery` on a member that a radio button edits fails here, and that is precisely the edit somebody
// under time pressure would make.
TEST( BuildSettingsConsumers, NothingAnEditingWidgetOffersIsCalledMachinery )
{
    const Sources src = ReadSources();
    ASSERT_FALSE( src.Panel.empty() );

    const std::vector<std::string> offered = NamesInEditingWidgets( src.Panel );
    const std::vector<std::string> members = DeclaredMembers( src.Header );
    const std::vector<std::string> prefs   = AllPreferenceFields();

    for ( const std::string& name : offered )
    {
        const bool isMember = std::find( members.begin(), members.end(), name ) != members.end();
        const bool isPref   = std::find( prefs.begin(), prefs.end(), name ) != prefs.end();
        if ( !isMember && !isPref )
            continue; // a literal, a local, an ImVec2 — not something this census is about

        const Row* row = FindRow( name );
        ASSERT_NE( row, nullptr ) << name << " is edited by a widget and is not in the census at all";

        EXPECT_EQ( row->Machinery, nullptr )
             << name
             << " is edited by an ImGui widget — a person can change it — but the census calls it panel "
                "machinery. Either it is a setting and must name the consumer that reads it, or the "
                "widget should not be offering it.";
    }
}

// ---------------------------------------------------------------------------------------------------
// 3. THE SEAM ITSELF: OFFERED -> PackageOptions -> READ
// ---------------------------------------------------------------------------------------------------

TEST( BuildSettingsConsumers, EveryOfferedChoiceIsWrittenIntoTheOptionsAndReadByThePackager )
{
    const Sources src = ReadSources();
    ASSERT_FALSE( src.Panel.empty() );
    ASSERT_FALSE( src.Packager.empty() );

    const std::vector<std::string> optionFields = PackageOptionFields();

    for ( const Row& r : kRows )
    {
        if ( r.Option == nullptr )
            continue;
        SCOPED_TRACE( std::string( r.Name ) + " -> PackageOptions::" + r.Option );

        EXPECT_NE( std::find( optionFields.begin(), optionFields.end(), r.Option ), optionFields.end() )
             << r.Option << " is not a field of PackageOptions — a stale row outliving the option";

        EXPECT_TRUE( FileWritesOption( src.Panel, r.Option ) )
             << kPanelSource << " never assigns PackageOptions::" << r.Option
             << ", so the choice the panel draws is not the one the packager is handed.";

        EXPECT_TRUE( FileReadsOption( src.Packager, r.Option ) )
             << kPackager << " contains no read of PackageOptions::" << r.Option
             << " on a value of that type. The panel offers a choice the packager ignores — which is how "
                "somebody gets a package that is not the one they asked for, with nothing in the log.";
    }
}

// The other direction, and it catches the opposite mistake: an option the packager honours that no
// widget can set. That is not harmless — it is a behaviour with no way to reach it, and the default
// silently decides for everybody.
TEST( BuildSettingsConsumers, EveryPackageOptionIsOfferedBySomething )
{
    for ( const std::string& field : PackageOptionFields() )
    {
        const bool claimed = std::any_of( std::begin( kRows ), std::end( kRows ), [&field]( const Row& r )
                                          { return r.Option != nullptr && field == r.Option; } );
        EXPECT_TRUE( claimed ) << "PackageOptions::" << field
                               << " is read by the packager but no row of this census fills it — an option "
                                  "with no way for a person to choose it.";
    }
}

// ---------------------------------------------------------------------------------------------------
// 4. THE PREFERENCE HALF
// ---------------------------------------------------------------------------------------------------
//
// The three packaging choices live in editor.json, so they are reachable from outside the panel and a
// preference that nothing draws is possible in a way a panel member is not. ConfigOwnership asks whether
// each has a consumer at all; this asks the sharper question — is it OFFERED?
TEST( BuildSettingsConsumers, EveryPackagingPreferenceIsOfferedByThePanel )
{
    const Sources src = ReadSources();
    ASSERT_FALSE( src.Panel.empty() );

    const std::vector<std::string> offered = NamesInEditingWidgets( src.Panel );

    for ( const std::string& pref : PackagingPreferenceFields() )
        EXPECT_NE( std::find( offered.begin(), offered.end(), pref ), offered.end() )
             << "EditorPreferences::" << pref
             << " is a packaging preference that no editing widget in the Build Settings panel draws. It "
                "is persisted to every editor.json and nobody can change it.";
}

// ---------------------------------------------------------------------------------------------------
// 5. THE WIDGET LIST CANNOT FALL BEHIND THE PANEL
// ---------------------------------------------------------------------------------------------------
//
// Everything above rests on kEditingWidgets naming the calls the panel really uses. If the panel starts
// using one this list does not know, the derivation quietly sees fewer choices — the census would then
// shrink and stay green, which is the failure mode §1.4 of the contract is about, applied to the checker
// itself. So the panel's ImGui calls are enumerated and each is required to be classified.
// ---------------------------------------------------------------------------------------------------
// 3b. THE OTHER DOOR: A TEXT HANDED TO THE CHUNK SCHEME SESSION (PK2)
// ---------------------------------------------------------------------------------------------------
//
// A Scheme row claims its member reaches the packager through ContentChunks.json. Checked from both
// ends: the panel passes the member as an argument of that session method, and the packager reads the
// file. A row naming a method the member never reaches fails here.
TEST( BuildSettingsConsumers, EveryTextHandedToTheChunkSchemeReachesTheSessionAndThePackagerReadsTheFile )
{
    const Sources src = ReadSources();
    ASSERT_FALSE( src.Panel.empty() );
    ASSERT_FALSE( WordPositions( src.Packager, "LoadChunkScheme" ).empty() )
         << kPackager
         << " no longer reads the chunk scheme, so nothing the Content Chunks panel edits reaches a package";

    int schemeRows = 0;
    for ( const Row& r : kRows )
    {
        if ( r.Scheme == nullptr )
            continue;
        ++schemeRows;
        bool handed = false;
        for ( std::size_t at : WordPositions( src.Panel, r.Scheme ) )
        {
            const std::string args = ArgumentsAt( src.Panel, at + std::string( r.Scheme ).size() );
            handed                 = handed || !WordPositions( args, r.Name ).empty();
        }
        EXPECT_TRUE( handed ) << r.Name << " is censused as handed to ChunkSchemeSession::" << r.Scheme
                              << ", and no call of it in the panel takes that member";
    }
    EXPECT_GE( schemeRows, 2 );
}

TEST( BuildSettingsConsumers, EveryEditingWidgetInThePanelIsOneThisSuiteKnows )
{
    const Sources src = ReadSources();
    ASSERT_FALSE( src.Panel.empty() );

    // The calls this panel makes that are NOT editing widgets. Named one by one rather than by a rule,
    // because "does this widget change a value" is a judgement and the judgement is the point.
    static const char* kKnownDisplayOrLayout[] = {
         "TextUnformatted", "TextDisabled",     "TextColored",      "Text",
         "Separator",       "Spacing",          "SameLine",         "BeginDisabled",
         "EndDisabled",     "Button",           "SmallButton",      "BeginCombo",
         "EndCombo",        "SetNextItemWidth", "PushTextWrapPos",  "PopTextWrapPos",
         "IsItemHovered",   "SetTooltip",       "BulletText",       "TextWrapped",
         "BeginTable",      "EndTable",         "TableSetupColumn", "TableSetupScrollFreeze",
         "TableHeadersRow", "TableNextRow",     "TableNextColumn",  "PushID",
         "PopID",
    };

    // Every `ImGui::<Name>` the panel calls — and every `ImGuiUtilities::<Name>` too, because the
    // engine's own wrappers are widgets as much as ImGui's are. The output-folder field is one:
    // `Utils::ImGuiUtilities::InputText` edits a std::string, and a version of this test that watched
    // only the `ImGui::` namespace would have let a whole second family of widgets in unclassified.
    std::vector<std::string> called;
    for ( const char* ns : { "ImGui", "ImGuiUtilities" } )
    {
        for ( std::size_t at : WordPositions( src.Panel, ns ) )
        {
            std::size_t i = SkipSpace( src.Panel, at + std::string( ns ).size() );
            if ( i + 1 >= src.Panel.size() || src.Panel[i] != ':' || src.Panel[i + 1] != ':' )
                continue;
            AddUnique( called, IdentAt( src.Panel, SkipSpace( src.Panel, i + 2 ) ) );
        }
    }
    ASSERT_FALSE( called.empty() ) << "no ImGui:: call found in the panel at all";

    for ( const std::string& name : called )
    {
        const bool editing = std::any_of( std::begin( kEditingWidgets ), std::end( kEditingWidgets ),
                                          [&name]( const char* w ) { return name == w; } );
        const bool display = std::any_of( std::begin( kKnownDisplayOrLayout ), std::end( kKnownDisplayOrLayout ),
                                          [&name]( const char* w ) { return name == w; } );

        EXPECT_TRUE( editing || display )
             << "ImGui::" << name
             << " is called by the Build Settings panel and this suite does not know whether it EDITS a "
                "value or merely displays one. Add it to kEditingWidgets or to the display list — until "
                "then the census cannot see what that call offers.";
    }
}

// ---------------------------------------------------------------------------------------------------
// 6. THE TARGET DESCRIPTION, AND THE ONE THING THE HOST BUILD CANNOT PROVE ABOUT ITSELF
// ---------------------------------------------------------------------------------------------------
//
// The packager's macOS behaviour is exercised every time anybody packages on this machine. Its WINDOWS
// behaviour is not, and cannot be — this suite runs on a macOS host and `HostPlatform()` is a compile
// time answer. So what is asserted here is the RELATION every row of the table must satisfy, which is
// checkable for all three platforms at once regardless of which one is running: a Windows package must
// name a `.exe` and a `.bat`, a Unix one must not, and the .app layout belongs to exactly one platform.
//
// This is the §4 pattern rather than a test of either side: the four artifacts (binary name, launcher
// name, bundle support, build script) were four independent macOS literals scattered through
// GamePackager.cpp, each individually correct and collectively unable to describe any other host.
TEST( PackageTargetTable, EveryPlatformsArtifactNamesAgreeWithThatPlatform )
{
    using namespace Desert::Editor;

    ASSERT_EQ( kTargetPlatformCount, 3u ) << "a platform was added or removed without a decision";

    int bundleHosts = 0;
    for ( const TargetPlatformInfo& target : kTargetPlatforms )
    {
        SCOPED_TRACE( target.DisplayName );

        const std::string binary   = target.RuntimeBinary;
        const std::string launcher = target.LauncherName;

        EXPECT_EQ( &PlatformInfo( target.Platform ), &target )
             << "PlatformInfo() does not return this row — the table's order and the enum have drifted";
        EXPECT_FALSE( binary.empty() );
        EXPECT_FALSE( launcher.empty() );

        // A null BuildScript is a legitimate row (Linux: the engine has no build for it), an EMPTY one
        // is not — that is a row claiming to name a script and naming nothing.
        if ( target.BuildScript != nullptr )
            EXPECT_FALSE( std::string( target.BuildScript ).empty() );

        ASSERT_NE( target.NotHereReason, nullptr )
             << "every row needs the sentence the panel shows when it is not the host";
        EXPECT_FALSE( std::string( target.NotHereReason ).empty() );

        const bool windows = target.Platform == TargetPlatform::Windows;
        EXPECT_EQ( binary.size() > 4 && binary.substr( binary.size() - 4 ) == ".exe", windows )
             << "the Runtime binary's extension and the platform disagree — the packager would look for a "
                "file that host can never produce";
        EXPECT_EQ( launcher.size() > 4 && launcher.substr( launcher.size() - 4 ) == ".bat", windows )
             << "the launcher's extension and the platform disagree — the package would ship a script "
                "nothing on that host can run";

        if ( target.SupportsAppBundle )
        {
            ++bundleHosts;
            EXPECT_EQ( target.Platform, TargetPlatform::MacOS )
                 << "a .app bundle is a macOS layout; claiming it elsewhere produces a bundle-shaped "
                    "directory with the wrong launcher inside it";
        }
    }
    EXPECT_EQ( bundleHosts, 1 ) << "exactly one platform has .app bundles";
}

TEST( PackageTargetTable, ThisEditorOffersItsOwnHostAndRefusesEveryOtherWithAReason )
{
    using namespace Desert::Editor;

    int hosts = 0;
    for ( const TargetPlatformInfo& target : kTargetPlatforms )
    {
        SCOPED_TRACE( target.DisplayName );
        const char* why = WhyNotPackageableHere( target.Platform );

        if ( target.Platform == HostPlatform() )
        {
            ++hosts;
            EXPECT_EQ( why, nullptr ) << "the host refuses itself";
            continue;
        }

        // The §1.4 half: a target that cannot be produced must SAY so, not be quietly dropped.
        ASSERT_NE( why, nullptr ) << "a non-host platform with no stated reason — the panel would show a "
                                     "row that looks available and is not";
        EXPECT_FALSE( std::string( why ).empty() );
    }
    EXPECT_EQ( hosts, 1 ) << "exactly one row is this editor's host";
    // That the HOST always has a build script is asserted where it belongs, at compile time in
    // PackageTarget.hpp: it is the string the packager's "Runtime binary not found" message is made of,
    // and a null there is a build error rather than a test failure.

    EXPECT_GE( std::string( kWhyOnlyTheHostIsOffered ).size(), 60u )
         << "the paragraph under the rows has to answer 'then how do I get a Windows build?' — a refusal "
            "that does not say what to do instead is half an answer";
}

// The packager must TAKE those four artifacts from the description rather than restating them. Without
// this the table above can be perfectly right and the packager still hard-code macOS, which is the state
// П6 found it in.
TEST( BuildSettingsConsumers, ThePackagerTakesItsArtifactNamesFromTheHostDescription )
{
    const Sources src = ReadSources();
    ASSERT_FALSE( src.Packager.empty() );

    const std::vector<std::string> receivers = DeriveReceivers( src.Packager, { "TargetPlatformInfo" } );
    ASSERT_FALSE( receivers.empty() )
         << kPackager << " binds no TargetPlatformInfo at all — it is describing its host itself again";

    for ( const char* field : { "RuntimeBinary", "LauncherName", "BuildScript", "SupportsAppBundle" } )
    {
        bool read = false;
        for ( const std::string& recv : receivers )
            read = read || ReceiverReadsField( src.Packager, recv, field );
        EXPECT_TRUE( read ) << kPackager << " never reads TargetPlatformInfo::" << field
                            << ", so that part of the package is decided by something other than the host "
                               "description the panel shows.";
    }
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
