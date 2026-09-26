#include "Carry.hpp"

#include <Common/Content/CanonicalText.hpp>

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>

namespace Common::Json
{
    namespace
    {
        struct MutDocFree
        {
            void operator()( yyjson_mut_doc* doc ) const
            {
                yyjson_mut_doc_free( doc );
            }
        };
        using MutDoc = std::unique_ptr<yyjson_mut_doc, MutDocFree>;

        std::shared_ptr<yyjson_doc> Own( yyjson_doc* doc )
        {
            return { doc, []( yyjson_doc* d ) { yyjson_doc_free( d ); } };
        }

        std::string KeyOf( yyjson_val* key )
        {
            return { yyjson_get_str( key ), yyjson_get_len( key ) };
        }

        // An allocation yyjson could not make is the same condition a std::string throws on; it is not a
        // state a merge can continue from, so it is not reported as a value.
        template <typename P>
        P* Allocated( P* pointer )
        {
            if ( pointer == nullptr )
                throw std::bad_alloc();
            return pointer;
        }

        yyjson_mut_val* Copy( yyjson_mut_doc* doc, yyjson_val* value )
        {
            return Allocated( yyjson_val_mut_copy( doc, value ) );
        }

        void Add( yyjson_mut_doc* doc, yyjson_mut_val* object, yyjson_val* key, yyjson_mut_val* value )
        {
            yyjson_mut_val* name =
                 Allocated( yyjson_mut_strncpy( doc, yyjson_get_str( key ), yyjson_get_len( key ) ) );
            if ( !yyjson_mut_obj_add( object, name, value ) )
                throw std::bad_alloc();
        }

        // A record's identity, as text. An integer is its 64 bits (yyjson reads a positive integer as
        // unsigned and a negative one as signed; the UUID they spell is the same), so a record a pre-fix save
        // wrote with a negative id is still the record this build wrote positive.
        std::string RecordIdentity( yyjson_val* record, const std::string& idMember )
        {
            yyjson_val* id = yyjson_obj_getn( record, idMember.data(), idMember.size() );
            if ( id == nullptr )
                return {};
            if ( yyjson_is_uint( id ) )
                return std::to_string( yyjson_get_uint( id ) );
            if ( yyjson_is_sint( id ) )
                return std::to_string( static_cast<std::uint64_t>( yyjson_get_sint( id ) ) );
            if ( yyjson_is_str( id ) )
                return KeyOf( id );
            return {};
        }

        bool NotOurs( const std::string& )
        {
            return false;
        }

        // One object and every object nested under it, merged (the rule is on MergeCarried). `ours` and
        // `replacedKey` answer for the top level only; every nested level answers "not ours". `replacedKey`,
        // when `replacement` is set, is the member whose merged value was computed by the caller (the record
        // array).
        //
        // An explicit stack rather than recursion, as the reflected reader does: a nested object is added to
        // its parent at once - empty, so its key keeps its place in member order - and filled when its own
        // frame is taken, so the nesting depth of the file costs heap, not the call stack.
        yyjson_mut_val* MergeLevel( yyjson_mut_doc* doc, yyjson_val* fresh, yyjson_val* source,
                                    const KeyIsOurs& ours, std::string_view replacedKey,
                                    yyjson_mut_val* replacement )
        {
            struct Frame
            {
                yyjson_val*     Fresh;
                yyjson_val*     Source;
                yyjson_mut_val* Merged;
                bool            Top;
            };
            yyjson_mut_val*    root = Allocated( yyjson_mut_obj( doc ) );
            std::vector<Frame> stack{ Frame{ fresh, source, root, true } };
            while ( !stack.empty() )
            {
                const Frame frame = stack.back();
                stack.pop_back();
                const bool replacing = frame.Top && replacement != nullptr;

                yyjson_val*     key = nullptr;
                yyjson_obj_iter iter;
                yyjson_obj_iter_init( frame.Source, &iter );
                while ( ( key = yyjson_obj_iter_next( &iter ) ) != nullptr )
                {
                    yyjson_val* sourceValue = yyjson_obj_iter_get_val( key );
                    yyjson_val* inFresh =
                         yyjson_obj_getn( frame.Fresh, yyjson_get_str( key ), yyjson_get_len( key ) );
                    if ( inFresh == nullptr )
                    {
                        if ( !frame.Top || !ours( KeyOf( key ) ) )
                            Add( doc, frame.Merged, key, Copy( doc, sourceValue ) );
                        continue;
                    }
                    if ( replacing && KeyOf( key ) == replacedKey )
                    {
                        Add( doc, frame.Merged, key, replacement );
                    }
                    else if ( yyjson_is_obj( inFresh ) && yyjson_is_obj( sourceValue ) )
                    {
                        yyjson_mut_val* nested = Allocated( yyjson_mut_obj( doc ) );
                        Add( doc, frame.Merged, key, nested );
                        stack.push_back( Frame{ inFresh, sourceValue, nested, false } );
                    }
                    else
                    {
                        Add( doc, frame.Merged, key, Copy( doc, inFresh ) );
                    }
                }

                yyjson_obj_iter_init( frame.Fresh, &iter );
                while ( ( key = yyjson_obj_iter_next( &iter ) ) != nullptr )
                {
                    if ( yyjson_obj_getn( frame.Source, yyjson_get_str( key ), yyjson_get_len( key ) ) != nullptr )
                        continue;
                    const bool replaced = replacing && KeyOf( key ) == replacedKey;
                    Add( doc, frame.Merged, key,
                         replaced ? replacement : Copy( doc, yyjson_obj_iter_get_val( key ) ) );
                }
            }
            return root;
        }

