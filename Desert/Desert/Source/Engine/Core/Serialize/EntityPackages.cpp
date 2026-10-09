#include <Engine/Core/Serialize/EntityPackages.hpp>

#include <algorithm>

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

    void EntityPackages::TouchAll()
    {
        m_Whole = true;
    }

    void EntityPackages::Forget()
    {
        m_Current.clear();
        m_Whole = false;
        m_BaselinePath.reset();
        m_SavedRevision.clear();
        m_SavedRecords.clear();
    }

    void EntityPackages::Baseline( const std::filesystem::path& scenePath, std::span<const LiveEntity> live )
    {
        m_Whole        = false;
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

        auto whole = [&]( const char* reason )
        {
            plan.Whole       = true;
            plan.WholeReason = reason;
            plan.Changed     = plan.Listed;
            plan.Removed.clear();
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

        std::unordered_set<std::uint64_t> alive;
        std::unordered_set<std::uint64_t> changed;
        for ( const LiveEntity& entity : live )
        {
            alive.insert( Bits( entity.Id ) );
            if ( IsDirty( entity.Id ) )
                changed.insert( Bits( entity.Record ) );
            if ( Bits( entity.Record ) != Bits( entity.Id ) )
                continue;
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
        for ( const Common::UUID id : plan.Listed )
            if ( changed.contains( Bits( id ) ) )
                plan.Changed.push_back( id );
        return plan;
    }

    Common::ResultStr<PackageSaveOutcome> SaveThroughPackages( const std::filesystem::path& scenePath,
                                                               EntityPackages&              packages,
                                                               std::span<const LiveEntity> live, bool partitioned,
                                                               const ComposeScene& compose )
    {
        PackageSaveOutcome    outcome;
        const PackageSavePlan plan = packages.Plan( scenePath, live, partitioned );
        outcome.Whole              = plan.Whole;

        std::unordered_set<std::uint64_t> only;
        for ( const Common::UUID id : plan.Changed )
            only.insert( Bits( id ) );
        auto document = compose( plan.Whole ? nullptr : &only );
        if ( !document )
            return Common::MakeError<PackageSaveOutcome>( document.GetError() );
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
