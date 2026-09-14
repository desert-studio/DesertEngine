#pragma once

#include <Common/Utilities/ContentUpdate.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace Desert::Editor
{
    // A format-agnostic asset USAGE index. Assets in Desert point at each other by 64-bit handle
    // (materials embed texture handles, scenes/prefabs embed mesh/material handles, ...) and, in a few
    // formats, by path. Rather than teach this index every on-disk schema, it works by TOKENS: each
    // asset contributes the tokens a referencer would embed to point at it (its stable handle as
    // decimal text, its self-declared id, its project-relative path/name), and asset A references
    // asset B when A's raw file text contains any of B's tokens.
    //
    // This is a search aid, not a proof: numeric handle collisions are astronomically unlikely and
    // path strings are exact, but a bespoke binary format could hide a reference from a text scan. The
    // index therefore drives "find references" and "unused asset" discovery and a delete WARNING — it
    // never deletes on its own.
    //
    // The index itself needs nothing from the engine (std plus Common's content-update types) so it is
    // unit-tested directly; BuildProjectAssetReferenceIndex (see AssetReferencesScan.cpp) is the
    // engine-side adapter that fills it from the project on disk.
    class AssetReferenceIndex
    {
    public:
        struct Entry
        {
            std::string              Path;   // project-relative, '/'-separated identity of the asset
            std::string              Ext;    // lowercased extension incl. dot (".demat"); "" if none
            std::vector<std::string> Tokens; // strings a referencer embeds to point at THIS asset
            std::string              Text;   // raw contents, searched for OTHER entries' tokens ("" = binary)
        };

        void                      Clear();
        void                      Add( Entry entry );
        const std::vector<Entry>& Entries() const;

        // Paths of entries whose Text contains any Token of the entry at @p path (self excluded).
        std::vector<std::string> ReferencersOf( const std::string& path ) const;

        bool IsReferenced( const std::string& path ) const;

        // The OTHER direction, and the one a packager needs: paths of entries whose Tokens appear in
        // the text of the entry at @p path (self excluded). ReferencersOf answers "who breaks if I
        // delete this"; this answers "what must travel with this", and the transitive closure of it
        // from a scene is exactly the set of files a build has to ship for that scene to open.
        //
        // Same caveat as the class: a token match is a search aid, not a proof. It over-approximates
        // rather than under-approximates — a spurious match ships one file too many, which is the
        // harmless direction. The harmful direction is a reference no text scan can see, and that is
        // what Desert/Tests/Tools/AssetClosure pins for the trees this repository actually ships.
        std::vector<std::string> ReferencedBy( const std::string& path ) const;

        // Every entry reachable from @p path through ReferencedBy, @p path included. Empty when
        // @p path is not in the index — an unknown root is a caller error, not an empty world.
        std::vector<std::string> ClosureFrom( const std::string& path ) const;

        // Entries with one of @p leafExts that nothing references — cleanup candidates. Roots (scenes,
        // prefabs) are naturally unreferenced, so callers pass only leaf extensions (textures, materials).
        std::vector<std::string> Orphans( const std::vector<std::string>& leafExts ) const;

    private:
        const Entry* Find( const std::string& path ) const;

        std::vector<Entry> m_Entries;
    };

    // Scans @p assetsRoot and fills @p index (clears it first). Text formats are read for scanning;
    // binaries contribute tokens only. @p projectDir is what an asset's project-relative token is
    // measured against — it is a separate argument rather than assetsRoot's parent because a project
    // may declare any AssetsRoot it likes.
    //
    // KNOWS NOTHING OF ProjectContext, and the separation is load-bearing rather than tidy: the token
    // rule (one handle, the project-relative spelling, the file name, the self-ids) is what makes two
    // assets agree that they refer to each other, and a SECOND implementation of that rule in a
    // packaging tool would be free to drift from this one — the packager would then ship a set that
    // the editor's own "find references" disagrees with, and the disagreement would show up as a file
    // missing from a shipped build. One rule, two callers.
    void BuildAssetReferenceIndex( AssetReferenceIndex& index, const std::filesystem::path& assetsRoot,
                                   const std::filesystem::path& projectDir );

    // The same scan against the currently-open project. No-op without an open project.
    void BuildProjectAssetReferenceIndex( AssetReferenceIndex& index );

    // One withheld removal, with somebody to name. The count is carried alongside the first referencer
    // because a message that says "still used by Main.desce" when four scenes use it is a message that
    // will be acted on once and then be wrong three more times.
    struct WithheldRemoval
    {
        std::string Key;             // the update plan's key
        std::string FirstReferencer; // assets-relative path of one asset that still points at it
        size_t      ReferencerCount = 0;
    };

    // THE GUARD THAT HAS TO ARRIVE WITH DELETIONS, NOT AFTER THEM.
    //
    // A content update can now remove files, and for a collection installed into a project that means
    // deleting an asset a scene may be pointing at — with no warning and no undo. The owner's answer to
    // that is the reference rewrite, which will list the affected scenes and be able to refuse; it has
    // not moved into the shared submodule yet, and until it does the deletion half would be shipping
    // ahead of the thing that makes it safe. So the removal asks the index HERE.
    //
    // This is not a stopgap. Asking what still points at a file is the right answer after the move too;
    // only the index's ADDRESS changes, when the launcher needs the same answer and the rewrite becomes
    // shared. What is temporary is the direction of the call, not the question.
    //
    // Every planned removal whose key (prefixed by @p indexPathPrefix, the plan root's own path
    // relative to the assets root) is still referenced is turned into "leave it alone", and returned so
    // the caller can say which scene stopped it. The index is a text scan and therefore a search aid,
    // not a proof — which is the right way round here: a false positive keeps a file, a false negative
    // is the deletion we were going to make anyway.
    std::vector<WithheldRemoval> WithholdReferencedRemovals( Common::Utils::ContentUpdatePlan& plan,
                                                             const AssetReferenceIndex&        index,
                                                             const std::string&                indexPathPrefix );
} // namespace Desert::Editor
