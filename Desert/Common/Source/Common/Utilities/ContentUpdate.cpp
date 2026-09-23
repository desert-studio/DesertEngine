#include "ContentUpdate.hpp"

#include "FileSystem.hpp"

#include <algorithm>
#include <optional>
#include <system_error>

namespace Common::Utils
{
    namespace
    {
        bool SameBytes( const ContentManifestEntry* a, const ContentManifestEntry* b )
        {
            return a && b && a->Hash == b->Hash && a->Size == b->Size;
        }

        // OBSERVATION ONLY — authorship is deliberately not a parameter. What happened to a file is a
        // fact about three manifests; who is allowed to win is a separate decision, taken in ActionFor.
        // Keeping them apart is what stops the matrix from quietly becoming two matrices.
        ContentFileState Observe( const ContentManifestEntry* recorded, const ContentManifestEntry* disk,
                                  const ContentManifestEntry* incoming )
        {
            if ( !recorded )
                return disk ? ContentFileState::LocallyAdded // never handed over by the source
                            : ContentFileState::SourceAdded; // (incoming must be set: the key came from somewhere)

            if ( !disk )
                return incoming
                            ? ContentFileState::LocallyDeleted // deleting IS an edit
                            : ContentFileState::SourceDeleted; // both sides dropped it; only the record is left

            if ( !incoming )
                return SameBytes( recorded, disk ) ? ContentFileState::SourceDeleted
                                                   : ContentFileState::Conflict; // it removed a file they edited

            if ( SameBytes( disk, recorded ) )
                return SameBytes( incoming, recorded ) ? ContentFileState::Unchanged
                                                       : ContentFileState::SourceUpdated;
            if ( SameBytes( incoming, recorded ) )
                return ContentFileState::LocallyEdited;
            // Both moved. If they landed on the same bytes there is nothing to reconcile — the install
            // already holds the version being published.
            return SameBytes( disk, incoming ) ? ContentFileState::Unchanged : ContentFileState::Conflict;
        }

        // THE ONE PLACE THE TWO CONSUMERS DIFFER, AND IT IS A TABLE. Four of the eight rows are the
        // same for both — the source's own additions, updates and removals are not in dispute, and a
        // file the source has never heard of is nobody's to touch. The four that differ are exactly the
        // four where a local edit exists, which is the definition of the split.
        ContentAction ActionFor( ContentFileState state, ContentAuthorship authorship, bool onDisk,
                                 bool inIncoming )
        {
            switch ( state )
            {
                case ContentFileState::Unchanged:
                    return ContentAction::None;
                case ContentFileState::SourceAdded:
                case ContentFileState::SourceUpdated:
                    return ContentAction::Write;
                case ContentFileState::SourceDeleted:
                    return onDisk ? ContentAction::Remove : ContentAction::None;

                case ContentFileState::LocallyAdded:
                    // Not the source's file in either direction. A game patch does not delete what it
                    // never shipped and has no record of, so this row does NOT split.
                    return ContentAction::None;

                case ContentFileState::LocallyEdited:
                    return authorship == ContentAuthorship::SourceOwned ? ContentAction::Write
                                                                        : ContentAction::None;
                case ContentFileState::LocallyDeleted:
                    return authorship == ContentAuthorship::SourceOwned ? ContentAction::Write
                                                                        : ContentAction::None;
                case ContentFileState::Conflict:
                    if ( authorship == ContentAuthorship::LocallyAuthored )
                        return ContentAction::None; // and the plan refuses; see ContentUpdatePlan::CanApply
                    return inIncoming ? ContentAction::Write : ContentAction::Remove;
            }
            return ContentAction::None;
        }

        // A key must land INSIDE the install. Manifests come off disk and out of archives, and a key
        // spelled "../../etc/x" would otherwise make an update a write anywhere the process can reach.
        bool KeyStaysInside( const std::string& key )
        {
            if ( key.empty() )
                return false;
            const std::filesystem::path rel( key );
            if ( rel.is_absolute() )
                return false;
            for ( const auto& part : rel )
                if ( part == ".." )
                    return false;
            return true;
        }
    } // namespace

