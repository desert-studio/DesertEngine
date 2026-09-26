#include "ChunkSchemeSession.hpp"

#include <Common/Utilities/FileSystem.hpp>

#include <algorithm>
#include <format>
#include <utility>

namespace Common::Content
{
    namespace
    {
        bool SameScheme( const ChunkScheme& a, const ChunkScheme& b )
        {
            if ( a.AlwaysBase != b.AlwaysBase || a.Chunks.size() != b.Chunks.size() )
                return false;
            for ( std::size_t i = 0; i < a.Chunks.size(); ++i )
                if ( a.Chunks[i].Name != b.Chunks[i].Name || a.Chunks[i].Roots != b.Chunks[i].Roots )
                    return false;
            return true;
        }

        // Only the name half of ValidateChunkScheme, for one chunk inside a draft: the roots half is the
        // save's to judge, because a chunk is born without roots.
        BoolResultStr CheckName( const ChunkScheme& draft, const std::string& name, std::size_t self )
        {
            ChunkScheme probe;
            probe.Chunks.push_back( ChunkRule{ name, { "_" } } );
            if ( const auto valid = ValidateChunkScheme( probe ); !valid )
                return valid;
            for ( std::size_t i = 0; i < draft.Chunks.size(); ++i )
                if ( i != self && draft.Chunks[i].Name == name )
                    return MakeFormattedError<bool>( "two chunks are called '{}'", name );
            return MakeSuccess( true );
        }
    } // namespace

    ChunkSchemeSession::ChunkSchemeSession( std::filesystem::path path ) : m_Path( std::move( path ) )
    {
        Reload();
    }

    const std::filesystem::path& ChunkSchemeSession::Path() const
    {
        return m_Path;
    }

    ChunkSchemeSession::Status ChunkSchemeSession::GetStatus() const
    {
        return m_Status;
    }

    const std::string& ChunkSchemeSession::LoadMessage() const
    {
        return m_LoadMessage;
    }

    const std::string& ChunkSchemeSession::LastAction() const
    {
        return m_LastAction;
    }

    const ChunkScheme& ChunkSchemeSession::Saved() const
    {
        return m_Saved;
    }

    const ChunkScheme& ChunkSchemeSession::Draft() const
    {
        return m_Draft;
    }

    bool ChunkSchemeSession::Dirty() const
    {
        return !SameScheme( m_Saved, m_Draft );
    }

    std::size_t ChunkSchemeSession::Revision() const
    {
        return m_Revision;
    }

    void ChunkSchemeSession::Reload()
    {
        ++m_Revision;
        m_Saved     = {};
        m_Draft     = {};
        auto loaded = LoadChunkScheme( m_Path );
        if ( !loaded )
        {
            m_Status      = Utils::FileSystem::Exists( m_Path ) ? Status::Unreadable : Status::Missing;
            m_LoadMessage = loaded.GetError();
            return;
        }
        m_Status      = Status::Loaded;
        m_Saved       = loaded.GetValue();
        m_Draft       = m_Saved;
        m_LoadMessage = m_Saved.Chunks.empty()
                             ? std::format( "{}: one archive (no chunks declared)", m_Path.string() )
                             : std::format( "{}: {} chunk(s) besides {}, {} pinned to {}", m_Path.string(),
                                            m_Saved.Chunks.size(), BASE_CHUNK_NAME, m_Saved.AlwaysBase.size(),
                                            BASE_CHUNK_NAME );
    }

    BoolResultStr ChunkSchemeSession::Record( BoolResultStr result, const std::string& done )
    {
        m_LastAction = result ? done : result.GetError();
        if ( result )
            ++m_Revision;
        return result;
    }

    BoolResultStr ChunkSchemeSession::RequireLoaded( const char* action )
    {
        if ( m_Status == Status::Loaded )
            return MakeSuccess( true );
        return Record( MakeFormattedError<bool>( "cannot {}: {}", action, m_LoadMessage ), "" );
    }

    BoolResultStr ChunkSchemeSession::CreateDefault()
    {
        auto written = WriteDefaultChunkScheme( m_Path );
        if ( written )
            Reload();
        return Record( std::move( written ), "Wrote the one-archive scheme to " + m_Path.string() );
    }

    BoolResultStr ChunkSchemeSession::AddChunk( const std::string& name )
    {
        if ( auto ok = RequireLoaded( "add a chunk" ); !ok )
            return ok;
        if ( auto ok = CheckName( m_Draft, name, m_Draft.Chunks.size() ); !ok )
            return Record( std::move( ok ), "" );
        m_Draft.Chunks.push_back( ChunkRule{ name, {} } );
        return Record( MakeSuccess( true ), "Added chunk '" + name + "' (unsaved; it needs a root)" );
    }

    BoolResultStr ChunkSchemeSession::RemoveChunk( std::size_t chunk )
    {
        if ( auto ok = RequireLoaded( "remove a chunk" ); !ok )
            return ok;
        if ( chunk >= m_Draft.Chunks.size() )
            return Record( MakeFormattedError<bool>( "there is no chunk #{} (the scheme has {})", chunk,
                                                     m_Draft.Chunks.size() ),
                           "" );
        const std::string name = m_Draft.Chunks[chunk].Name;
        m_Draft.Chunks.erase( m_Draft.Chunks.begin() + static_cast<std::ptrdiff_t>( chunk ) );
        return Record( MakeSuccess( true ), "Removed chunk '" + name + "' (unsaved)" );
    }

