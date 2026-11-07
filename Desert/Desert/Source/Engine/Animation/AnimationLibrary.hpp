#pragma once

#include <Engine/Animation/ClipSkeletonMatch.hpp>
#include <Engine/Assets/Mesh/AnimationAsset.hpp>
#include <Engine/Assets/Mesh/SkinnedMeshAsset.hpp>
#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/AsyncAssetLoader.hpp>

#include <unordered_map>

#include <string>
#include <vector>

namespace Desert::Animation
{
    /**
     * @brief WHICH CLIPS FIT WHICH RIG — an index of animation assets by the skeleton they animate.
     *
     * IT HANDS OUT ASSETS WITHOUT LOADING THEM, and that is why eviction had to be taught about it. The
     * index is built at Register time from properties READ OUT OF THE CLIP (its skeleton signature, its
     * animated bone names), and the lookup then resolves a handle straight through the AssetManager. An
     * evicted clip would come back from here as a perfectly valid asset holding an empty track list, and
     * the character would silently T-pose — §1.4's empty successful answer, at the seam where it is
     * hardest to see. The lookup therefore RELOADS before returning; see Resolve.
     */
    class AnimationLibrary
    {
    public:
        // NON-CONST because the lookups reload an evicted clip before handing it out, and
        // AssetBase::EnsureLoaded needs a manager it can resolve dependencies against. It was const while
        // nothing ever released an asset.
        explicit AnimationLibrary( Assets::AssetManager* assetManager );
        void Register( const Assets::Asset<Assets::AnimationAsset>& animation );
        void Unregister( const Assets::AssetHandle& handle );

        /// The mesh side of ClipPlaysOnMesh, from the mesh's skeleton reference (SkinnedMeshAsset::GetSkeleton)
        /// and that skeleton's CompatibleSkeletons (its bound SkeletonAsset). The one place callers get it.
        [[nodiscard]] static MeshSkeletonIdentity IdentifyMesh( const Assets::SkinnedMeshAsset& mesh );

        /// IdentifyMesh of the SkinnedMeshAsset @p mesh names, if it is resident; else none. Not (not resident, or
        /// an editor-built runtime rig, which has no asset) = an identity that references no skeleton: every clip
        /// is refused.
        [[nodiscard]] MeshSkeletonIdentity IdentifyMeshHandle( const Assets::AssetHandle& mesh ) const;

        /// A skeleton reference with its name for refusals (the registry key of its row, or its GUID text).
        [[nodiscard]] static SkeletonAssetRef SkeletonRefOf( const Common::Content::AssetGuid& skeleton );

        // THE ONLY LOOKUP, over THE ONLY RULE (ClipPlaysOnMesh, SkeletonReference.hpp): the pickers and the
        // runtime both ask here, so what a picker offers is what a state can resolve.
        std::vector<Assets::Asset<Assets::AnimationAsset>> GetForMesh( const MeshSkeletonIdentity& mesh ) const;

        // The single-clip form the runtime needs, over the SAME rule and the same records. It reports WHY it
        // found nothing, naming the clip, the rig and what the rig does have — the caller cannot turn a
        // refusal into a state that quietly plays nothing without discarding a message first.
        [[nodiscard]] Common::ResultStr<Assets::Asset<Assets::AnimationAsset>>
        FindForMesh( const MeshSkeletonIdentity& mesh, const std::string& clipName ) const;

        void Clear();

        // Indexes every `.anim` row of the content registry by its stated clip name WITHOUT reading a
        // clip. A clip's rig identity lives in its body, so a row becomes matchable only once the loader
        // has read it; a lookup that names it starts that read. Returns how many rows were indexed.
        size_t IndexRegistryRows();

        // Whether a clip named @p clipName (any clip, when empty) is still being read or waits to be.
        // A lookup that failed while this is true is "not yet", not "never" — callers stay quiet.
        [[nodiscard]] bool HasPending( const std::string& clipName ) const;

        /// THE STAMP A CACHED ANSWER IS VALID FOR. Moves whenever any lookup above could answer differently:
        /// a clip registered, unregistered or re-read, a read finished or cancelled, a registry write caught
        /// up (asked here, so a `.anim` an import wrote moves it), the library cleared. Not moved by a
        /// read merely being STARTED — that changes no answer, the pending one stays pending. An eviction
        /// does not move it either: a cached asset is checked with IsReadyForUse by its holder
        /// (ClipBindingCache), because the library never hears of one.
        [[nodiscard]] uint64_t Revision() const;

