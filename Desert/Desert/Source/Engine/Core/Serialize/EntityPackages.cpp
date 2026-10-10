#include <Engine/Core/Serialize/EntityPackages.hpp>

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <unordered_set>

namespace Desert::Core
{
    namespace
    {
        std::uint64_t Bits( Common::UUID id )
        {
            return static_cast<std::uint64_t>( id );
        }

        void SortAscending( std::vector<Common::UUID>& ids )
        {
            std::sort( ids.begin(), ids.end(),
                       []( Common::UUID a, Common::UUID b ) { return Bits( a ) < Bits( b ); } );
        }
    } // namespace

    EntityPackages::Revision EntityPackages::CurrentOf( std::uint64_t id ) const
    {
        const auto found = m_Current.find( id );
        return found == m_Current.end() ? 0 : found->second;
    }

    EntityPackages::Stamp EntityPackages::Touch( Common::UUID id )
    {
        Stamp stamp{ id, CurrentOf( Bits( id ) ), m_Next++ };
        m_Current[Bits( id )] = stamp.After;
        return stamp;
    }

    void EntityPackages::Restore( Common::UUID id, Revision revision )
    {
        m_Current[Bits( id )] = revision;
    }

    void EntityPackages::MarkModified( Common::UUID id )
    {
        m_Unrecorded.insert( Bits( id ) );
    }

    void EntityPackages::TouchAll()
    {
        m_Whole = true;
    }

    void EntityPackages::Forget()
    {
        m_Current.clear();
        m_Unrecorded.clear();
        m_Whole = false;
        m_BaselinePath.reset();
        m_SavedRevision.clear();
        m_SavedRecords.clear();
        m_NotLoaded.clear();
    }

    void EntityPackages::AdoptRegion( std::span<const LiveEntity> live, std::span<const Common::UUID> notLoaded )
    {
        std::unordered_set<std::uint64_t> alive;
        for ( const LiveEntity& entity : live )
        {
            const std::uint64_t id = Bits( entity.Id );
            alive.insert( id );
            if ( m_SavedRevision.contains( id ) )
                continue;
            // Just read from its file: what the file holds is what the entity is.
            m_SavedRevision[id] = CurrentOf( id );
            if ( Bits( entity.Record ) == id )
                m_SavedRecords[id] = SavedRecord{ Bits( entity.Parent ), entity.SiblingIndex };
        }
        std::erase_if( m_SavedRevision, [&]( const auto& row ) { return !alive.contains( row.first ); } );
        std::erase_if( m_SavedRecords, [&]( const auto& row ) { return !alive.contains( row.first ); } );
        m_NotLoaded.clear();
        for ( const Common::UUID id : notLoaded )
            m_NotLoaded.insert( Bits( id ) );
    }

    void EntityPackages::Baseline( const std::filesystem::path& scenePath, std::span<const LiveEntity> live )
    {
        m_Whole        = false;
        m_Unrecorded.clear();
        m_BaselinePath = scenePath.lexically_normal();
        m_SavedRevision.clear();
        m_SavedRecords.clear();
        for ( const LiveEntity& entity : live )
        {
            m_SavedRevision[Bits( entity.Id )] = CurrentOf( Bits( entity.Id ) );
            if ( Bits( entity.Record ) == Bits( entity.Id ) )
                m_SavedRecords[Bits( entity.Id )] = SavedRecord{ Bits( entity.Parent ), entity.SiblingIndex };
        }
    }

    bool EntityPackages::IsDirty( Common::UUID id ) const
    {
        if ( m_Unrecorded.contains( Bits( id ) ) )
            return true;
        const auto saved = m_SavedRevision.find( Bits( id ) );
        return saved == m_SavedRevision.end() || saved->second != CurrentOf( Bits( id ) );
    }