    bool ContentUpdatePlan::CanApply() const
    {
        return Conflicts.empty();
    }

    size_t ContentUpdatePlan::CountOf( ContentFileState state ) const
    {
        return static_cast<size_t>( std::count_if(
             Steps.begin(), Steps.end(), [state]( const ContentUpdateStep& s ) { return s.State == state; } ) );
    }

    const ContentUpdateStep* ContentUpdatePlan::Find( const std::string& key ) const
    {
        const auto at = std::find_if( Steps.begin(), Steps.end(),
                                      [&key]( const ContentUpdateStep& s ) { return s.Key == key; } );
        return at == Steps.end() ? nullptr : &*at;
    }

    bool ContentUpdatePlan::WithholdRemoval( const std::string& key )
    {
        const auto at = std::find_if( Steps.begin(), Steps.end(),
                                      [&key]( const ContentUpdateStep& s ) { return s.Key == key; } );
        if ( at == Steps.end() || at->Action != ContentAction::Remove )
            return false;
        // The STATE is untouched: what happened to the file is still that the source dropped it, and
        // that observation stays true whatever we decide to do about it. Only the action moves.
        at->Action = ContentAction::None;
        return true;
    }

    ContentUpdatePlan PlanContentUpdate( const ContentManifest& recorded, const ContentManifest& onDisk,
                                         const ContentManifest& incoming, ContentAuthorship authorship )
    {
        // The union of all three key sets, sorted and deduplicated: a state that only exists because a
        // key is ABSENT from one of the points cannot be found by walking any single one of them.
        std::vector<std::string> keys;
        keys.reserve( recorded.Count() + onDisk.Count() + incoming.Count() );
        for ( const auto* manifest : { &recorded, &onDisk, &incoming } )
            for ( const auto& entry : manifest->Entries() )
                keys.push_back( entry.Key );
        std::sort( keys.begin(), keys.end() );
        keys.erase( std::unique( keys.begin(), keys.end() ), keys.end() );

        ContentUpdatePlan plan;
        plan.Steps.reserve( keys.size() );
        for ( const auto& key : keys )
        {
            const ContentManifestEntry* r = recorded.Find( key );
            const ContentManifestEntry* d = onDisk.Find( key );
            const ContentManifestEntry* i = incoming.Find( key );

            ContentUpdateStep step;
            step.Key    = key;
            step.State  = Observe( r, d, i );
            step.Action = ActionFor( step.State, authorship, d != nullptr, i != nullptr );
            if ( step.State == ContentFileState::Conflict && authorship == ContentAuthorship::LocallyAuthored )
                plan.Conflicts.push_back( key );
            plan.Steps.push_back( std::move( step ) );
        }
        return plan;
    }

    std::vector<std::string> KeysToCensus( const std::vector<std::string>& offered,
                                           const ContentManifest&          recorded )
    {
        std::vector<std::string> keys = offered;
        keys.reserve( offered.size() + recorded.Count() );
        for ( const auto& entry : recorded.Entries() )
            keys.push_back( entry.Key );
        std::sort( keys.begin(), keys.end() );
        keys.erase( std::unique( keys.begin(), keys.end() ), keys.end() );
        return keys;
    }

    ContentManifest AdoptUnrecordedInstall( const ContentManifest& onDisk, const ContentManifest& incoming )
    {
        ContentManifest adopted;
        for ( const auto& entry : incoming.Entries() )
            if ( const auto* disk = onDisk.Find( entry.Key ); SameBytes( disk, &entry ) )
                adopted.Insert( *disk );
        return adopted;
    }