        // The record array merged record by record: each fresh record matched to the source record with
        // its id (a hash map, so a world of N records costs N lookups, not N^2), unmatched fresh records
        // written as they are, unmatched source records dropped.
        yyjson_mut_val* MergeRecords( yyjson_mut_doc* doc, yyjson_val* fresh, yyjson_val* source,
                                      const CarryRule& rule )
        {
            std::unordered_map<std::string, yyjson_val*> sourceById;
            std::size_t                                  index  = 0;
            std::size_t                                  count  = 0;
            yyjson_val*                                  record = nullptr;
            yyjson_arr_foreach( source, index, count, record )
            {
                if ( !yyjson_is_obj( record ) )
                    continue;
                if ( std::string identity = RecordIdentity( record, rule.RecordId ); !identity.empty() )
                    sourceById.emplace( std::move( identity ), record );
            }

            const KeyIsOurs& ours   = rule.RecordKeyIsOurs ? rule.RecordKeyIsOurs : KeyIsOurs( NotOurs );
            yyjson_mut_val*  merged = Allocated( yyjson_mut_arr( doc ) );
            yyjson_arr_foreach( fresh, index, count, record )
            {
                yyjson_mut_val* element = nullptr;
                const auto      match   = yyjson_is_obj( record )
                                               ? sourceById.find( RecordIdentity( record, rule.RecordId ) )
                                               : sourceById.end();
                if ( match == sourceById.end() || match->first.empty() )
                    element = Copy( doc, record );
                else
                    element = MergeLevel( doc, record, match->second, ours, {}, nullptr );
                if ( !yyjson_mut_arr_append( merged, element ) )
                    throw std::bad_alloc();
            }
            return merged;
        }
    } // namespace

    TextDocument::TextDocument()
    {
        static const char kEmpty[] = "{}";
        m_Doc                      = Own( Allocated( yyjson_read( kEmpty, sizeof( kEmpty ) - 1, 0 ) ) );
    }

    ResultStr<TextDocument> TextDocument::Parse( std::string_view json )
    {
        // yyjson_read_opts takes a mutable buffer (it writes into it only in insitu mode, which is off here);
        // a private copy gives it one without casting const away from the caller's text.
        std::string     buffer( json );
        yyjson_read_err error{};
        yyjson_doc*     doc = yyjson_read_opts( buffer.data(), buffer.size(), 0, nullptr, &error );
        if ( doc == nullptr )
            return MakeError<TextDocument>( "document: not JSON at byte " + std::to_string( error.pos ) + ": " +
                                            ( error.msg != nullptr ? error.msg : "unknown error" ) );
        return MakeSuccess( TextDocument( Own( doc ) ) );
    }