    private:
        /// The handle -> asset step both lookups share, INCLUDING the reload. One place, so a third lookup
        /// cannot be written that forgets it.
        [[nodiscard]] Assets::Asset<Assets::AnimationAsset> Resolve( const Assets::AssetHandle& handle ) const;

        // Non-owning, and it outlives nothing: the library is destroyed with the layer that made it, and
        // the manager with the project. A project switch that replaced the manager without replacing this
        // would leave it dangling — nothing does that today, and nothing checks either.
        struct UnreadRow
        {
            Assets::AssetHandle Handle;
            std::string         ClipName; // the row's stated name; empty = unknown until read
            // The row's Rig tag: the skeleton the clip references, as the scan read it from the header. Lets a
            // miss name what the rig DOES have before any clip is read (null = the header states none).
            Common::Content::AssetGuid Skeleton;
        };

        // Every clip the library knows, read or not: the read records plus one identity per unread row (its
        // stated name and Rig tag). What a refusal counts and lists, so "0 clip(s) known" is never said of a
        // project whose clips are indexed and simply have other names.
        [[nodiscard]] std::vector<ClipRigIdentity> Indexed() const;

        void RequestUnread( const std::string& clipName ) const;
        // UE: the registry's OnAssetAdded/OnAssetUpdated reaching the index. A `.anim` an import (or any cook)
        // wrote after this library indexed the rows is indexed now, on the same terms as IndexRegistryRows:
        // a clip already read in place (the importer re-reads rewritten clips) goes through Register, any
        // other becomes an unread row. A rewritten clip loses its old record first, since its rig may differ.
        void CatchUpWrites() const;
        void RequestRead( const Assets::Asset<Assets::AnimationAsset>& asset ) const;
        /// m_ClipsByName rebuilt from m_Clips when a Register/Unregister/Clear marked it stale.
        void RebuildNameIndex() const;
        void Touch() const
        {
            ++m_Revision;
            m_NameIndexStale = true;
        }

        Assets::AssetManager* m_AssetManager;
        // Rows indexed but not yet read, and the reads in flight. Mutable: a const lookup is what asks.
        mutable std::vector<UnreadRow>                                       m_Unread;
        mutable std::unordered_map<Assets::AssetHandle, Assets::LoadRequest> m_Requests;
        // The registry write serial this library has indexed up to (ContentRegistry::WriteSerial).
        mutable uint64_t m_SeenWrites = 0;

        // ONE record per registered clip, holding everything the match rule is allowed to look at. There used
        // to be two indexes of the same clips — a signature map and this list — and Clear() emptied only one
        // of them, so the library went on offering the previous project's clips out of the half nobody
        // remembered. A single container cannot fall out of step with itself.
        std::vector<ClipRigIdentity> m_Clips;
        // DERIVED from m_Clips, never written on its own: clip name -> indices into m_Clips in ascending order
        // (so the first that plays is the one the linear rule picked). Rebuilt lazily after any edit of
        // m_Clips (Touch), which keeps the "one container cannot fall out of step" property above.
        mutable std::unordered_map<std::string, std::vector<uint32_t>> m_ClipsByName;
        mutable bool                                                   m_NameIndexStale = true;
        mutable uint64_t                                               m_Revision       = 1;
        // RequestUnread's handles to read, reused: a read that completes inside Request edits m_Unread, so
        // the rows are picked first and requested after, without copying the rows themselves.
        mutable std::vector<Assets::AssetHandle> m_RequestScratch;
    };

    /**
     * @brief ONE ENTITY'S CLIPS, RESOLVED ONCE — the AnimationComponent's half of the lookup above.
     *
     * UE: FAnimNode_SequencePlayer holds the UAnimSequence* resolved at load, and the tick never asks by
     * name. Here a graph names its clips and the library resolves a name against a rig; that answer is
     * kept per (name, rig, AnimationLibrary::Revision) and asked again only when the rig or the library's
     * revision moves — or when the asset it holds was evicted (IsReadyForUse), which the library cannot see.
     * A refusal is formatted once per revision, not once per tick. A pending clip stays pending until the
     * read it waits for finishes, which is exactly a revision move.
     */
    class ClipBindingCache
    {
    public:
        struct Binding
        {
            std::string                           Name;
            Assets::Asset<Assets::AnimationAsset> Asset;   ///< null = pending or refused
            std::string                           Error;   ///< the refusal, when not pending
            bool                                  Pending = false;
            uint64_t                              Epoch   = 0; ///< the cache epoch it was resolved at
            /// The owners (state / node names) the refusal was already reported for, this epoch.
            std::vector<std::string> ReportedOwners;
        };