    Common::ResultStr<ContentUpdateReport>
    ApplyContentUpdate( const ContentUpdatePlan& plan, const ContentManifest& recorded,
                        const ContentManifest& incoming, const std::filesystem::path& installRoot,
                        const std::function<std::optional<std::string>( const std::string& )>& fetch )
    {
        if ( !plan.CanApply() )
            return Common::MakeFormattedError<ContentUpdateReport>(
                 "{} file(s) were changed both here and by the source (first: '{}'); nothing was applied",
                 plan.Conflicts.size(), plan.Conflicts.front() );

        // EVERY KEY IS CHECKED BEFORE THE FIRST BYTE MOVES. An update that stops halfway leaves an
        // install matching no released version, so the whole plan is validated first and only then
        // executed.
        for ( const auto& step : plan.Steps )
            if ( step.Action != ContentAction::None && !KeyStaysInside( step.Key ) )
                return Common::MakeFormattedError<ContentUpdateReport>(
                     "'{}' does not name a file inside the install; nothing was applied", step.Key );

        ContentUpdateReport report;
        report.Recorded = recorded;

        for ( const auto& step : plan.Steps )
        {
            const std::filesystem::path target = installRoot / std::filesystem::path( step.Key );
            const ContentManifestEntry* wanted = incoming.Find( step.Key );

            switch ( step.Action )
            {
                case ContentAction::Write:
                {
                    const auto bytes = fetch( step.Key );
                    if ( !bytes )
                        return Common::MakeFormattedError<ContentUpdateReport>(
                             "the source could not produce '{}'", step.Key );

                    std::error_code ec;
                    std::filesystem::create_directories( target.parent_path(), ec );
                    if ( const auto written = FileSystem::WriteContentToFileAtomic( target, *bytes ); !written )
                        return Common::MakeFormattedError<ContentUpdateReport>( "'{}': {}", step.Key,
                                                                                written.GetError() );
                    if ( wanted )
                        report.Recorded.Insert( *wanted );
                    ++report.Written;
                    break;
                }
                case ContentAction::Remove:
                {
                    std::error_code ec;
                    std::filesystem::remove( target, ec );
                    if ( ec )
                        return Common::MakeFormattedError<ContentUpdateReport>( "'{}' could not be removed: {}",
                                                                                step.Key, ec.message() );
                    report.Recorded.Remove( step.Key );
                    ++report.Removed;
                    break;
                }
                case ContentAction::None:
                    // THE RECORD FOLLOWS THE BYTES ON DISK, NOT THE VERSION WE DECLINED TO INSTALL.
                    // Advancing it on a skipped write would make the person's file compare equal to the
                    // record next time and be silently overwritten by the update after this one.
                    if ( step.State == ContentFileState::Unchanged && wanted )
                        report.Recorded.Insert( *wanted );
                    else if ( step.State == ContentFileState::SourceDeleted )
                    {
                        // Taken literally, and that literalness is what makes a WITHHELD removal work.
                        // Two different situations reach here as "the source dropped it, do nothing":
                        // the file was already gone (forget it), and a removal the caller withheld
                        // because something still points at the file (keep it, or the next update would
                        // see a file it has no record of, call it locally added, and never offer to
                        // remove it again — the reference could be gone by then and we would never know).
                        // The disk tells the two apart; the state cannot, and should not have to.
                        std::error_code existsEc;
                        if ( !std::filesystem::exists( target, existsEc ) )
                            report.Recorded.Remove( step.Key );
                    }
                    break;
            }

            switch ( step.State )
            {
                case ContentFileState::Unchanged:
                    ++report.Unchanged;
                    break;
                case ContentFileState::LocallyEdited:
                    if ( step.Action == ContentAction::None )
                        ++report.KeptLocalEdit;
                    break;
                case ContentFileState::LocallyAdded:
                    ++report.KeptLocalAdd;
                    break;
                case ContentFileState::LocallyDeleted:
                    if ( step.Action == ContentAction::None )
                        ++report.NotRestored;
                    break;
                default:
                    break;
            }
        }
        return Common::MakeSuccess( std::move( report ) );
    }
} // namespace Common::Utils
