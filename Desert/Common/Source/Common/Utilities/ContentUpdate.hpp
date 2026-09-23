#pragma once

#include "ContentManifest.hpp"

#include <Common/Core/ResultStr.hpp>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Common::Utils
{
    // UPDATING INSTALLED CONTENT WHEN THE CONSUMER MAY HAVE AUTHORED SOME OF IT.
    //
    // The mechanism is the two-point comparison in ContentManifest.hpp, applied TWICE against one
    // reference point:
    //
    //     recorded -> on disk    what the PERSON did since the install
    //     recorded -> incoming   what the SOURCE did since the install
    //
    // Neither comparison knows who its caller is; the matrix below is the only thing that reads both
    // answers at once, and ContentAuthorship is the only place the two consumers differ. That is the
    // seam: if CompareManifests ever has to ask whether it is serving a game patch or a collection,
    // the seam has been drawn in the wrong place.
    //
    // WITHOUT THE RECORD THERE ARE ONLY TWO STATES AND ONE OF THEM IS WRONG. The engine's collection
    // installer compared the EXISTENCE of a path (CollectionsPanel, "keep the existing, possibly
    // user-edited .demat"), which collapses "the person edited this" and "the source published a new
    // version" into one cell — and whichever arrived second lost. Concretely: a corrected material in
    // an updated collection was ignored, silently and for ever, for anyone who had ever opened that
    // collection before. The record is what makes the promise in that comment true rather than
    // aspirational.

    // WHO OWNS THE FILES ON THE CONSUMER'S SIDE. The two consumers of this mechanism differ in exactly
    // this one value, and the difference is not a preference — it is a fact about the situation.
    enum class ContentAuthorship
    {
        // A shipped game patch, a project template: the person on the receiving end authored nothing
        // here, so there is no edit to protect and the source is simply right. Local differences are
        // damage or debugging leftovers, and are overwritten.
        SourceOwned,

        // A collection installed into a project: the files are opened in the editor and edited. An edit
        // outranks an update, a deletion is an edit, and a file the person added is not the source's to
        // remove.
        LocallyAuthored,
    };

    // The six cells. The obvious four are the product of two independent predicates ("the person
    // changed it" x "the source changed it"); the last two are the ones a three-state model forgets,
    // and they are the harmful ones — a restored file the person deliberately deleted, and a file of
    // theirs removed because the source never knew about it.
    enum class ContentFileState
    {
        Unchanged,      // disk == recorded, incoming == recorded
        SourceAdded,    // absent from the record and from disk, present in incoming
        SourceUpdated,  // disk == recorded, incoming differs        -> the update everyone expects
        SourceDeleted,  // disk == recorded, absent from incoming    -> the case an overlay cannot express
        LocallyEdited,  // disk != recorded, incoming == recorded    -> the promise being kept
        LocallyAdded,   // on disk, in neither the record nor incoming
        LocallyDeleted, // in the record, gone from disk             -> deleting IS an edit; do not restore
        Conflict,       // disk != recorded AND incoming != recorded (or incoming dropped an edited file)
    };

    enum class ContentAction
    {
        None,   // leave the file exactly as it is
        Write,  // take the incoming bytes
        Remove, // delete it from the install
    };

    struct ContentUpdateStep
    {
        std::string      Key;
        ContentFileState State  = ContentFileState::Unchanged;
        ContentAction    Action = ContentAction::None;
    };

    struct ContentUpdatePlan
    {
        std::vector<ContentUpdateStep> Steps;     // sorted by key; every key of all three points appears
        std::vector<std::string>       Conflicts; // always empty under SourceOwned

        // ALL OR NOTHING. A half-applied update leaves an install that matches no released version and
        // no record of how it got there, so a plan with conflicts is not applied at all — the caller
        // shows the list and lets a person decide. (dpkg spends two force flags on exactly this
        // question, and names the cost of each; we make the refusal the default and name the files.)
        bool                     CanApply() const;
        size_t                   CountOf( ContentFileState state ) const;
        const ContentUpdateStep* Find( const std::string& key ) const;

        // TURNS ONE PLANNED REMOVAL INTO "LEAVE IT ALONE", for a caller that knows something this
        // mechanism cannot. A manifest says what the source stopped shipping; it says nothing about who
        // is still POINTING at that file, and deleting an asset a scene references breaks the scene
        // with no warning and no undo.
        //
        // The knowledge stays outside on purpose: what counts as a reference is a question about asset
        // FORMATS, and teaching this file about scenes would give the shared mechanism a consumer. So
        // the mechanism owns the CAPABILITY and the caller owns the REASON — see the editor's
        // WithholdReferencedRemovals, which supplies that reason from the asset reference index.
        //
        // Returns false when @p key is not a planned removal, so a caller cannot believe it withheld
        // something it did not. The RECORD IS KEPT for a withheld key: the file is still on disk and it
        // is still the source's, so the next update must see the same removal and withhold it again
        // until the last reference is gone. ApplyContentUpdate enforces that by looking at the disk
        // rather than at the state — "the record follows the bytes on disk", taken literally.
        bool WithholdRemoval( const std::string& key );
    };

    ContentUpdatePlan PlanContentUpdate( const ContentManifest& recorded, const ContentManifest& onDisk,
                                         const ContentManifest& incoming, ContentAuthorship authorship );

    // THE KEYS AN UPDATE HAS TO LOOK FOR ON DISK: everything the source is offering, PLUS everything
    // the record says was handed over. Sorted and deduplicated.
    //
    // This is four lines and it exists as a named function because getting it wrong is invisible. A
    // caller that censuses only what the DELIVERY contains cannot see the one file a removal is about
    // — the source has stopped mentioning it, by definition — so onDisk comes back without it,
    // PlanContentUpdate reads "the person already deleted it", the removal becomes ContentAction::None,
    // and the update reports success having done nothing. That shipped here, in the collection
    // installer, and no unit test could reach it because the census was built inline in a panel.
    //
    // So the rule is out where a test can hold it: an inventory must be built from the RECORD of what
    // was given, never from the delivery that no longer mentions it.
    std::vector<std::string> KeysToCensus( const std::vector<std::string>& offered,
                                           const ContentManifest&          recorded );

    // A STARTING RECORD FOR AN INSTALL THAT PREDATES RECORDS — the migration, and without it this whole
    // mechanism would help nobody who already has content installed.
    //
    // With no record, every file already on disk is "the person added it" and is therefore never touched
    // again: the freeze the record was introduced to end, preserved for exactly the installs that have
    // it. Adoption ends that, and it adopts ONLY the files whose bytes already equal what the source is
    // offering right now. That is the one case where the absent record can be reconstructed without
    // guessing: identical bytes cannot be somebody's edit.
    //
    // A file that differs is deliberately NOT adopted. It is either an edit or an older release and
    // nothing on disk can say which, so it stays untouched — the conservative half of the old
    // behaviour, kept on purpose rather than by omission, and undone by deleting the file.
    ContentManifest AdoptUnrecordedInstall( const ContentManifest& onDisk, const ContentManifest& incoming );

    // WHAT HAPPENED, POIMENNO — a result, not a log line. Nothing downstream can act on "the update
    // finished"; a person deciding whether their edits survived needs the counts and the names.
    struct ContentUpdateReport
    {
        size_t Written       = 0;
        size_t Removed       = 0;
        size_t Unchanged     = 0;
        size_t KeptLocalEdit = 0; // the person's version stood
        size_t KeptLocalAdd  = 0; // their own file, untouched
        size_t NotRestored   = 0; // they deleted it and it stayed deleted

        // There is no conflict list HERE on purpose. A plan with conflicts is never applied, so a report
        // could only ever carry an empty one — a field that looks like an answer and is always the same
        // answer. The list lives on ContentUpdatePlan, which is what a caller has in hand at the moment
        // it has to decide.

        // The record to persist afterwards. It tracks the source version that is ACTUALLY ON DISK: a
        // key whose write was skipped keeps its old entry, because that — and not the version we
        // declined to install — is what "unmodified" has to be measured against next time.
        ContentManifest Recorded;
    };

    // Executes a plan against `installRoot`. `fetch(key)` yields the incoming bytes for one key; it is a
    // callback rather than a directory or a pak so that the same applier serves a source that is a
    // directory, an archive, or (for collections) bytes generated in memory that never existed as a
    // file. Returns an error, having changed nothing, when the plan cannot be applied.
    Common::ResultStr<ContentUpdateReport>
    ApplyContentUpdate( const ContentUpdatePlan& plan, const ContentManifest& recorded,
                        const ContentManifest& incoming, const std::filesystem::path& installRoot,
                        const std::function<std::optional<std::string>( const std::string& )>& fetch );

    // The install record's file name, kept INSIDE the installed unit.
    //
    // Not a central registry: a pack is uninstalled by deleting its folder, and a registry outside it
    // would be orphaned at that moment and hand the next install someone else's entries. Not a sidecar
    // per file either — that is Unity's `.meta` convention, which this engine does not have and should
    // not acquire for one feature.
    //
    // Leading dot and NO EXTENSION on purpose. The dot keeps content walkers off it; the missing
    // extension keeps it away from the scanners that dispatch on one — AssetReferencesScan treats
    // `.json` as text and would go looking for asset references inside the record.
    //
    // That second half is load-bearing rather than tidy, and it took the removal guard to show why. The
    // record names every key the source handed over. If it were scanned as text it would be counted as
    // a REFERENCER of each of them, and a removal the guard is meant to allow would be withheld by the
    // very file that recorded it — for ever, and for a reason nobody would find. It is inert only
    // because its name has no extension the scanner recognises.
    inline constexpr std::string_view kInstallRecordFileName = ".desert-install";
} // namespace Common::Utils
