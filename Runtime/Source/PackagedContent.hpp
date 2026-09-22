#pragma once

// The packaged game's CONTENT MOUNT: which archives sit next to the executable, in what order they
// go onto the VFS stack, and what happens when one of them will not open.
//
// WHY THIS IS ITS OWN TRANSLATION UNIT rather than twenty lines inside CreateApplication. The
// decision it makes — start, or refuse and say what the player should do — is the whole subject of
// this file, and inside Main.cpp it could only ever be exercised by launching the game with a
// damaged archive and looking. Here a test builds a real .dpak, breaks a byte in it and asserts the
// refusal, with nothing linked but Common. The Runtime's Main.cpp is left holding policy only:
// print the message, exit with the code.

#include <filesystem>
#include <string>
#include <vector>

namespace Desert::Player
{
    // Process exit codes for the startup content path. DISTINCT ON PURPOSE: a launcher, a crash
    // reporter or a support script has to be able to tell a broken INSTALL from a broken UPDATE,
    // because the remedies are opposite — reinstall the game, versus re-download (or delete) one
    // patch file. Collapsing both into 1 would have thrown that away at the only place it is free.
    enum ContentExitCode : int
    {
        kContentOk                 = 0,
        kContentBaseArchiveFailed  = 2,
        kContentPatchArchiveFailed = 3,
        kContentArchivesAmbiguous  = 4,
        kContentChunkArchiveFailed = 5,
    };

    struct ContentMountResult
    {
        // 0 = the content set is mounted and the game may start. Non-zero = startup must stop, and
        // Message is the text to put in front of the player.
        int ExitCode = kContentOk;

        // Multi-line, and written for someone who has a game and no sources: which archive, which
        // step of opening it failed with the actual numbers, and what to do about it. Empty exactly
        // when ExitCode is kContentOk.
        std::string Message;

        // What actually went onto the stack, in mount order. BasePak is empty in a dev tree, where
        // there is no archive at all and every read is a plain disk read — that is a success, not a
        // failure.
        std::filesystem::path              BasePak;
        // The chunk archives the base's own list named, in mount order — between the base and the
        // patches. Empty for a game that was never divided, which is every game built before chunks.
        std::vector<std::filesystem::path> Chunks;
        std::vector<std::filesystem::path> Patches;
    };

    // Picks the base archive next to the executable. Preference order (deterministic):
    //   1. <exeStem>.dpak    (UE-style: rename exe + pak together = a different game)
    //   2. Content.dpak      (the default packaging name)
    //   3. the ONLY *.dpak in the folder, if exactly one exists (survives a rename of the pak alone)
    // Patch*.dpak and Chunk_*.dpak are excluded (both mount on top of the base afterwards, and
    // neither is a game on its own — a chunk in particular carries no descriptor at all). Returns empty when there is none, and
    // ALSO when several are candidates — in that case `ambiguous` receives their names, because "no
    // archive here" and "I cannot tell which of these three is the game" need different answers.
    std::filesystem::path FindBasePak( const std::filesystem::path& dir, const std::string& exeStem,
                                       std::vector<std::filesystem::path>* ambiguous = nullptr );

    // Mounts the base archive, then the CHUNK archives the base itself names, then every Patch*.dpak
    // ON TOP in name order (later overrides earlier), which is what makes shipping a fix a matter of
    // dropping one file into the folder.
    //
    // THE CHUNKS COME FROM THE BASE'S OWN LIST, NOT FROM A FILENAME SCAN, and that is the difference
    // between a division that can fail loudly and one that cannot fail at all. A scan would mount
    // whatever happened to be in the folder, so an update that failed to deliver one chunk would
    // produce a game that starts, looks healthy, and is missing a region — the exact shape this file
    // exists to refuse. A named list means a missing chunk is a refusal that can say WHICH.
    //
    // REFUSES ON THE FIRST FAILURE, and leaves NOTHING mounted when it refuses. Half a content set
    // is the state this whole file exists to prevent: the game would come up, look healthy, and be
    // running a mixture nobody ever built or tested.
    ContentMountResult MountPackagedContent( const std::filesystem::path& baseDir, const std::string& exeStem );
} // namespace Desert::Player
