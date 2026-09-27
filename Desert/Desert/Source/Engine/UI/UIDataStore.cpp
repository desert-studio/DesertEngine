#include "UIDataStore.hpp"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <string>

namespace Desert::UI
{
    UIDataStore& UIDataStore::Get()
    {
        static UIDataStore store;
        return store;
    }

    UIDataStore::UIDataStore()                                          = default;
    UIDataStore::UIDataStore( UIDataStore&& other ) noexcept            = default;
    UIDataStore& UIDataStore::operator=( UIDataStore&& other ) noexcept = default;
    UIDataStore::~UIDataStore()                                         = default;

    // A record of a collection is itself a store (a bound list inside a row of another), so a deep copy
    // recurses as deep as the data nests; that depth is the authored UI's, not an input's. The collection's
    // half is UICollection::Clone, in its own translation unit (UICollectionClone.cpp).
    UIDataStore::UIDataStore( const UIDataStore& other ) : m_Values( other.m_Values )
    {
        for ( const auto& [key, c] : other.m_Collections )
            m_Collections.emplace( key, c->Clone() );
    }

    UIDataStore& UIDataStore::operator=( const UIDataStore& other )
    {
        if ( this != &other )
        {
            UIDataStore copy( other );
            *this = std::move( copy );
        }
        return *this;
    }

    void UIDataStore::Set( const std::string& key, Value value )
    {
        if ( !key.empty() )
            m_Values[key] = std::move( value );
    }

    void UIDataStore::Erase( const std::string& key )
    {
        m_Values.erase( key );
    }

    void UIDataStore::Clear()
    {
        m_Values.clear();
        m_Collections.clear();
    }

    UICollection& UIDataStore::Collection( const std::string& key )
    {
        auto& slot = m_Collections[key];
        if ( !slot )
            slot = std::make_unique<UICollection>();
        return *slot;
    }

    const UICollection* UIDataStore::FindCollection( const std::string& key ) const
    {
        const auto it = m_Collections.find( key );
        return it != m_Collections.end() ? it->second.get() : nullptr;
    }

    void UIDataStore::EraseCollection( const std::string& key )
    {
        m_Collections.erase( key );
    }

    // --- UICollection ---------------------------------------------------------------------------------

    std::uint64_t UICollection::NextSerial()
    {
        // The UI data store is written and read on the main thread only (gameplay scripts and the canvas
        // walk), like every other member of it.
        static std::uint64_t next = 0;
        return ++next;
    }

    void UICollection::Log( Change::Kind what, int first, int count )
    {
        ++m_Generation;
        m_Log.push_back( Change{ what, first, count, m_Generation } );
        if ( m_Log.size() > kChangeLogDepth )
            m_Log.pop_front();
    }

    int UICollection::Add( UIDataStore record )
    {
        const int index = Size();
        m_Records.push_back( Entry{ std::move( record ), m_NextId++, ++m_Revision } );
        Log( Change::Kind::Insert, index, 1 );
        return index;
    }