    PackageSavePlan EntityPackages::Plan( const std::filesystem::path& scenePath, std::span<const LiveEntity> live,
                                          bool partitioned ) const
    {
        PackageSavePlan plan;
        for ( const LiveEntity& entity : live )
            if ( Bits( entity.Record ) == Bits( entity.Id ) )
                plan.Listed.push_back( entity.Id );
        SortAscending( plan.Listed );
        std::vector<Common::UUID> loaded = plan.Listed;
        for ( const std::uint64_t id : m_NotLoaded )
        {
            plan.NotLoaded.emplace_back( id );
            plan.Listed.emplace_back( id );
        }
        SortAscending( plan.NotLoaded );
        SortAscending( plan.Listed );

        std::unordered_set<std::uint64_t> alive;
        for ( const LiveEntity& entity : live )
            alive.insert( Bits( entity.Id ) );
        // The records of the baseline that are gone: their files are to be deleted.
        auto goneRecords = [&]
        {
            std::vector<Common::UUID> gone;
            for ( const auto& [id, saved] : m_SavedRecords )
                if ( !alive.contains( id ) && !m_NotLoaded.contains( id ) )
                    gone.emplace_back( id );
            SortAscending( gone );
            return gone;
        };

        auto whole = [&]( const char* reason )
        {
            plan.WholeReason = reason;
            plan.Changed     = loaded;
            plan.Removed.clear();
            if ( m_NotLoaded.empty() )
            {
                plan.Whole = true;
                return plan;
            }
            // A world an editor region holds in part (WP19): "whole" is every LOADED record, written as a delta
            // so the records left on disk keep their files and their place in the header.
            plan.Whole = false;
            if ( !partitioned || !m_BaselinePath || *m_BaselinePath != scenePath.lexically_normal() )
                plan.Refusal = fmt::format( "{} of its {} entities are not loaded (an editor region left them on disk), "
                                            "and a save that is not a delta of the world's own files would drop them. Load "
                                            "the whole world first. Nothing was written.",
                                            m_NotLoaded.size(), plan.Listed.size() );
            else
                plan.Removed = goneRecords();
            return plan;
        };
        if ( !partitioned )
            return whole( "the scene is not partitioned" );
        if ( !m_BaselinePath )
            return whole( "no baseline: the scene was not opened from or saved to its files" );
        if ( *m_BaselinePath != scenePath.lexically_normal() )
            return whole( "the save goes to a file other than the baseline's" );
        if ( m_Whole )
            return whole( "an edit was recorded without naming its entities" );

        std::unordered_set<std::uint64_t> changed;
        for ( const LiveEntity& entity : live )
        {
            if ( IsDirty( entity.Id ) )
                changed.insert( Bits( entity.Record ) );
            if ( Bits( entity.Record ) != Bits( entity.Id ) )
            {
                // A record that stopped being one (its entity was taken into a prefab instance): its file goes,
                // and the record that now states it is rewritten.
                if ( m_SavedRecords.contains( Bits( entity.Id ) ) )
                {
                    plan.Removed.push_back( entity.Id );
                    changed.insert( Bits( entity.Record ) );
                }
                continue;
            }
            const auto saved = m_SavedRecords.find( Bits( entity.Id ) );
            if ( saved == m_SavedRecords.end() || saved->second.Parent != Bits( entity.Parent ) ||
                 saved->second.SiblingIndex != entity.SiblingIndex )
                changed.insert( Bits( entity.Id ) );
        }

        // An entity of the baseline that is gone: a record is a file to delete; anything else was stated by a
        // record that can no longer be named from it.
        for ( const auto& [id, revision] : m_SavedRevision )
        {
            if ( alive.contains( id ) )
                continue;
            if ( !m_SavedRecords.contains( id ) )
                return whole( "an entity inside a prefab instance or a foliage field is gone" );
            plan.Removed.emplace_back( id );
        }
        SortAscending( plan.Removed );

        plan.Whole       = false;
        plan.WholeReason = "";
        for ( const Common::UUID id : loaded )
            if ( changed.contains( Bits( id ) ) )
                plan.Changed.push_back( id );
        return plan;
    }

    Common::ResultStr<PackageSaveOutcome> SaveThroughPackages( const std::filesystem::path& scenePath,
                                                               EntityPackages&              packages,
                                                               std::span<const LiveEntity> live, bool partitioned,
                                                               const ComposeScene& compose, CleanCheck check )
    {
        PackageSaveOutcome    outcome;
        const PackageSavePlan plan = packages.Plan( scenePath, live, partitioned );
        outcome.Whole              = plan.Whole;
        if ( !plan.Refusal.empty() )
            return Common::MakeError<PackageSaveOutcome>( fmt::format( "'{}': {}", scenePath.string(), plan.Refusal ) );
        std::unordered_set<std::uint64_t> notLoaded;
        for ( const Common::UUID id : plan.NotLoaded )
            notLoaded.insert( Bits( id ) );

        std::unordered_set<std::uint64_t> only;
        for ( const Common::UUID id : plan.Changed )
            only.insert( Bits( id ) );
        // The check composes every record (WriteSceneDelta ignores the ones it is not asked to write), so the
        // clean ones can be laid against their files before anything is written.
        const bool proveClean = !plan.Whole && check == CleanCheck::AgainstFiles;
        auto       document   = compose( plan.Whole || proveClean ? nullptr : &only );
        if ( !document )
            return Common::MakeError<PackageSaveOutcome>( document.GetError() );
        if ( proveClean )
        {
            std::vector<Common::UUID> clean;
            for ( const Common::UUID id : plan.Listed )
                if ( !only.contains( Bits( id ) ) && !notLoaded.contains( Bits( id ) ) )
                    clean.push_back( id );
            if ( auto verified = ExternalEntities::VerifyCleanRecords( scenePath, document.GetValue(), clean );
                 !verified )
                return Common::MakeError<PackageSaveOutcome>( verified.GetError() );
        }
        if ( auto records = document.GetValue().RecordsAt( ExternalEntities::kRecords ) )
            outcome.Serialized = records.GetValue().size();

        auto written = plan.Whole ? ExternalEntities::WriteSceneFile( scenePath, document.GetValue() )
                                  : ExternalEntities::WriteSceneDelta( scenePath, document.GetValue(), plan.Listed,
                                                                       plan.Changed, plan.Removed );
        if ( !written )
            return Common::MakeError<PackageSaveOutcome>( written.GetError() );
        outcome.Files = written.GetValue();
        packages.Baseline( scenePath, live );
        return Common::MakeSuccess( outcome );
    }
} // namespace Desert::Core
