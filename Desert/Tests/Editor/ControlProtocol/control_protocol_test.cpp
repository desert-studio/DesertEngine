// A REQUEST IS UNDERSTOOD OR NAMED, AND A REPLY ALWAYS CARRIES AN OUTCOME.
//
// The control channel replaces a family of command-line flags that grew one entry per task, and it inherits
// the rule those flags were eventually forced to obey: EVERY input is either recognised and consumed, or
// refused with a message naming it. There is no third outcome. The flags learned it the expensive way --
// `--sceen X` parsed to "nothing happened", which for a capture meant a plausible PNG of the DEFAULT scene
// under the name of the one that was asked for, and one row of measurements was lost to it. A request
// dropped in silence is that same failure over a socket: the client concludes the editor did as it asked.
//
// The relations here:
//
//   1. TOTALITY. An operation this parser does not know is an error that names itself AND lists the known
//      ones -- built from the table the parser actually reads, so the two cannot drift.
//   2. INCOMPLETE REQUESTS ARE REFUSED. A `run` with no label, a shot with no path: each would otherwise
//      "succeed" having done nothing, or having written nowhere.
//   3. AN OUTCOME IS ALWAYS PRESENT, and a refusal always carries a reason. Response has no way to express
//      "failed, and nothing said" -- the shape this project keeps finding and removing.
//   4. ROUND TRIP. What a request means survives being written and read back. This is what says the wire
//      is a wire and not two programs agreeing by luck.
//   5. THE TWO CAPTURES ARE TWO OPERATIONS and neither can be mistaken for the other. Until this channel
//      existed no picture this engine took contained one pixel of its interface, so a window shot quietly
//      answered by a viewport shot would be a picture of the wrong subject under the right name.
//   6. STATE SECTIONS. An unknown section is refused rather than omitted: omitted, it comes back empty,
//      which reads exactly like a section that exists and is empty. One of those two readings is a lie.

#include <Common/Json/Document.hpp>
#include <Common/Json/Json.hpp>
#include <Editor/Core/Control/ControlProtocol.hpp>
#include <Editor/Core/Control/ControlState.hpp>

#include <gtest/gtest.h>

#include <string>
#include <vector>

using Desert::Editor::Control::EditorQuiescence;
using Desert::Editor::Control::EditorSnapshot;
using Desert::Editor::Control::FormatResponse;
using Desert::Editor::Control::IsShot;
using Desert::Editor::Control::kOps;
using Desert::Editor::Control::kStateSections;
using Desert::Editor::Control::kSubjects;
using Desert::Editor::Control::NeedsReadyEditor;
using Desert::Editor::Control::Op;
using Desert::Editor::Control::ParseRequest;
using Desert::Editor::Control::PendingWork;
using Desert::Editor::Control::PropertiesToJson;
using Desert::Editor::Control::Request;
using Desert::Editor::Control::RequiresReady;
using Desert::Editor::Control::Response;
using Desert::Editor::Control::Subject;
using Desert::Editor::Control::ToJson;
using Desert::Editor::Control::ValidateSections;

namespace
{
    Request ParseOk( const std::string& line )
    {
        const auto parsed = ParseRequest( line );
        EXPECT_TRUE( parsed.IsSuccess() ) << line << " -> " << ( parsed.IsSuccess() ? "" : parsed.GetError() );
        return parsed.IsSuccess() ? parsed.GetValue() : Request{};
    }

    std::string ParseError( const std::string& line )
    {
        const auto parsed = ParseRequest( line );
        EXPECT_FALSE( parsed.IsSuccess() ) << line << " was accepted and should not have been";
        return parsed.IsSuccess() ? std::string() : parsed.GetError();
    }

    // The reply, read back as JSON. The whole point of the format tests is that a CLIENT can read what
    // this writes, so they assert against a parse rather than against a substring where they can.
    Common::Json::Object ReadBack( const Response& response )
    {
        const auto parsed = Common::Json::Parse( FormatResponse( response ) );
        EXPECT_TRUE( parsed ) << "a response that is not readable JSON is not a response";
        if ( !parsed )
            return {};
        const auto object = parsed.GetValue().to_object();
        EXPECT_TRUE( object );
        return object ? object.value() : Common::Json::Object{};
    }