    Common::BoolResultStr UICollection::Insert( int index, UIDataStore record )
    {
        if ( index < 0 || index > Size() )
            return Common::MakeError( "UI collection insert at " + std::to_string( index ) + " is outside [0, " +
                                      std::to_string( Size() ) + "]" );
        m_Records.insert( m_Records.begin() + index, Entry{ std::move( record ), m_NextId++, ++m_Revision } );
        Log( Change::Kind::Insert, index, 1 );
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr UICollection::Remove( int index )
    {
        if ( index < 0 || index >= Size() )
            return Common::MakeError( "UI collection remove at " + std::to_string( index ) + " is outside [0, " +
                                      std::to_string( Size() ) + ")" );
        m_Records.erase( m_Records.begin() + index );
        Log( Change::Kind::Remove, index, 1 );
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr UICollection::SetField( int index, const std::string& field, UIDataStore::Value value )
    {
        if ( index < 0 || index >= Size() )
            return Common::MakeError( "UI collection field '" + field + "' at " + std::to_string( index ) +
                                      " is outside [0, " + std::to_string( Size() ) + ")" );
        if ( field.empty() )
            return Common::MakeError( "UI collection field name is empty (record " + std::to_string( index ) +
                                      ")" );
        Entry& e = m_Records[static_cast<std::size_t>( index )];
        e.Fields.Set( field, std::move( value ) );
        e.Revision = ++m_Revision;
        Log( Change::Kind::Set, index, 1 );
        return Common::MakeSuccess( true );
    }

    void UICollection::Clear()
    {
        m_Records.clear();
        Log( Change::Kind::Clear, 0, 0 );
    }

    bool UICollection::ChangesSince( std::uint64_t generation, std::vector<Change>& out ) const
    {
        if ( generation >= m_Generation )
            return true;
        // The oldest change still logged must be the one right after @p generation, or something between
        // them has been dropped and a partial answer would be a wrong one.
        if ( m_Log.empty() || m_Log.front().Generation > generation + 1 )
            return false;
        for ( const Change& c : m_Log )
            if ( c.Generation > generation )
                out.push_back( c );
        return true;
    }

    bool UIDataStore::Has( const std::string& key ) const
    {
        return m_Values.find( key ) != m_Values.end();
    }

    std::optional<double> UIDataStore::Number( const std::string& key ) const
    {
        const auto it = m_Values.find( key );
        if ( it == m_Values.end() )
            return std::nullopt;
        if ( const double* d = std::get_if<double>( &it->second ) )
            return *d;
        if ( const bool* b = std::get_if<bool>( &it->second ) )
            return *b ? 1.0 : 0.0;
        if ( const std::string* s = std::get_if<std::string>( &it->second ) )
        {
            try // a numeric string is a number the author clearly meant
            {
                return std::stod( *s );
            }
            catch ( ... )
            {
                return std::nullopt;
            }
        }
        return std::nullopt;
    }

    std::optional<bool> UIDataStore::Bool( const std::string& key ) const
    {
        const auto it = m_Values.find( key );
        if ( it == m_Values.end() )
            return std::nullopt;
        if ( const bool* b = std::get_if<bool>( &it->second ) )
            return *b;
        if ( const double* d = std::get_if<double>( &it->second ) )
            return *d != 0.0;
        if ( const std::string* s = std::get_if<std::string>( &it->second ) )
        {
            std::string lower = *s;
            std::transform( lower.begin(), lower.end(), lower.begin(),
                            []( unsigned char c ) { return static_cast<char>( std::tolower( c ) ); } );
            if ( lower == "true" || lower == "1" || lower == "yes" )
                return true;
            if ( lower == "false" || lower == "0" || lower == "no" )
                return false;
        }
        return std::nullopt;
    }

    std::optional<std::string> UIDataStore::Text( const std::string& key ) const
    {
        const auto it = m_Values.find( key );
        if ( it == m_Values.end() )
            return std::nullopt;
        if ( const std::string* s = std::get_if<std::string>( &it->second ) )
            return *s;
        if ( const bool* b = std::get_if<bool>( &it->second ) )
            return std::string( *b ? "true" : "false" );
        if ( const double* d = std::get_if<double>( &it->second ) )
        {
            // Whole numbers read as "42", not "42.000000" — a score or an HP value is the common case.
            char buf[64];
            if ( *d == static_cast<double>( static_cast<long long>( *d ) ) )
                std::snprintf( buf, sizeof( buf ), "%lld", static_cast<long long>( *d ) );
            else
                std::snprintf( buf, sizeof( buf ), "%g", *d );
            return std::string( buf );
        }
        return std::nullopt;
    }

    std::optional<glm::vec3> UIDataStore::Color( const std::string& key ) const
    {
        const auto it = m_Values.find( key );
        if ( it == m_Values.end() )
            return std::nullopt;
        if ( const glm::vec3* c = std::get_if<glm::vec3>( &it->second ) )
            return *c;
        return std::nullopt;
    }
} // namespace Desert::UI

namespace Desert::UI
{
    UIMessageQueue& UIMessageQueue::Get()
    {
        static UIMessageQueue queue;
        return queue;
    }

    void UIMessageQueue::Push( std::string message )
    {
        if ( message.empty() )
            return;
        // A UI can only produce so many events per frame; the cap is purely a runaway guard (a script
        // that answers a message by raising another would otherwise grow this without bound).
        constexpr size_t kMax = 256;
        if ( m_Messages.size() < kMax )
            m_Messages.push_back( std::move( message ) );
    }

    std::vector<std::string> UIMessageQueue::Drain()
    {
        std::vector<std::string> out;
        out.swap( m_Messages );
        return out;
    }

    void UIMessageQueue::Clear()
    {
        m_Messages.clear();
    }
} // namespace Desert::UI