    BoolResultStr ChunkSchemeSession::RenameChunk( std::size_t chunk, const std::string& name )
    {
        if ( auto ok = RequireLoaded( "rename a chunk" ); !ok )
            return ok;
        if ( chunk >= m_Draft.Chunks.size() )
            return Record( MakeFormattedError<bool>( "there is no chunk #{} (the scheme has {})", chunk,
                                                     m_Draft.Chunks.size() ),
                           "" );
        if ( auto ok = CheckName( m_Draft, name, chunk ); !ok )
            return Record( std::move( ok ), "" );
        const std::string old      = m_Draft.Chunks[chunk].Name;
        m_Draft.Chunks[chunk].Name = name;
        return Record( MakeSuccess( true ), "Renamed chunk '" + old + "' to '" + name + "' (unsaved)" );
    }

    BoolResultStr ChunkSchemeSession::AddRoot( std::size_t chunk, const std::string& key )
    {
        if ( auto ok = RequireLoaded( "add a root" ); !ok )
            return ok;
        if ( chunk >= m_Draft.Chunks.size() )
            return Record( MakeFormattedError<bool>( "there is no chunk #{} (the scheme has {})", chunk,
                                                     m_Draft.Chunks.size() ),
                           "" );
        ChunkRule& rule = m_Draft.Chunks[chunk];
        if ( key.empty() )
            return Record( MakeFormattedError<bool>( "chunk '{}': a root is a stable key, and an empty one names "
                                                     "no asset",
                                                     rule.Name ),
                           "" );
        if ( std::find( rule.Roots.begin(), rule.Roots.end(), key ) != rule.Roots.end() )
            return Record( MakeFormattedError<bool>( "chunk '{}' is already rooted at '{}'", rule.Name, key ),
                           "" );
        rule.Roots.push_back( key );
        return Record( MakeSuccess( true ), "Rooted chunk '" + rule.Name + "' at '" + key + "' (unsaved)" );
    }

    BoolResultStr ChunkSchemeSession::RemoveRoot( std::size_t chunk, std::size_t root )
    {
        if ( auto ok = RequireLoaded( "remove a root" ); !ok )
            return ok;
        if ( chunk >= m_Draft.Chunks.size() || root >= m_Draft.Chunks[chunk].Roots.size() )
            return Record( MakeFormattedError<bool>( "there is no root #{} in chunk #{}", root, chunk ), "" );
        ChunkRule&        rule = m_Draft.Chunks[chunk];
        const std::string key  = rule.Roots[root];
        rule.Roots.erase( rule.Roots.begin() + static_cast<std::ptrdiff_t>( root ) );
        return Record( MakeSuccess( true ), "Removed root '" + key + "' from '" + rule.Name + "' (unsaved)" );
    }

    BoolResultStr ChunkSchemeSession::AddPin( const std::string& key )
    {
        if ( auto ok = RequireLoaded( "pin a key" ); !ok )
            return ok;
        if ( key.empty() )
            return Record( MakeError<bool>( "a pin is a stable key, and an empty one names no asset" ), "" );
        if ( std::find( m_Draft.AlwaysBase.begin(), m_Draft.AlwaysBase.end(), key ) != m_Draft.AlwaysBase.end() )
            return Record( MakeFormattedError<bool>( "'{}' is already pinned to {}", key, BASE_CHUNK_NAME ), "" );
        m_Draft.AlwaysBase.push_back( key );
        return Record( MakeSuccess( true ),
                       "Pinned '" + key + "' to " + std::string( BASE_CHUNK_NAME ) + " (unsaved)" );
    }

    BoolResultStr ChunkSchemeSession::RemovePin( std::size_t pin )
    {
        if ( auto ok = RequireLoaded( "unpin a key" ); !ok )
            return ok;
        if ( pin >= m_Draft.AlwaysBase.size() )
            return Record( MakeFormattedError<bool>( "there is no pin #{} (the scheme has {})", pin,
                                                     m_Draft.AlwaysBase.size() ),
                           "" );
        const std::string key = m_Draft.AlwaysBase[pin];
        m_Draft.AlwaysBase.erase( m_Draft.AlwaysBase.begin() + static_cast<std::ptrdiff_t>( pin ) );
        return Record( MakeSuccess( true ), "Unpinned '" + key + "' (unsaved)" );
    }

    BoolResultStr ChunkSchemeSession::Save( const Utils::AssetRegistry& registry )
    {
        if ( auto ok = RequireLoaded( "save" ); !ok )
            return ok;
        auto saved = SaveChunkScheme( m_Path, m_Draft, registry );
        if ( !saved )
            return Record( std::move( saved ), "" );
        Reload();
        return Record( std::move( saved ), "Saved " + m_Path.string() );
    }

    void ChunkSchemeSession::Revert()
    {
        m_Draft = m_Saved;
        Record( MakeSuccess( true ), "Discarded the unsaved edits to " + m_Path.string() );
    }
} // namespace Common::Content
