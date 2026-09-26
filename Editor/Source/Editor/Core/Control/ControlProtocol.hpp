#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Common/Json/Document.hpp>
#include <Common/Json/Json.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::Editor::Control
{
    /**
     * @brief THE WIRE. One JSON object per line in, one JSON object per line out.
     *
     * WHY A CHANNEL AT ALL, and why this file is the whole of its vocabulary. This editor already had a
     * control surface: eighteen command-line flags, four of them purely about driving the interface
     * (`--select`, `--open-panel`, `--open-menu`, `--preview-orbit`). Nobody designed it — each flag was
     * added by whichever developer needed a picture that week, because macOS refuses synthetic input to
     * this machine and a click is therefore not available. It grew by one flag per task and could only
     * ever grow that way, since a flag is read once at boot and a session is not a boot.
     *
     * This replaces that growth with a channel, and the four control flags are DELETED rather than kept
     * beside it: two ways to open a panel is the defect shape this codebase spends its days removing, and
     * the losing side is always the one nobody remembers to update.
     *
     * THE PARSE IS TOTAL, exactly as Editor/Core/CommandLine.hpp is for argv, and for the same reason
     * written out there at length: a request this parser does not understand is an ERROR NAMING ITSELF,
     * never a request that quietly did nothing. A client that mistyped an operation and got silence would
     * conclude the editor had done what it asked.
     *
     * NOTHING HERE TOUCHES A SOCKET, ImGui, THE RENDERER OR A GLOBAL. Bytes in, a request out; a response
     * in, bytes out. That is what makes the whole protocol assertable by a suite — and it has to be,
     * because EditorLayer.cpp is compiled by no suite at all (scripts/CI/UnreachedSources.sh) and a rule
     * written there is a rule nothing can show going red.
     */

    /**
     * @brief TWO CATEGORIES OF REQUEST, AND WHY THERE HAD TO BE A SECOND ONE.
     *
     * `commands`/`run` reach the COMMAND PALETTE: a dictionary of ACTIONS, each with a name. That is
     * everything a person does by choosing something.
     *
     * `properties`/`set` reach the FOCUSED DOCUMENT's own values. That is everything a person does by
     * DRAGGING something, and it does not fit in a dictionary of actions: a slider has no name, and giving
     * it one would mean a palette entry per value — "Set Albedo to 0.3", a hundred and seventy of them for
     * one material — which is an argument list pretending to be a vocabulary.
     *
     * THIS IS A SECOND CATEGORY, NOT A SECOND EXECUTION PATH, and the difference is the whole of the rule.
     * A `set` lands in the same setter the widget calls (MaterialEditorPanel::WriteParam), so there is one
     * route into the value and two ways to reach it — a mouse and a socket. What stays forbidden is a
     * second DISPATCH for palette commands: two lists of actions that must agree, one of which falls
     * behind. Nothing here is one; `properties` is derived from the document's own declaration, and for a
     * material that declaration is the shader's schema.
     */

    /// What a request asks for. Deliberately a closed set.
    enum class Op
    {
        Commands,     ///< list every command the palette offers this instant
        Run,          ///< run one of them, addressed by group + label
        Properties,   ///< list the properties the FOCUSED document exposes, with their current values
        Set,          ///< write one of them, addressed by name
        State,        ///< read the editor's state as JSON
        ShotWindow,   ///< capture the WHOLE editor, interface included (swapchain readback)
        ShotViewport, ///< capture the 3D viewport only, no interface (the scene's final image)
        Quit,         ///< end the session with an exit status
    };

    /**
     * @brief DOES THIS OPERATION NEED AN EDITOR THAT HAS FINISHED COMING UP?
     *
     * THE DEFECT THIS ANSWERS, with its numbers. `commands` used to be answered the instant it arrived,
     * from whatever the editor happened to hold at that moment — and the editor holds almost nothing for
     * the first twenty seconds of a session. Measured on this repository's own project: the palette's
     * `Open` group goes 0 -> 106 -> 130 as five separate startup stages fill the asset cache, and the
     * 106-entry answer — every material, not one of the twenty-four cloud assets — is a SUCCESSFUL reply
     * that stands for 3.3 seconds of every single boot. A client cannot tell it from a project that has no
     * cloud documents. §1.4: an empty (or half-empty) successful answer is a silent wrong answer.
     *
     * TWO OPERATIONS ARE DELIBERATELY EXEMPT, and the exemptions are the interesting half.
     *
     * `state` must answer THROUGHOUT the boot, because it is how readiness is OBSERVED: its `quiescence`
     * section names what is still outstanding, in the same words a refusal uses. Making it wait would be
     * blinding the one client that is watching the editor come up, and would turn readiness back into
     * something inferred from silence — which is exactly the property this whole change exists to remove.
     *
     * `quit` must answer because a boot that has WEDGED is the case where ending the session matters most.
     * An operation that could only be run by an editor that was already fine is no use to anybody.
     *
     * STATED IN THE TABLE AND NOT IN A `switch`, so it is impossible to add an operation without deciding.
     * The field has no default: `{ "thing", Op::Thing }` does not compile, and the next person is made to
     * answer the question rather than inherit somebody else's answer.
     */
    enum class RequiresReady
    {
        No,  ///< answered from whatever state the editor is in, including mid-boot
        Yes, ///< held until a presented frame proves the editor settled, or refused saying what is pending
    };

    /// One accepted operation. A table, for the same reason kCommandLineFlags is one: the message that
    /// lists the known operations is built FROM the set the parser accepts, so the two cannot drift.
    struct OpSpec
    {
        const char*   Name;
        Op            Operation;
        RequiresReady Readiness;
    };

    inline constexpr OpSpec kOps[] = {
         { "commands", Op::Commands, RequiresReady::Yes },
         { "run", Op::Run, RequiresReady::Yes },
         { "properties", Op::Properties, RequiresReady::Yes },
         { "set", Op::Set, RequiresReady::Yes },
         { "state", Op::State, RequiresReady::No },
         { "shot.window", Op::ShotWindow, RequiresReady::Yes },
         { "shot.viewport", Op::ShotViewport, RequiresReady::Yes },
         { "quit", Op::Quit, RequiresReady::No },
    };

    /// The table's answer for one operation. Yes for anything not in the table at all — an operation this
    /// build has never heard of is the last thing that should be run against a half-built editor.
    [[nodiscard]] constexpr bool NeedsReadyEditor( Op op ) noexcept
    {
        for ( const OpSpec& spec : kOps )
        {
            if ( spec.Operation == op )
                return spec.Readiness == RequiresReady::Yes;
        }
        return true;
    }

    /// TWO CAPTURES THAT NEVER SUBSTITUTE FOR EACH OTHER, and that is why they are two operations rather
    /// than one with a flag. `shot.window` reads the presented swapchain image and therefore contains the
    /// panels, the menus and the dialogs; `shot.viewport` reads the scene's own final image and contains
    /// none of them, because ImGui is recorded into the swapchain pass (VulkanImGuiLayer::End).
    ///
    /// Until this channel existed, only the second capture existed at all — so no picture ever taken by
    /// this engine held one pixel of its interface, and `--open-menu`'s promise that "a capture can show
    /// what is in it" was a promise its own capture path could not keep. A window shot that silently
    /// fell back to the viewport would be that same failure with a new name: a picture of the wrong
    /// subject, delivered under the name of the right one.
    [[nodiscard]] constexpr bool IsShot( Op op ) noexcept
    {
        return op == Op::ShotWindow || op == Op::ShotViewport;
    }

    /**
     * @brief WHOSE PROPERTIES `properties` AND `set` ARE ABOUT.
     *
     * A CLOSED SET OF TWO, and the second one is A6-1's. The category was written for the focused document
     * and that stays the default, so every client that predates this sends nothing and gets what it always
     * got. What it could not reach is the EDITOR'S OWN VIEW: placing the camera was wired to `--camera` /
     * `--look`, which are read only inside `shot.Active()`, so a developer who wanted the camera somewhere
     * and had no intention of taking a `--shot` had to launch with a fictitious `--shot --shot-frames
     * 1000000` to unlock it. The mandatory step of a proof was being done by the flag family this channel
     * replaced.
     *
     * A SUBJECT AND NOT AN OPERATION, because it is the same request: same census type, same refusals,
     * same JSON. Two operations would be two vocabularies for "read a value and write it back", and the
     * one nobody remembers falls behind — which is this codebase's most-paid-for defect shape.
     *
     * NOT A FREE STRING either. An unknown subject is refused NAMING the known ones, for the reason a
     * section is (ControlState.hpp): a subject quietly ignored would answer about the focused document
     * while the client believed it had addressed the viewport, and both answers look exactly alike.
     */
    enum class Subject
    {
        Document, ///< the focused document's own values — the default, and what every older client means
        Viewport, ///< the editor's view: where the camera is and which way it looks
        Modeling, ///< the Modeling panel's dragged values (Core::kModelingStateRows)
    };

    struct SubjectSpec
    {
        const char* Name;
        Subject     Which;
    };

    inline constexpr SubjectSpec kSubjects[] = {
         { "document", Subject::Document },
         { "viewport", Subject::Viewport },
         { "modeling", Subject::Modeling },
    };

    [[nodiscard]] inline std::string KnownSubjectList()
    {
        std::string list;
        for ( const SubjectSpec& spec : kSubjects )
        {
            if ( !list.empty() )
                list += ", ";
            list += spec.Name;
        }
        return list;
    }

    struct Request
    {
        /// Echoed in the response. A client that pipelines needs to know which answer is whose, and an id
        /// the client chose is the only thing that can tell it — the editor's own frame counter cannot.
        int64_t Id = 0;

        Op Operation = Op::Commands;

        /// Op::Run — the palette entry, addressed exactly as the palette shows it.
        std::string Group;
        std::string Label;

        /// Op::Properties / Op::Set — whose values. Absent means the focused document, which is what the
        /// category has always meant and what every client written before the viewport existed sends.
        Subject Whose = Subject::Document;

        /// Op::Set — the property of the subject, addressed by the name its declaration gives it
        /// (not by its label: two properties may display the same words).
        std::string Property;

        /// Op::Set — the value, with as many components as the property takes. The COUNT is carried rather
        /// than padded to four: a float given three numbers is a client that meant a different property,
        /// and the document refuses it instead of writing something that looks accepted.
        std::vector<float> Value;

        /// Op::ShotWindow / Op::ShotViewport — where the PNG goes.
        std::string Path;

        /// Op::State — which sections to include. Empty means all of them.
        std::vector<std::string> Sections;

        /// Op::Quit — the process exit status.
        int32_t ExitCode = 0;
    };

    /// The known operations, comma separated, for the message a rejected one gets. Built from the table
    /// so an operation added above appears here without anyone remembering to add it.
    [[nodiscard]] inline std::string KnownOpList()
    {
        std::string list;
        for ( const OpSpec& spec : kOps )
        {
            if ( !list.empty() )
                list += ", ";
            list += spec.Name;
        }
        return list;
    }

    namespace ProtocolDetail
    {
        /// A string field, or empty when absent. Absent and empty-string are the same thing to every
        /// consumer here, so they are not distinguished — a client that omits `group` and a client that
        /// sends `"group": ""` have both failed to name a command, and get the same refusal.
        [[nodiscard]] inline std::string ReadString( const Common::Json::Node& fields, const char* key )
        {
            const auto field = fields.Find( key );
            if ( !field )
                return {};
            const auto text = field->AsString();
            return text ? text.GetValue() : std::string{};
        }

        /// An integer field, or @p fallback.
        ///
        /// BOTH SPELLINGS ARE ACCEPTED, and that is not defensive coding: reflect-cpp keeps a whole JSON
        /// number in the variant arm its text implies, so `7` arrives as an int64 and `7.0` as a double.
        /// Asking only for a double made every `"id": 7` read as the fallback — which meant every reply
        /// came back with id 0 and a client that pipelined could not match one answer to its question.
        /// Measured, not reasoned about: the round-trip test caught it on the first run.
        [[nodiscard]] inline int64_t ReadInt( const Common::Json::Node& fields, const char* key, int64_t fallback )
        {
            const auto field = fields.Find( key );
            if ( !field )
                return fallback;

            if ( const auto whole = field->AsInteger() )
                return whole.GetValue();
            if ( const auto real = field->AsNumber() )
                return static_cast<int64_t>( real.GetValue() );
            return fallback;
        }

        /// An array-of-strings field, or empty. Anything in the array that is not a string is DROPPED
        /// rather than stringified: a section name is matched against a known set downstream, so a number
        /// here would be reported as an unknown section, which is a clearer message than a coerced "3".
        [[nodiscard]] inline std::vector<std::string> ReadStringArray( const Common::Json::Node& fields,
                                                                       const char*               key )
        {
            std::vector<std::string> values;
            const auto               field = fields.Find( key );
            if ( !field )
                return values;

            if ( field->GetKind() != Common::Json::Kind::Array )
                return values;

            field->ForEachElement(
                 [&values]( std::size_t, const Common::Json::Node& element )
                 {
                     if ( const auto text = element.AsString() )
                         values.push_back( text.GetValue() );
                 } );
            return values;
        }

        /// What a `value` field turned out to be. THREE outcomes and not two, because "absent" and
        /// "present but not numbers" are different mistakes and a client told the wrong one looks for the
        /// fault in the wrong place.
        enum class NumberArray
        {
            Absent,     ///< no `value` field at all
            NotNumbers, ///< there is one, but it is not an array of numbers
            Read        ///< read, into the out parameter (possibly empty)
        };

        /// An array-of-numbers field. BOTH SPELLINGS OF EVERY ELEMENT, for the reason ReadInt states at
        /// length: reflect-cpp keeps a number in the variant arm its text implies, so `[1, 0.5]` arrives
        /// as one int64 and one double. Reading only doubles made `[1,1,1]` — the commonest colour a
        /// person types — parse as nothing at all.
        ///
        /// A non-number ANYWHERE in the array fails the whole field rather than being skipped: dropping
        /// one element of `[1,"x",0]` would silently turn a three-component write into a two-component
        /// one, and the document would refuse it with a count the client never sent.
        [[nodiscard]] inline NumberArray ReadFloatArray( const Common::Json::Node& fields, const char* key,
                                                         std::vector<float>& out )
        {
            const auto field = fields.Find( key );
            if ( !field )
                return NumberArray::Absent;

            if ( field->GetKind() != Common::Json::Kind::Array )
                return NumberArray::NotNumbers;

            out.clear();
            bool allNumbers = true;
            field->ForEachElement(
                 [&]( std::size_t, const Common::Json::Node& element )
                 {
                     if ( !allNumbers )
                         return;
                     if ( const auto whole = element.AsInteger() )
                     {
                         out.push_back( static_cast<float>( whole.GetValue() ) );
                         return;
                     }
                     if ( const auto real = element.AsNumber() )
                     {
                         out.push_back( static_cast<float>( real.GetValue() ) );
                         return;
                     }
                     allNumbers = false;
                 } );

            if ( !allNumbers )
            {
                out.clear();
                return NumberArray::NotNumbers;
            }
            return NumberArray::Read;
        }
    } // namespace ProtocolDetail

    /**
     * @brief One line of JSON -> a request, or a NAMED failure.
     *
     * Every failure mode below used to be a silent no-op in the flag family this replaces:
     *   - text that is not a JSON object at all;
     *   - no `op` field;
     *   - an `op` this parser does not know;
     *   - `run` without a group or without a label;
     *   - a shot without a path to write to.
     */
    [[nodiscard]] inline Common::ResultStr<Request> ParseRequest( std::string_view line )
    {
        using namespace ProtocolDetail;

        const auto parsed = Common::Json::Parse( line );
        if ( !parsed )
        {
            return Common::MakeFormattedError<Request>( "the request is not JSON: {}", parsed.GetError() );
        }

        const Common::Json::Value& document = parsed.GetValue();
        const Common::Json::Node   fields   = Common::Json::Root( document );
        if ( fields.GetKind() != Common::Json::Kind::Object )
        {
            return Common::MakeError<Request>(
                 "the request is JSON but not an object; every request is a single object with an 'op' "
                 "field." );
        }

        const std::string opName = ReadString( fields, "op" );
        if ( opName.empty() )
        {
            return Common::MakeFormattedError<Request>(
                 "the request names no operation ('op' is missing or empty). Known operations: {}",
                 KnownOpList() );
        }

        const OpSpec* spec = nullptr;
        for ( const OpSpec& candidate : kOps )
        {
            if ( opName == candidate.Name )
            {
                spec = &candidate;
                break;
            }
        }

        if ( spec == nullptr )
        {
            return Common::MakeFormattedError<Request>(
                 "'{}' is not an operation this editor knows. An operation dropped in silence would look "
                 "exactly like an editor that ignored it. Known operations: {}",
                 opName, KnownOpList() );
        }

        Request request;
        request.Id        = ReadInt( fields, "id", 0 );
        request.Operation = spec->Operation;

        // THE SUBJECT, FOR THE TWO OPERATIONS THAT HAVE ONE. Parsed before the per-operation switch
        // because an unknown subject must be refused whatever else the request got right — a request that
        // named "viewpoint" and was answered about the focused document would be answered wrongly and
        // successfully, and the two replies are indistinguishable.
        if ( spec->Operation == Op::Properties || spec->Operation == Op::Set )
        {
            const std::string subjectName = ReadString( fields, "subject" );
            if ( !subjectName.empty() )
            {
                const SubjectSpec* subject = nullptr;
                for ( const SubjectSpec& candidate : kSubjects )
                {
                    if ( subjectName == candidate.Name )
                    {
                        subject = &candidate;
                        break;
                    }
                }

                if ( subject == nullptr )
                {
                    return Common::MakeFormattedError<Request>(
                         "'{}' is not something this editor has properties for. Known subjects: {}.", subjectName,
                         KnownSubjectList() );
                }
                request.Whose = subject->Which;
            }
        }

        switch ( spec->Operation )
        {
            case Op::Run:
            {
                request.Group = ReadString( fields, "group" );
                request.Label = ReadString( fields, "label" );
                if ( request.Group.empty() || request.Label.empty() )
                {
                    return Common::MakeError<Request>(
                         "'run' addresses a palette entry by BOTH its group and its label, and one of them "
                         "is missing. Ask 'commands' for the pairs this editor offers right now." );
                }
                break;
            }
            case Op::Set:
            {
                request.Property = ReadString( fields, "property" );
                if ( request.Property.empty() )
                {
                    return Common::MakeError<Request>(
                         "'set' addresses a property of the focused document by name, and none was given. "
                         "Ask 'properties' for the ones it offers right now." );
                }

                switch ( ReadFloatArray( fields, "value", request.Value ) )
                {
                    case NumberArray::Absent:
                        return Common::MakeFormattedError<Request>(
                             "'set' needs a 'value' for '{}'. A write with nothing to write would come back "
                             "successful and change nothing.",
                             request.Property );
                    case NumberArray::NotNumbers:
                        return Common::MakeFormattedError<Request>(
                             "the 'value' for '{}' is not an array of numbers. Write it as one even for a "
                             "single component — [0.2] — so the COUNT is always part of the request; the "
                             "document checks it against the property's own shape.",
                             request.Property );
                    case NumberArray::Read:
                        break;
                }

                if ( request.Value.empty() )
                {
                    return Common::MakeFormattedError<Request>(
                         "the 'value' for '{}' is an empty array. No property takes nothing.", request.Property );
                }

                // The count is not checked against the property here and cannot be: this parser has never
                // heard of a shader schema. The DOCUMENT owns that check, because the document is what owns
                // the declaration — see ISubjectDocument::SetEditableProperty. What is refused here is
                // only what makes no sense for any property at all.
                if ( request.Value.size() > 4 )
                {
                    return Common::MakeFormattedError<Request>(
                         "the 'value' for '{}' carries {} numbers. Nothing this channel can write is wider "
                         "than four.",
                         request.Property, request.Value.size() );
                }
                break;
            }

            case Op::ShotWindow:
            case Op::ShotViewport:
            {
                request.Path = ReadString( fields, "path" );
                if ( request.Path.empty() )
                {
                    return Common::MakeError<Request>(
                         "a shot needs a 'path' to write the PNG to. A capture with nowhere to go would "
                         "report success and leave no evidence." );
                }
                break;
            }
            case Op::State:
                request.Sections = ReadStringArray( fields, "sections" );
                break;
            case Op::Quit:
                request.ExitCode = static_cast<int32_t>( ReadInt( fields, "code", 0 ) );
                break;
            case Op::Commands:
            case Op::Properties:
                break;
        }

        return Common::MakeSuccess( std::move( request ) );
    }

    /**
     * @brief What goes back. ALWAYS carries an outcome, and a refusal ALWAYS carries a reason.
     *
     * The two factories are the only way to build one, and that is the point: a default-constructed
     * response would be a silent failure — `ok` false with nothing said — which is precisely the shape
     * this project keeps finding and removing. Here the type cannot express it.
     */
    class Response
    {
    public:
        [[nodiscard]] static Response Success( int64_t id, Common::Json::Object payload = {} )
        {
            Response response;
            response.m_Id      = id;
            response.m_Ok      = true;
            response.m_Payload = std::move( payload );
            return response;
        }

        [[nodiscard]] static Response Failure( int64_t id, std::string reason )
        {
            Response response;
            response.m_Id = id;
            response.m_Ok = false;
            // A refusal with nothing said is the defect this class exists to prevent, so an empty reason
            // is not quietly accepted — it becomes a message that names the channel itself as the fault.
            // Loud and wrong beats silent and wrong: someone reads this and fixes the call site.
            response.m_Error = reason.empty()
                                    ? std::string( "a refusal was produced with no reason given; that is a "
                                                   "defect in the control channel, not in the request." )
                                    : std::move( reason );
            return response;
        }

        [[nodiscard]] int64_t Id() const noexcept
        {
            return m_Id;
        }
        [[nodiscard]] bool Ok() const noexcept
        {
            return m_Ok;
        }
        [[nodiscard]] const std::string& Error() const noexcept
        {
            return m_Error;
        }
        [[nodiscard]] const Common::Json::Object& Payload() const noexcept
        {
            return m_Payload;
        }

    private:
        Response() = default;

        int64_t              m_Id = 0;
        bool                 m_Ok = false;
        std::string          m_Error;
        Common::Json::Object m_Payload;
    };

    /// The three keys the outcome owns. A payload may not carry them, whatever it thinks it is doing.
    inline constexpr const char* kReservedResponseKeys[] = { "id", "ok", "error" };

    /**
     * @brief The response as ONE LINE of JSON.
     *
     * ONE LINE, because the framing is one message per line: a payload carrying a newline would split one
     * reply into two, and the second half would be read as the answer to the NEXT request.
     *
     * THE RESERVED KEYS ARE REBUILT, NOT OVERWRITTEN. Copying the payload and then assigning `ok` over it
     * is not enough, and the difference is the whole of a defect this had: a SUCCESS whose payload happens
     * to carry a key called `error` would keep it, and every client convention in existence reads an
     * `error` field as a failure. So the payload is filtered first and the outcome written into a clean
     * object — the three fields every reader depends on come from the response and from nowhere else.
     */
    [[nodiscard]] inline std::string FormatResponse( const Response& response )
    {
        Common::Json::Object object;

        for ( const auto& [key, value] : response.Payload() )
        {
            const bool reserved =
                 std::any_of( std::begin( kReservedResponseKeys ), std::end( kReservedResponseKeys ),
                              [&key]( const char* name ) { return key == name; } );
            if ( !reserved )
                object[key] = value;
        }

        // AN INTEGER, NOT A DOUBLE. reflect-cpp keeps a number in the variant arm its type implies, so a
        // double comes back out as `1.0` — and a client that matched its request id against the reply's
        // would have been comparing 1 with 1.0. The request side accepts both spellings (ReadInt); the
        // reply side emits the one the request used.
        object["id"] = Common::Json::Value( response.Id() );
        object["ok"] = Common::Json::Value( response.Ok() );
        if ( !response.Ok() )
            object["error"] = Common::Json::Value( response.Error() );

        std::string text = Common::Json::Write( Common::Json::Value( object ) );
        std::erase( text, '\n' );
        std::erase( text, '\r' );
        return text;
    }
} // namespace Desert::Editor::Control