    std::vector<std::string> TextDocument::KeysAt( std::string_view member ) const
    {
        yyjson_val* level = yyjson_doc_get_root( m_Doc.get() );
        if ( !member.empty() && yyjson_is_obj( level ) )
            level = yyjson_obj_getn( level, member.data(), member.size() );
        std::vector<std::string> keys;
        if ( !yyjson_is_obj( level ) )
            return keys;
        yyjson_val*     key = nullptr;
        yyjson_obj_iter iter;
        yyjson_obj_iter_init( level, &iter );
        while ( ( key = yyjson_obj_iter_next( &iter ) ) != nullptr )
            keys.push_back( KeyOf( key ) );
        return keys;
    }

    std::vector<std::vector<std::string>> TextDocument::RecordKeysAt( std::string_view arrayMember ) const
    {
        yyjson_val* root = yyjson_doc_get_root( m_Doc.get() );
        yyjson_val* array =
             yyjson_is_obj( root ) ? yyjson_obj_getn( root, arrayMember.data(), arrayMember.size() ) : nullptr;
        std::vector<std::vector<std::string>> records;
        if ( !yyjson_is_arr( array ) )
            return records;
        std::size_t index  = 0;
        std::size_t count  = 0;
        yyjson_val* record = nullptr;
        yyjson_arr_foreach( array, index, count, record )
        {
            if ( !yyjson_is_obj( record ) )
                continue;
            auto&           keys = records.emplace_back();
            yyjson_val*     key  = nullptr;
            yyjson_obj_iter iter;
            yyjson_obj_iter_init( record, &iter );
            while ( ( key = yyjson_obj_iter_next( &iter ) ) != nullptr )
                keys.push_back( KeyOf( key ) );
        }
        return records;
    }

    std::string TextDocument::Text() const
    {
        std::size_t length = 0;
        char*       text   = Allocated( yyjson_write( m_Doc.get(), YYJSON_WRITE_NOFLAG, &length ) );
        // yyjson_write hands back a malloc'd buffer; it is owned here so a throwing std::string copy frees it too.
        const std::unique_ptr<char, decltype( &std::free )> owned( text, &std::free );
        std::string                                         out( owned.get(), length );
        return out;
    }

    TextDocument MergeCarried( const TextDocument& fresh, const TextDocument& source, const CarryRule& rule )
    {
        yyjson_val* freshRoot  = yyjson_doc_get_root( fresh.m_Doc.get() );
        yyjson_val* sourceRoot = yyjson_doc_get_root( source.m_Doc.get() );
        if ( !yyjson_is_obj( freshRoot ) || !yyjson_is_obj( sourceRoot ) )
            return fresh;

        const MutDoc     doc( Allocated( yyjson_mut_doc_new( nullptr ) ) );
        const KeyIsOurs& ours = rule.RootKeyIsOurs ? rule.RootKeyIsOurs : KeyIsOurs( NotOurs );

        yyjson_val* freshRecords  = nullptr;
        yyjson_val* sourceRecords = nullptr;
        if ( !rule.RecordArray.empty() )
        {
            freshRecords  = yyjson_obj_getn( freshRoot, rule.RecordArray.data(), rule.RecordArray.size() );
            sourceRecords = yyjson_obj_getn( sourceRoot, rule.RecordArray.data(), rule.RecordArray.size() );
        }
        yyjson_mut_val* records = nullptr;
        if ( yyjson_is_arr( freshRecords ) && yyjson_is_arr( sourceRecords ) )
            records = MergeRecords( doc.get(), freshRecords, sourceRecords, rule );

        yyjson_mut_doc_set_root( doc.get(),
                                 MergeLevel( doc.get(), freshRoot, sourceRoot, ours, rule.RecordArray, records ) );
        return TextDocument( Own( Allocated( yyjson_mut_doc_imut_copy( doc.get(), nullptr ) ) ) );
    }

    ResultStr<std::string> WriteCanonical( const TextDocument& document )
    {
        return Content::CanonicalJsonTextOfWriterOutput( document.Text() );
    }
} // namespace Common::Json
