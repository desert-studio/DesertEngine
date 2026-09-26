#pragma once

// CARRYING THE KEYS ANOTHER BUILD WROTE, WITHOUT RESPELLING A NUMBER.
//
// A file that is loaded, edited and saved must come back with every key this build does not state exactly
// where it stood (Engine/Core/Serialize/ForeignKeys.hpp is the rule). The merge that does it runs HERE, on
// yyjson documents, and not on Json::Value, for one reason: Value (rfl::Generic) has a signed 64-bit integer
// and no unsigned one, so an entity id at or above 2^63 read into a Value comes out NEGATIVE - the same 64
// bits, a different text. Every save of a loaded scene used to spell such ids negative (the scene tree was
// re-read as a Value before the merge). A yyjson document keeps a number at the width it was read or
// written with, so the typed writer's uint64 stays a uint64 through the merge and into the file.
//
// The whole flow is: the loader parses the text ONCE into a TextDocument (and reads its typed tree off that
// same document); the saver writes its typed tree with the typed writer, parses that as a TextDocument and
// merges the loaded one onto it (MergeCarried); the result is written canonically once.

#include <Common/Json/Json.hpp>

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace Common::Json
{
    // Asked about one key at one level: does THIS build state it? A key it states and did not write was
    // deleted by this build and must not come back; a key it does not state belongs to another build.
    using KeyIsOurs = std::function<bool( const std::string& )>;

    struct CarryRule;

    // A parsed JSON text, kept as parsed: every member in file order, every number at its own width.
    // Immutable and cheap to copy (the parse is shared).
    class TextDocument
    {
    public:
        // An empty object (`{}`) - what ResultStr needs to hold one; every real document comes from Parse.
        TextDocument();

        // The text parsed once. Not JSON = an error naming the byte.
        [[nodiscard]] static ResultStr<TextDocument> Parse( std::string_view json );

        // The typed tree read off this document (no second parse). An unknown key is tolerated at every
        // level - refusing it would refuse a file another build wrote, and carrying it is MergeCarried's
        // job; a missing required member or a wrong-typed value is an error.
        template <typename T>
        [[nodiscard]] ResultStr<T> AsDocument() const
        {
            try
            {
                auto parsed = rfl::json::read<T>( rfl::json::InputVarType( yyjson_doc_get_root( m_Doc.get() ) ) );
                if ( !parsed )
                    return MakeError<T>( "document: " + Detail::DescribeReadError( parsed.error().what() ) );
                return MakeSuccess( std::move( parsed.value() ) );
            }
            catch ( const std::exception& e )
            {
                return MakeError<T>( "document: " + Detail::DescribeReadError( e.what() ) );
            }
        }

        // The member names of the root object (`member` empty) or of the root's object member `member`, in
        // file order. Empty when there is no such object.
        [[nodiscard]] std::vector<std::string> KeysAt( std::string_view member = {} ) const;

        // The member names of every object element of the root's array member `arrayMember`, in order.
        [[nodiscard]] std::vector<std::vector<std::string>> RecordKeysAt( std::string_view arrayMember ) const;

        // The document as the writer's one line (the text Json::Write gives for the same values).
        [[nodiscard]] std::string Text() const;

    private:
        explicit TextDocument( std::shared_ptr<yyjson_doc> doc ) : m_Doc( std::move( doc ) )
        {
        }
        friend TextDocument MergeCarried( const TextDocument&, const TextDocument&, const CarryRule& );

        std::shared_ptr<yyjson_doc> m_Doc;
    };

    // How a document's levels answer "is this key ours" during MergeCarried.
    struct CarryRule
    {
        // Asked about the keys of the root object. Every level below answers "not ours".
        KeyIsOurs RootKeyIsOurs;
        // The root's array member whose object elements have IDENTITY (a scene's `Entities`): they are
        // matched on `RecordId` rather than merged as a whole value. Empty = no such array.
        std::string RecordArray;
        std::string RecordId = "id";
        // Asked about the keys of one matched record.
        KeyIsOurs RecordKeyIsOurs;
    };

    // `fresh` (what this build wrote) with every key of `source` (the loaded document) that this build
    // does not state merged back where it stood:
    //   - a key in both: fresh's value wins; two objects are merged one level down, recursively;
    //   - a key only in source: kept, in source's order, unless the level's rule says it is ours;
    //   - a key only in fresh: appended after source's keys;
    //   - arrays are replaced whole, except the RecordArray, whose records are matched on RecordId (an
    //     integer id is matched on its 64 bits, so an id a pre-fix save spelled negative still matches);
    //     a source record whose id is not in fresh is dropped (the entity was deleted), and the first
    //     source record claiming an id is the one matched.
    // A root that is not an object on either side = `fresh` as it is.
    [[nodiscard]] TextDocument MergeCarried( const TextDocument& fresh, const TextDocument& source,
                                             const CarryRule& rule );

    // The document in the canonical file layout (Content/CanonicalText.hpp).
    [[nodiscard]] ResultStr<std::string> WriteCanonical( const TextDocument& document );
} // namespace Common::Json
