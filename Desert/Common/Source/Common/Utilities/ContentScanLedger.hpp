#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// HOW MUCH DIRECTORY WALKING THIS PROCESS HAS DONE — the detector for a claim nothing else in the
// repository can observe.
//
// WHY IT EXISTS. `AssetPathIndex` made the number of resolvable handles observable
// (`[AssetPathIndex] boot finished — N handle(s) can name their own path`), and the cooked registry
// (GAP_ANALYSIS T2.4) has to reproduce that N *without the directory walk that used to mint them*.
// The second half of that sentence had no instrument: a boot stage's wall time mixes the walk with
// the parses and the GPU work it precedes, so "the walk is gone" could only ever have been argued
// from the source. A property that is not observable in the chosen tool cannot be evidence in
// either direction, so here is the tool.
//
// WHAT COUNTS AS A WALK: one call to `FileSystem::ListFilesRecursive`, i.e. one
// `recursive_directory_iterator` over a content root plus the VFS half of the same root. That is
// the ONLY enumeration primitive in the engine — the font and icon services once hand-rolled the
// disk half and a packaged game scanned nothing, which is why there is exactly one — so counting it
// counts every content scan there is.
//
// IT IS A COUNTER AND NOTHING BRANCHES ON IT. Nothing reads these numbers to decide anything; they
// are printed beside the other two boot lines and asserted by `Desert/Tests/Common/ContentScanners`
// and by the cooked-registry suites. A detector that changes behaviour would be a second mechanism
// to keep in step with the first.
namespace Common::Utils::ContentScanLedger
{
    struct Readout
    {
        // Calls to ListFilesRecursive. THE number the registry work is judged by: the demand-driven
        // boot must reach the same handle count with this at zero.
        uint64_t Walks = 0;
        // Regular files the walks looked at, loose + packed, before extension filtering. Says how
        // much the walks actually cost rather than how many there were — eight walks over eight
        // empty roots and eight over a thousand files each are the same `Walks`.
        uint64_t Entries = 0;
        // Wall time inside those calls. Not a budget: it is here so a report can put a millisecond
        // beside the count instead of asserting one from the shape of the code.
        double Milliseconds = 0.0;
    };

    // Adds one walk. Called by `FileSystem::ListFilesRecursive` and by nothing else.
    void NoteWalk( std::size_t entries, double milliseconds );

    Readout Take();

    // One line, in the shape of `SyncLoadLedger::Report()` so the three boot lines read alike.
    std::string Report();

    // EXISTS FOR TESTS ONLY, and for the same reason `AssetPathIndex::Clear` does: a suite that
    // measures a scan has to start from a known zero, and the process-wide counter is shared with
    // whatever ran before it in the same binary. Production has no caller — a boot that reset its
    // own detector would be a boot that cannot be asked what it did.
    void Reset();
} // namespace Common::Utils::ContentScanLedger
