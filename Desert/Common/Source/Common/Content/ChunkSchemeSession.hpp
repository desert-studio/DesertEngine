#pragma once

#include <Common/Content/ContentChunks.hpp>
#include <Common/Core/ResultStr.hpp>

#include <cstddef>
#include <filesystem>
#include <string>

namespace Common::Content
{
    // THE EDITOR'S ONE STATE FOR A PROJECT'S CHUNK SCHEME.
    //
    // Before this object the scheme had as many states as it had callers: the Build Settings button
    // kept its own "what did the last click do" string, the palette command kept none, and a scheme
    // created from the palette left the panel still showing the text of the click before. Every
    // action — create the default, reload, edit, save, revert — now goes through here, so whoever
    // draws the scheme draws the same path, the same load result and the same last action.
    //
    // UE's pattern is Project Settings -> Packaging (chunk assignment by rules); the letter we do not
    // copy is a settings object that silently fills in defaults — a missing file stays Missing until
    // somebody asks for the default by name (owner, 2026-09-25).
    //
    // THE SAVED SCHEME AND THE DRAFT ARE TWO VALUES ON PURPOSE: the packager reads the file, so the
    // panel has to be able to say "what you see is not what Build will use" (Dirty) instead of
    // pretending an unsaved edit already applies.
    class ChunkSchemeSession
    {
    public:
        enum class Status
        {
            Missing,    // no file at Path(); LoadMessage() is LoadChunkScheme's refusal naming it
            Unreadable, // a file exists but was refused (empty, malformed); LoadMessage() says why
            Loaded,
        };

        // Reads @p path immediately: a session never exists in a state nobody has looked at.
        explicit ChunkSchemeSession( std::filesystem::path path );

        [[nodiscard]] const std::filesystem::path& Path() const;
        [[nodiscard]] Status                       GetStatus() const;
        // The refusal text (with the path) when not Loaded, a one-line summary when Loaded.
        [[nodiscard]] const std::string& LoadMessage() const;
        // What the last action did, or why it was refused; empty until one ran.
        [[nodiscard]] const std::string& LastAction() const;

        // The scheme as the file holds it, and the edited copy. Both empty unless Loaded.
        [[nodiscard]] const ChunkScheme& Saved() const;
        [[nodiscard]] const ChunkScheme& Draft() const;
        [[nodiscard]] bool               Dirty() const;
        // Bumped whenever Saved() or Draft() may have changed, so a view that derives something costly
        // from them (the panel's plan preview) re-derives exactly then and not every frame.
        [[nodiscard]] std::size_t Revision() const;

        // Re-reads the file; a draft is discarded (the file is the truth the packager reads).
        void Reload();

        // WriteDefaultChunkScheme, then Reload. Refused (and nothing changes) if the file exists.
        BoolResultStr CreateDefault();

        // DRAFT EDITS. Each refuses by name and leaves the draft as it was; none corrects the input.
        // A new chunk has no roots yet, which is a legal draft and an illegal save: the save refuses
        // it with ValidateChunkScheme's text rather than this call guessing a root.
        BoolResultStr AddChunk( const std::string& name );
        BoolResultStr RemoveChunk( std::size_t chunk );
        BoolResultStr RenameChunk( std::size_t chunk, const std::string& name );
        BoolResultStr AddRoot( std::size_t chunk, const std::string& key );
        BoolResultStr RemoveRoot( std::size_t chunk, std::size_t root );
        BoolResultStr AddPin( const std::string& key );
        BoolResultStr RemovePin( std::size_t pin );

        // SaveChunkScheme( Path(), Draft(), registry ) and, on success, Reload. On refusal the draft
        // stays for the author to fix and the file is untouched.
        BoolResultStr Save( const Utils::AssetRegistry& registry );
        void          Revert();

    private:
        BoolResultStr Record( BoolResultStr result, const std::string& done );
        BoolResultStr RequireLoaded( const char* action );

        std::filesystem::path m_Path;
        Status                m_Status = Status::Missing;
        std::string           m_LoadMessage;
        std::string           m_LastAction;
        ChunkScheme           m_Saved;
        ChunkScheme           m_Draft;
        std::size_t           m_Revision = 0;
    };
} // namespace Common::Content
