#pragma once

#include <Common/Core/ResultStr.hpp>

#include <filesystem>
#include <string>
#include <string_view>

namespace Desert::Editor::Core::Rules
{
    /**
     * @brief WHICH FILE a save of the open scene writes. Pure: three strings in, a path out.
     *
     * THE DEFECT THIS REPLACES. The destination used to be computed inside the engine, on every save,
     * from the scene's NAME: `Scene/` + the name with its spaces turned into underscores. So a scene
     * opened from `Scene/U52_LockProbe.desce` whose name is "U52 Lock Probe" saved into
     * `Scene/U52_Lock_Probe.desce` — a second file, beside the first, under a green "Saved" toast. The
     * user's next open of "the same" level showed the work missing, and the work itself was sitting one
     * filename away. A display name and a file identity are two different things and the engine cannot
     * tell them apart; only whoever opened the file can, so it says.
     *
     * @param openScenePath the file the scene was opened from or last written to. WINS whenever it is
     *        not empty, and that precedence IS the fix — the name may say anything.
     * @param sceneName     what the scene is called. Reached only when there is no file yet (File -> New
     *        Scene), where it is the only thing there is to name one after.
     * @param sceneDir      the project's scene directory, for that fallback.
     * @param extension     the scene file extension, including the dot.
     */
    [[nodiscard]] inline std::string SceneSaveDestination( std::string_view openScenePath,
                                                           std::string_view sceneName, std::string_view sceneDir,
                                                           std::string_view extension )
    {
        if ( !openScenePath.empty() )
            return std::string( openScenePath );

        std::string name( sceneName );
        for ( auto& ch : name )
            if ( ch == ' ' )
                ch = '_';

        std::string directory( sceneDir );
        if ( !directory.empty() && directory.back() != '/' )
            directory += '/';
        return directory + name + std::string( extension );
    }

    /**
     * @brief What the editor is allowed to do once a scene save has ANSWERED. Pure: a result and two
     *        names in, a verdict out — no ImGui, no disk, no globals.
     *
     * WHY THIS IS A FUNCTION AND NOT FOUR COPIES OF AN `if`. There are four places that save the open
     * scene — Ctrl+S, File -> Save, the command palette's "Save Scene", and the "Save and Open" button
     * of the unsaved-changes modal — and before this existed all four wrote the same three statements
     * in a row: save, clear the saved-revision mark, announce success. Unconditionally. The save chain
     * returned void, so "announce success" was the only thing any of them could do.
     *
     * The cost was not evenly spread. Ctrl+S on a read-only file or a full disk put the amber "unsaved
     * changes" star out and raised a green "Saved 'X'" toast over a scene that was still only in
     * memory. "Save and Open" did the same and then called LoadScene, which clears the command history
     * and the scene itself — so the user was asked "save before opening the other one?", said yes, and
     * the work they said yes FOR was destroyed in memory after failing to reach the disk. There was
     * nowhere left to recover it from.
     *
     * So the two permissions below are deliberately separate fields even though today they carry the
     * same value: they answer different questions ("is the file on disk current?" and "may I throw the
     * scene in memory away?"), and a future policy that clears the mark for, say, an autosave without
     * ever permitting a discard should have to change this function rather than one call site.
     */
    struct SaveVerdict
    {
        /// May the "unsaved changes" mark be cleared (s_SavedRevision advanced, the amber star put out)?
        /// FALSE on failure — the scene in memory is still the only current copy, and the star is the
        /// only thing telling the user so.
        bool MarkSceneSaved = false;

        /// May a step that DESTROYS the in-memory scene now run (LoadScene over it, New Scene, quit)?
        /// FALSE on failure, and this is the field the "Save and Open" button exists to read.
        bool MayDiscardScene = false;

        /// Is `Message` a failure? Drives the toast level and whether the log line is an error.
        bool IsError = false;

        /// What the user is shown. It names the scene, the FILE, and on failure the reason the save
        /// chain gave — a message that says only "save failed" sends the user to the log to find out
        /// which file, and the log is exactly what a user of a GUI editor does not have open.
        ///
        /// THE SUCCESS MESSAGE NAMES THE FILE FOR A REASON. It used to say only "Saved 'X'", where X is
        /// the scene's NAME, at a time when the destination was DERIVED from that name — so a scene
        /// opened from U52_LockProbe.desce and called "U52 Lock Probe" was written to a second file and
        /// the toast said the one thing that was true of both. The path is what the two differ in, so
        /// the path is what the message has to carry.
        std::string Message;
    };

    /**
     * @param save        what Scene::Serialize answered.
     * @param sceneName   the scene's name, for the message. Not used to decide anything.
     * @param destination the file that was written, for the message. Not used to decide anything.
     */
    [[nodiscard]] inline SaveVerdict DecideAfterSceneSave( const Common::BoolResultStr& save,
                                                           std::string_view             sceneName,
                                                           std::string_view             destination )
    {
        SaveVerdict verdict;
        if ( save.IsSuccess() )
        {
            verdict.MarkSceneSaved  = true;
            verdict.MayDiscardScene = true;
            verdict.IsError         = false;
            verdict.Message         = "Saved '" + std::string( sceneName ) + "' to " + std::string( destination );
            return verdict;
        }

        verdict.MarkSceneSaved  = false;
        verdict.MayDiscardScene = false;
        verdict.IsError         = true;
        verdict.Message         = "'" + std::string( sceneName ) + "' was NOT saved — " + save.GetError() +
                          ". The scene is still open and still unsaved.";
        return verdict;
    }

    /**
     * @brief Whether a save to `destination` writes the SAME asset the scene was opened as, so its GUID (the
     * text header's identity) is kept - or a NEW one, whose header is dropped before the write so a fresh
     * GUID is minted. Pure: two paths in, one bit out.
     *
     * A copy is a new asset, as in UE's Save As: two files stating one GUID would make every lookup by
     * identity pick one of them at random. So the identity is kept only when the bytes go back to the file
     * the scene came from; a scene with no file yet (File -> New Scene, a generated Starter or showcase) has
     * no identity to keep - whatever header it still carries belonged to the scene it replaced.
     *
     * @param openScenePath the file the scene was opened from or last written to; empty when none.
     * @param destination   the file this save writes.
     */
    [[nodiscard]] inline bool SaveKeepsAssetIdentity( std::string_view openScenePath, std::string_view destination )
    {
        if ( openScenePath.empty() )
            return false;
        return std::filesystem::path( openScenePath ).lexically_normal() ==
               std::filesystem::path( destination ).lexically_normal();
    }
} // namespace Desert::Editor::Core::Rules