        /// Moves the epoch when the library revision or the rig differs from the last call: every binding is
        /// then resolved again on its next ask. Returns whether it moved.
        bool Revalidate( uint64_t libraryRevision, const MeshSkeletonIdentity& rig );

        /// @p name resolved on the rig of the last Revalidate; resolved through @p library on its first ask
        /// this epoch, or again when the asset it held was evicted.
        [[nodiscard]] Binding& Resolve( const AnimationLibrary& library, const MeshSkeletonIdentity& rig,
                                        const std::string& name );

        /// Whether a graph's clips must be asked for again (UE: an AnimBP's hard references): the epoch
        /// moved, or another graph (its source handle) or graph revision is attached. Records the answer.
        [[nodiscard]] bool TakeGraphPrefetch( uint64_t graphSource, uint32_t graphRevision );

        /// Every held asset that was evicted is asked for again (the library starts its read), so a state
        /// entered later finds its clip resident, as the per-tick prefetch it replaces guaranteed.
        void RefreshEvicted( const AnimationLibrary& library, const MeshSkeletonIdentity& rig );

    private:
        void ResolveInto( Binding& binding, const AnimationLibrary& library, const MeshSkeletonIdentity& rig );

        std::vector<Binding>                    m_Bindings;
        uint64_t                                m_Epoch           = 0;
        uint64_t                                m_LibraryRevision = 0;
        Common::Content::AssetGuid              m_Skeleton;
        std::vector<Common::Content::AssetGuid> m_Compatible;
        bool                                    m_Keyed = false;

        uint64_t m_PrefetchEpoch    = 0;
        uint64_t m_PrefetchSource   = 0;
        uint32_t m_PrefetchRevision = 0;
        bool     m_Prefetched       = false;
    };

    /// What one population run put in the library, so a caller can say which half is empty.
    struct LibraryPopulation
    {
        size_t FromFiles = 0; ///< clips registered out of `.anim` assets the scan created
    };

    /**
     * @brief THE ONE PLACE AN ANIMATION LIBRARY IS FILLED — for the editor and for the packaged game alike.
     *
     * WHAT IT REPLACES, because the shape of the defect is the argument for the shape of the fix. The
     * editor used to walk the manager and call Register itself, in `OnAttach`; the runtime did NOTHING —
     * it built a library, handed it straight to `AnimationECSSystem` and never put a clip in it, so every
     * skinned character in a shipped build stood in its T-pose. Two hosts, one of them with a copy of the
     * loop and the other with none, is the "both ends correct, the link between them missing" shape this
     * project keeps paying for; the answer is not a second copy of the loop but a single point that
     * neither host has to remember, called from the asset scan itself.
     *
     * AND THE EDITOR'S COPY WAS ALSO IN THE WRONG ORDER, which is the half a shared function alone would
     * not fix. `OnAttach` ran BEFORE the staged startup pass that scans `.anim` off disk, so the library
     * was filled out of a manager that had not been shown the clips yet: with clips on disk the editor
     * still reported exactly the four procedural ones.
     *
     * @param clipFilesDiscovered how many `.anim` files the asset scan just found under the cooked root.
     *        IT IS A PARAMETER RATHER THAN SOMETHING THIS FUNCTION COUNTS FOR ITSELF, and that is the
     *        ordering rule made structural instead of textual: the number does not exist until the scan
     *        has run, so a caller that fills the library first has nothing to pass and does not compile.
     *        A later refactor is free to move these statements around; it cannot move them past each
     *        other. It is also the only way the refusal below can be stated — a library that comes out
     *        empty is a defect when there were files and the shipped state when there were none, and
     *        those two are indistinguishable from inside the library.
     *
     * @return a refusal when the scan found clip files and not one of them reached the library. Everything
     *         else is logged with its counts; the caller is expected to LOG_ERROR the refusal, as the
     *         preloader's other register loops do.
     */
    [[nodiscard]] Common::ResultStr<LibraryPopulation>
    PopulateLibrary( Assets::AssetManager& assets, AnimationLibrary& library, size_t clipFilesDiscovered );
} // namespace Desert::Animation