    std::string StringField( const Common::Json::Object& object, const char* key )
    {
        const auto field = object.get( key );
        if ( !field )
            return {};
        const auto text = field.value().to_string();
        return text ? text.value() : std::string{};
    }

    bool BoolField( const Common::Json::Object& object, const char* key, bool fallback )
    {
        const auto field = object.get( key );
        if ( !field )
            return fallback;
        const auto value = field.value().to_bool();
        return value ? value.value() : fallback;
    }
} // namespace

// ---------------------------------------------------------------------------------------------------
// 1. Totality.
// ---------------------------------------------------------------------------------------------------

// EVERY operation in the table parses. Walked rather than listed, so an operation added to kOps without a
// case in ParseRequest fails here instead of at the first client that tries it.
TEST( ControlProtocol, EveryKnownOperationParses )
{
    for ( const auto& spec : kOps )
    {
        // Each op is given the fields it requires; the point is that the NAME is accepted and maps to the
        // enumerator it is paired with in the table.
        std::string line = std::string( R"({"id":1,"op":")" ) + spec.Name + R"(")";
        if ( spec.Operation == Op::Run )
            line += R"(,"group":"Panel","label":"Open Details")";
        if ( spec.Operation == Op::Set )
            line += R"(,"property":"RoughnessFactor","value":[0.25])";
        if ( IsShot( spec.Operation ) )
            line += R"(,"path":"/tmp/shot.png")";
        line += "}";

        const Request request = ParseOk( line );
        EXPECT_EQ( static_cast<int>( request.Operation ), static_cast<int>( spec.Operation ) )
             << "'" << spec.Name << "' parsed as a different operation from the one the table pairs it with";
    }
}

// ---------------------------------------------------------------------------------------------------
// WHICH OPERATIONS NEED AN EDITOR THAT HAS FINISHED COMING UP.
//
// `commands` used to be answered the instant it arrived, from whatever the editor held at that moment --
// and it holds almost nothing for the first twenty seconds of a session. Measured on this repository's own
// project: the palette's `Open` group goes 0 -> 106 -> 130 as five separate startup stages fill the asset
// cache, and the 106-entry answer is a SUCCESSFUL reply that stands for 3.3 s of every boot.
//
// The two exemptions are the load-bearing part, so they are pinned BY NAME. Both are about being able to
// deal with an editor that is not fine: `state` is how readiness is observed at all, and `quit` is what
// ends a session whose boot has wedged. An exemption that quietly grew a third member would put some other
// answer back on the wrong side of the boot.
// ---------------------------------------------------------------------------------------------------

TEST( ControlProtocol, OnlyTheTwoOperationsThatMustSurviveABootAreExemptFromWaitingForOne )
{
    EXPECT_FALSE( NeedsReadyEditor( Op::State ) ) << "'state' is how a client WATCHES the boot; blocking it "
                                                     "makes readiness inferable only from silence";
    EXPECT_FALSE( NeedsReadyEditor( Op::Quit ) ) << "a wedged boot is exactly when ending the session matters";

    for ( const auto& spec : kOps )
    {
        if ( spec.Operation == Op::State || spec.Operation == Op::Quit )
            continue;
        EXPECT_TRUE( NeedsReadyEditor( spec.Operation ) )
             << "'" << spec.Name << "' would answer about a project the editor has not read yet";
    }
}

// The table and the function are one answer, not two. A `switch` beside the table is the shape that drifts;
// this asserts that the function really is the table's own field and not a second copy of the decision.
TEST( ControlProtocol, TheReadinessRuleComesFromTheTableAndNotFromASecondList )
{
    for ( const auto& spec : kOps )
    {
        EXPECT_EQ( NeedsReadyEditor( spec.Operation ), spec.Readiness == RequiresReady::Yes )
             << "'" << spec.Name << "'";
    }
}

// ---------------------------------------------------------------------------------------------------
// WHOSE PROPERTIES. The category was written for the focused document; A6-1 gave it a second subject —
// the editor's own view — because placing the camera was wired to `--camera`/`--look`, which are read
// only inside `shot.Active()`. A developer who wanted a viewpoint and no capture had to launch with a
// fictitious `--shot --shot-frames 1000000` to unlock it.
// ---------------------------------------------------------------------------------------------------

// EVERY OLDER CLIENT KEEPS WORKING BY CONSTRUCTION. A request with no `subject` is the focused document,
// which is what the category has always meant — so the field is additive and not a migration.
TEST( ControlProtocol, ARequestThatNamesNoSubjectMeansTheFocusedDocument )
{
    EXPECT_EQ( ParseOk( R"({"id":1,"op":"properties"})" ).Whose, Subject::Document );
    EXPECT_EQ( ParseOk( R"({"id":1,"op":"set","property":"RoughnessFactor","value":[0.25]})" ).Whose,
               Subject::Document );
    // An explicitly empty subject is the same as none: absent and empty-string are not distinguished
    // anywhere else in this parser, and a client that sent "" has failed to name a subject either way.
    EXPECT_EQ( ParseOk( R"({"id":1,"op":"properties","subject":""})" ).Whose, Subject::Document );
}

TEST( ControlProtocol, EveryKnownSubjectParses )
{
    for ( const auto& spec : kSubjects )
    {
        const std::string line = std::string( R"({"id":1,"op":"properties","subject":")" ) + spec.Name + R"("})";
        EXPECT_EQ( static_cast<int>( ParseOk( line ).Whose ), static_cast<int>( spec.Which ) )
             << "'" << spec.Name << "' parsed as a different subject from the one the table pairs it with";
    }
}

// An unknown subject is REFUSED naming the known ones, for the reason an unknown state section is: a
// subject quietly ignored would answer about the focused document while the client believed it had
// addressed the viewport, and the two replies are indistinguishable.
TEST( ControlProtocol, AnUnknownSubjectIsRefusedAndTheKnownOnesListed )
{
    const std::string message = ParseError( R"({"id":1,"op":"properties","subject":"viewpoint"})" );

    EXPECT_NE( message.find( "viewpoint" ), std::string::npos );
    for ( const auto& spec : kSubjects )
        EXPECT_NE( message.find( spec.Name ), std::string::npos ) << spec.Name;
}

// The refusal comes before anything else the request got right or wrong: a `set` with a bad subject AND a
// bad value must complain about the subject, because the value belongs to whatever the subject turns out
// to be and cannot be judged until that is known.
TEST( ControlProtocol, AnUnknownSubjectIsRefusedBeforeTheValueIsJudged )
{
    const std::string message =
         ParseError( R"({"id":1,"op":"set","subject":"nowhere","property":"X","value":[1,2,3,4,5]})" );
    EXPECT_NE( message.find( "nowhere" ), std::string::npos );
}

TEST( ControlProtocol, AnUnknownOperationIsNamedAndListsTheKnownOnes )
{
    const std::string message = ParseError( R"({"id":1,"op":"open-panel"})" );

    EXPECT_NE( message.find( "open-panel" ), std::string::npos ) << "the refusal must quote what was sent";
    for ( const auto& spec : kOps )
    {
        EXPECT_NE( message.find( spec.Name ), std::string::npos )
             << "'" << spec.Name << "' is accepted by the parser and missing from the list it prints";
    }
}

TEST( ControlProtocol, TextThatIsNotAJsonObjectIsRefusedRatherThanIgnored )
{
    EXPECT_FALSE( ParseError( "" ).empty() );
    EXPECT_FALSE( ParseError( "not json at all" ).empty() );
    EXPECT_FALSE( ParseError( R"(["run"])" ).empty() ) << "an array is JSON and is not a request";
    EXPECT_FALSE( ParseError( R"({"id":1})" ).empty() ) << "no operation named";
    EXPECT_FALSE( ParseError( R"({"id":1,"op":""})" ).empty() );
}

// ---------------------------------------------------------------------------------------------------
// 2. Incomplete requests are refused.
// ---------------------------------------------------------------------------------------------------

TEST( ControlProtocol, RunNeedsBothHalvesOfTheAddress )
{
    EXPECT_FALSE( ParseError( R"({"id":1,"op":"run","group":"Panel"})" ).empty() );
    EXPECT_FALSE( ParseError( R"({"id":1,"op":"run","label":"Open Details"})" ).empty() );
    EXPECT_FALSE( ParseError( R"({"id":1,"op":"run","group":"","label":"Open Details"})" ).empty() );

    const Request request = ParseOk( R"({"id":7,"op":"run","group":"Panel","label":"Open Details"})" );
    EXPECT_EQ( request.Id, 7 );
    EXPECT_EQ( request.Group, "Panel" );
    EXPECT_EQ( request.Label, "Open Details" );
}

// THE SECOND CATEGORY OF REQUEST. `set` is the drag a mouse would do -- direct manipulation, which has no
// name and therefore cannot be a palette command. Its parse is total for the same reason `run`'s is: every
// mistake below was a silent no-op in `--material-step`, the flag this replaces, and a value that quietly
// failed to land renders as "the preview did not move" -- indistinguishable, in the very capture taken to
// prove the feature works, from the feature being broken.
TEST( ControlProtocol, SetNeedsAPropertyAndAValueAndSaysWhichIsMissing )
{
    const Request request =
         ParseOk( R"({"id":3,"op":"set","property":"AlbedoColor","value":[0.05,0.35,0.95,1]})" );
    EXPECT_EQ( request.Id, 3 );
    EXPECT_EQ( request.Property, "AlbedoColor" );
    ASSERT_EQ( request.Value.size(), 4u );
    EXPECT_FLOAT_EQ( request.Value[0], 0.05f );
    EXPECT_FLOAT_EQ( request.Value[3], 1.0f );

    EXPECT_FALSE( ParseError( R"({"id":1,"op":"set","value":[0.5]})" ).empty() ) << "no property named";
    EXPECT_FALSE( ParseError( R"({"id":1,"op":"set","property":"","value":[0.5]})" ).empty() );
    EXPECT_FALSE( ParseError( R"({"id":1,"op":"set","property":"X"})" ).empty() )
         << "a write with nothing to write would come back successful and change nothing";
    EXPECT_FALSE( ParseError( R"({"id":1,"op":"set","property":"X","value":[]})" ).empty() )
         << "no property takes nothing";
    EXPECT_FALSE( ParseError( R"({"id":1,"op":"set","property":"X","value":0.5})" ).empty() )
         << "a bare number carries no COUNT, and the count is part of the property's identity";
    EXPECT_FALSE( ParseError( R"({"id":1,"op":"set","property":"X","value":[0.1,0.2,0.3,0.4,0.5]})" ).empty() )
         << "nothing this channel writes is wider than four";
}

// BOTH SPELLINGS OF EVERY NUMBER, and it is not defensive coding. reflect-cpp keeps a JSON number in the
// variant arm its TEXT implies, so `[1, 0.5]` arrives as one int64 and one double. Reading only doubles
// made `[1,1,1]` -- the commonest colour anybody types -- parse as no value at all, which the request
// above would then refuse as empty. The same defect ReadInt was written for, one field along.
TEST( ControlProtocol, AValueReadsWholeNumbersAndFractionsAlike )
{
    const Request request = ParseOk( R"({"id":1,"op":"set","property":"AlbedoColor","value":[1,0.5,0,1]})" );
    ASSERT_EQ( request.Value.size(), 4u );
    EXPECT_FLOAT_EQ( request.Value[0], 1.0f );
    EXPECT_FLOAT_EQ( request.Value[1], 0.5f );
    EXPECT_FLOAT_EQ( request.Value[2], 0.0f );
}

// A NON-NUMBER FAILS THE WHOLE FIELD rather than being skipped. Dropping one element of [1,"x",0] would
// silently turn a three-component write into a two-component one, and the document would then refuse it
// with a count the client never sent -- a refusal about a request that was never made.
TEST( ControlProtocol, AValueWithSomethingThatIsNotANumberIsRefusedWhole )
{
    EXPECT_FALSE( ParseError( R"({"id":1,"op":"set","property":"X","value":[1,"x",0]})" ).empty() );
    EXPECT_FALSE( ParseError( R"({"id":1,"op":"set","property":"X","value":["0.5"]})" ).empty() )
         << "a number in quotes is a client that has not decided what it is sending";
}

// A capture with nowhere to go would report success and leave no evidence -- which is the failure the whole
// capture family exists to prevent, arriving through the door marked "convenience".
TEST( ControlProtocol, AShotNeedsSomewhereToWrite )
{
    EXPECT_FALSE( ParseError( R"({"id":1,"op":"shot.window"})" ).empty() );
    EXPECT_FALSE( ParseError( R"({"id":1,"op":"shot.viewport","path":""})" ).empty() );

    EXPECT_EQ( ParseOk( R"({"id":1,"op":"shot.window","path":"/tmp/a.png"})" ).Path, "/tmp/a.png" );
}

// ---------------------------------------------------------------------------------------------------
// 3. An outcome is always present.
// ---------------------------------------------------------------------------------------------------

TEST( ControlProtocol, EveryResponseCarriesItsOutcome )
{
    const Common::Json::Object success = ReadBack( Response::Success( 3 ) );
    EXPECT_TRUE( BoolField( success, "ok", false ) );
    EXPECT_TRUE( success.get( "id" ) ) << "a client that pipelines cannot match a reply without one";

    const Common::Json::Object failure = ReadBack( Response::Failure( 4, "the scene has no camera" ) );
    EXPECT_FALSE( BoolField( failure, "ok", true ) );
    EXPECT_EQ( StringField( failure, "error" ), "the scene has no camera" );
}

// A refusal with nothing said is the exact shape this project keeps removing. It cannot be built here --
// and rather than accepting an empty reason quietly, the factory produces a message that names the CHANNEL
// as the fault, because that is what it is. Loud and wrong beats silent and wrong: somebody reads it.
TEST( ControlProtocol, ARefusalWithNoReasonBlamesTheChannelRatherThanSayingNothing )
{
    const Common::Json::Object failure = ReadBack( Response::Failure( 5, "" ) );

    EXPECT_FALSE( BoolField( failure, "ok", true ) );
    const std::string reason = StringField( failure, "error" );
    EXPECT_FALSE( reason.empty() ) << "a response that failed and said nothing is the whole defect";
    EXPECT_NE( reason.find( "defect in the control channel" ), std::string::npos );
}

// A payload cannot displace the outcome. The fields a client depends on are written after the payload is
// copied in, so a command whose result happened to carry a key called "ok" cannot make a failure read as a
// success.
TEST( ControlProtocol, APayloadCannotOverwriteTheOutcome )
{
    Common::Json::Object payload;
    payload["ok"]    = Common::Json::Value( false );
    payload["id"]    = Common::Json::Value( 999.0 );
    payload["error"] = Common::Json::Value( std::string( "not really" ) );

    const Common::Json::Object written = ReadBack( Response::Success( 11, payload ) );
    EXPECT_TRUE( BoolField( written, "ok", false ) );
    EXPECT_FALSE( written.get( "error" ) ) << "a success must not carry an error field";
}

// The framing is one message per line, so a payload carrying a newline would split one reply into two --
// and the second half would be read as the answer to the NEXT request.
TEST( ControlProtocol, AResponseIsOneLine )
{
    Common::Json::Object payload;
    payload["message"] = Common::Json::Value( std::string( "first\nsecond\r\nthird" ) );

    const std::string written = FormatResponse( Response::Success( 1, payload ) );
    EXPECT_EQ( written.find( '\n' ), std::string::npos );
    EXPECT_EQ( written.find( '\r' ), std::string::npos );
}

// ---------------------------------------------------------------------------------------------------
// 4. Round trip.
// ---------------------------------------------------------------------------------------------------

// A refusal produced by the editor has to survive the wire and arrive as the same words. A message that is
// mangled in transit is a diagnosis nobody can act on, and quotes badly in a report.
TEST( ControlProtocol, ARefusalSurvivesBeingWrittenAndReadBack )
{
    const std::string reason =
         R"(no command 'Panel' / 'Open "Details"' is offered right now (3 available). Did you mean...?)";

    EXPECT_EQ( StringField( ReadBack( Response::Failure( 2, reason ) ), "error" ), reason );
}

// ---------------------------------------------------------------------------------------------------
// 5. The two captures are two operations.
// ---------------------------------------------------------------------------------------------------

// THE DISTINCTION THIS CHANNEL WAS BUILT ON. `shot.window` reads the presented swapchain image and contains
// the panels, the menus and the dialogs; `shot.viewport` reads the scene's own final image and contains
// none of them, because ImGui is recorded into the swapchain pass. Before this channel, only the second
// existed -- which is why no capture this engine ever took held one pixel of its interface, and why proving
// anything about a panel meant photographing the window from outside the process.
//
// They are separate operations rather than one with a flag precisely so that neither can quietly stand in
// for the other.
TEST( ControlProtocol, TheWindowShotAndTheViewportShotAreDistinctOperations )
{
    const Request window   = ParseOk( R"({"id":1,"op":"shot.window","path":"/tmp/w.png"})" );
    const Request viewport = ParseOk( R"({"id":2,"op":"shot.viewport","path":"/tmp/v.png"})" );

    EXPECT_EQ( window.Operation, Op::ShotWindow );
    EXPECT_EQ( viewport.Operation, Op::ShotViewport );
    EXPECT_NE( static_cast<int>( window.Operation ), static_cast<int>( viewport.Operation ) );

    EXPECT_TRUE( IsShot( window.Operation ) );
    EXPECT_TRUE( IsShot( viewport.Operation ) );
    EXPECT_FALSE( IsShot( Op::Run ) ) << "IsShot decides what waits for a settled frame AND gets captured";
    EXPECT_FALSE( IsShot( Op::State ) );
    EXPECT_FALSE( IsShot( Op::Commands ) );
    EXPECT_FALSE( IsShot( Op::Quit ) );
}

// ---------------------------------------------------------------------------------------------------
// 6. State sections.
// ---------------------------------------------------------------------------------------------------

TEST( ControlProtocol, EveryKnownSectionIsAccepted )
{
    for ( const char* section : kStateSections )
        EXPECT_TRUE( ValidateSections( { section } ).IsSuccess() ) << section;

    EXPECT_TRUE( ValidateSections( {} ).IsSuccess() ) << "empty means all of them";
}

TEST( ControlProtocol, AnUnknownSectionIsRefusedAndTheKnownOnesListed )
{
    const auto refused = ValidateSections( { "documents", "everything" } );
    ASSERT_FALSE( refused.IsSuccess() );

    const std::string message = refused.GetError();
    EXPECT_NE( message.find( "everything" ), std::string::npos );
    for ( const char* section : kStateSections )
        EXPECT_NE( message.find( section ), std::string::npos ) << section << " is served and not listed";
}

// EVERY section, asked for on its own, comes back. Walked rather than spot-checked: a section in the table
// with no branch in ToJson would validate, return nothing, and read as "the editor has none of those".
TEST( ControlProtocol, EverySectionInTheTableIsActuallySerialised )
{
    EditorSnapshot snapshot;
    snapshot.SceneName = "Clouds_Protocol";

    for ( const char* section : kStateSections )
    {
        const Common::Json::Object json = ToJson( snapshot, { section } );
        EXPECT_TRUE( json.get( section ) )
             << "'" << section << "' is offered by ValidateSections and produced no output";
    }
}

TEST( ControlProtocol, AskingForOneSectionDoesNotReturnTheOthers )
{
    EditorSnapshot snapshot;
    snapshot.SceneName = "Clouds_Protocol";

    const Common::Json::Object json = ToJson( snapshot, { "scene" } );
    EXPECT_TRUE( json.get( "scene" ) );
    EXPECT_FALSE( json.get( "documents" ) );
    EXPECT_FALSE( json.get( "panels" ) );
}

// THE DOCUMENTS SECTION IS WHAT THE ACCEPTANCE OF THIS TASK RESTS ON: the open documents in most-recently-
// used order, and the list of the ones that were CLOSED -- state that outlives the window it describes and
// which nothing could photograph before, because closing a window needs a mouse.
TEST( ControlProtocol, TheDocumentsSectionCarriesBothTheOpenOnesAndTheClosedOnes )
{
    EditorSnapshot snapshot;
    snapshot.Documents.push_back( { .Name               = "M_Crate",
                                    .Type               = "SurfaceMaterial",
                                    .Subject            = "1111",
                                    .HoldsRendererSlot  = true,
                                    .ClaimsRendererSlot = true,
                                    .Focused            = true } );
    snapshot.RecentlyClosed.push_back( { .Name = "M_Barrel", .Type = "SurfaceMaterial", .Subject = "2222" } );

    const Common::Json::Object json      = ToJson( snapshot, { "documents" } );
    const auto                 documents = json.get( "documents" );
    ASSERT_TRUE( documents );

    const auto object = documents.value().to_object();
    ASSERT_TRUE( object );

    const auto open = object.value().get( "open" );
    ASSERT_TRUE( open );
    const auto openArray = open.value().to_array();
    ASSERT_TRUE( openArray );
    ASSERT_EQ( openArray.value().size(), 1u );

    const auto first = openArray.value()[0].to_object();
    ASSERT_TRUE( first );
    EXPECT_EQ( StringField( first.value(), "name" ), "M_Crate" );
    EXPECT_TRUE( BoolField( first.value(), "holdsSlot", false ) );
    EXPECT_TRUE( BoolField( first.value(), "focused", false ) );

    const auto closed = object.value().get( "recentlyClosed" );
    ASSERT_TRUE( closed );
    const auto closedArray = closed.value().to_array();
    ASSERT_TRUE( closedArray );
    ASSERT_EQ( closedArray.value().size(), 1u );

    const auto onlyClosed = closedArray.value()[0].to_object();
    ASSERT_TRUE( onlyClosed );
    EXPECT_EQ( StringField( onlyClosed.value(), "name" ), "M_Barrel" );
}

// ---------------------------------------------------------------------------------------------------
// 5. The picture and the number have to say the same thing.
// ---------------------------------------------------------------------------------------------------

// A CAPTURE ALONE CANNOT PROVE "THE PREVIEW MOVED AND THE SCENE DID NOT". It cannot distinguish that from
// a scene which happens to be out of frame, or from an editor that did nothing at all. The three fields
// below are what turn the sequence of pictures into evidence: a client reads them beside each shot and the
// two agree or the claim fails.
TEST( ControlProtocol, EachOpenDocumentReportsWhereItsEditsHaveReached )
{
    EditorSnapshot snapshot;
    snapshot.Documents.push_back( { .Name              = "M_Crate",
                                    .Type              = "SurfaceMaterial",
                                    .Subject           = "1111",
                                    .Focused           = true,
                                    .EditModel         = "staged",
                                    .HasUnappliedEdits = true,
                                    .DiskState         = "dirty" } );

    const auto documents = ToJson( snapshot, { "documents" } ).get( "documents" );
    ASSERT_TRUE( documents );
    const auto open = documents.value().to_object().value().get( "open" );
    ASSERT_TRUE( open );
    const auto first = open.value().to_array().value()[0].to_object();
    ASSERT_TRUE( first );

    EXPECT_EQ( StringField( first.value(), "editModel" ), "staged" );
    EXPECT_TRUE( BoolField( first.value(), "unapplied", false ) );
    EXPECT_EQ( StringField( first.value(), "disk" ), "dirty" );
}

// UNTRACKED IS THE DEFAULT AND IT IS NOT "CLEAN". A document that took no snapshot of its file has no
// evidence about it, and a client that read the two as one would report an unsaved edit as saved.
TEST( ControlProtocol, ADocumentThatSaysNothingIsNotReportedAsSavedAndUpToDate )
{
    EditorSnapshot snapshot;
    snapshot.Documents.push_back( { .Name = "Untitled", .Type = "SurfaceMaterial", .Subject = "3333" } );

    const auto open =
         ToJson( snapshot, { "documents" } ).get( "documents" ).value().to_object().value().get( "open" );
    ASSERT_TRUE( open );
    const auto first = open.value().to_array().value()[0].to_object();
    ASSERT_TRUE( first );

    EXPECT_EQ( StringField( first.value(), "disk" ), "untracked" );
    EXPECT_EQ( StringField( first.value(), "editModel" ), "write-through" )
         << "a document that does not say it stages must not be offered Apply and Discard";
    EXPECT_FALSE( BoolField( first.value(), "unapplied", true ) );
}

// ---------------------------------------------------------------------------------------------------
// 6. The property census on the wire.
// ---------------------------------------------------------------------------------------------------

TEST( ControlProtocol, ThePropertyCensusCarriesTheShapeOfEveryPropertyAndNamesItsDocument )
{
    Desert::Editor::EditableProperty roughness;
    roughness.Name       = "RoughnessFactor";
    roughness.Label      = "Roughness";
    roughness.Type       = "float";
    roughness.Components = 1;
    roughness.Min        = 0.0f;
    roughness.Max        = 1.0f;
    roughness.Value[0]   = 0.25f;

    Desert::Editor::EditableProperty albedoMap;
    albedoMap.Name              = "AlbedoMap";
    albedoMap.Type              = "texture";
    albedoMap.Components        = 0;
    albedoMap.Settable          = false;
    albedoMap.NotSettableReason = "'AlbedoMap' is a texture slot.";

    const auto payload = PropertiesToJson( "M_Crate", { roughness, albedoMap } );

    // THE SUBJECT IS NAMED, because both of them move: the focus changes, and so does whether the editor's
    // own camera is the view being driven. A client that asked for properties and then set one has to be
    // able to see WHICH thing answered, or a change between the two requests is invisible in both replies.
    EXPECT_EQ( StringField( payload, "subject" ), "M_Crate" );

    const auto entries = payload.get( "properties" );
    ASSERT_TRUE( entries );
    const auto array = entries.value().to_array();
    ASSERT_TRUE( array );
    ASSERT_EQ( array.value().size(), 2u );

    const auto first = array.value()[0].to_object();
    ASSERT_TRUE( first );
    EXPECT_EQ( StringField( first.value(), "name" ), "RoughnessFactor" );
    EXPECT_EQ( StringField( first.value(), "type" ), "float" );
    EXPECT_TRUE( BoolField( first.value(), "settable", false ) );
    EXPECT_TRUE( first.value().get( "min" ) );
    EXPECT_TRUE( first.value().get( "max" ) );

    // Exactly as many numbers as the property takes -- the count is what the editor checks a write
    // against, so a census that padded it to four would be describing a property nobody can set.
    const auto value = first.value().get( "value" );
    ASSERT_TRUE( value );
    ASSERT_TRUE( value.value().to_array() );
    EXPECT_EQ( value.value().to_array().value().size(), 1u );

    // A row that cannot be written is LISTED, saying no and saying why. Omitting it would read as a
    // property the shader does not declare, and that is a different problem with a different fix.
    const auto second = array.value()[1].to_object();
    ASSERT_TRUE( second );
    EXPECT_FALSE( BoolField( second.value(), "settable", true ) );
    EXPECT_FALSE( StringField( second.value(), "why" ).empty() );
    EXPECT_EQ( second.value().get( "value" ).value().to_array().value().size(), 0u );
}

// A property with no declared range has NO min/max field, rather than a null or a made-up bound. A client
// that read `min` as a number cannot be handed a null, and inventing 0 would tell it the editor refuses
// negatives when the editor does not.
TEST( ControlProtocol, APropertyWithNoDeclaredRangeCarriesNoBounds )
{
    Desert::Editor::EditableProperty unbounded;
    unbounded.Name       = "Tiling";
    unbounded.Type       = "float2";
    unbounded.Components = 2;

    const auto payload = PropertiesToJson( "M_Crate", { unbounded } );
    const auto first   = payload.get( "properties" ).value().to_array().value()[0].to_object();
    ASSERT_TRUE( first );

    EXPECT_FALSE( first.value().get( "min" ) );
    EXPECT_FALSE( first.value().get( "max" ) );
    EXPECT_EQ( first.value().get( "value" ).value().to_array().value().size(), 2u );
}

// The quiescence section speaks the SAME vocabulary a settle timeout does, so a client that read
// "asset documents are waiting to be opened" here recognises it when a refusal quotes it back.
TEST( ControlProtocol, TheQuiescenceSectionNamesOutstandingWorkInTheSameWordsARefusalDoes )
{
    EditorSnapshot snapshot;
    snapshot.Quiescence.Set( PendingWork::AssetOpens, true );

    const auto section = ToJson( snapshot, { "quiescence" } ).get( "quiescence" );
    ASSERT_TRUE( section );
    const auto object = section.value().to_object();
    ASSERT_TRUE( object );

    EXPECT_FALSE( BoolField( object.value(), "settled", true ) );
    EXPECT_EQ( StringField( object.value(), "outstanding" ), EditorQuiescence( snapshot.Quiescence ).Describe() );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
