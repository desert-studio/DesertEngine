#pragma once

#include <Engine/Animation/ClipSkeletonMatch.hpp>
#include <Engine/Assets/Mesh/AnimationAsset.hpp>
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

        // THE ONLY LOOKUP. It replaced a pair — an exact-signature one used by the runtime and a tolerant
        // bone-name one used by the editor's pickers — whose disagreement is described in
        // ClipSkeletonMatch.hpp. Both of those spellings are gone: with two of them in the tree, a caller
        // chose a semantics by choosing a function name, and nothing made the choices agree.
        std::vector<Assets::Asset<Assets::AnimationAsset>> GetForSkeleton( const Skeleton& skeleton ) const;

        // The single-clip form the runtime needs, over the SAME rule and the same records. It reports WHY it
        // found nothing, naming the clip, the rig and what the rig does have — the caller cannot turn a
        // refusal into a state that quietly plays nothing without discarding a message first.
        [[nodiscard]] Common::ResultStr<Assets::Asset<Assets::AnimationAsset>>
        FindForSkeleton( const Skeleton& skeleton, const std::string& clipName ) const;

        void Clear();

        // Indexes every `.anim` row of the content registry by its stated clip name WITHOUT reading a
        // clip. A clip's rig identity lives in its body, so a row becomes matchable only once the loader
        // has read it; a lookup that names it starts that read. Returns how many rows were indexed.
        size_t IndexRegistryRows();

        // Whether a clip named @p clipName (any clip, when empty) is still being read or waits to be.
        // A lookup that failed while this is true is "not yet", not "never" — callers stay quiet.
        [[nodiscard]] bool HasPending( const std::string& clipName ) const;

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
        };

        void RequestUnread( const std::string& clipName ) const;
        // UE: the registry's OnAssetAdded/OnAssetUpdated reaching the index. A `.anim` an import (or any cook)
        // wrote after this library indexed the rows is indexed now, on the same terms as IndexRegistryRows:
        // a clip already read in place (the importer re-reads rewritten clips) goes through Register, any
        // other becomes an unread row. A rewritten clip loses its old record first, since its rig may differ.
        void CatchUpWrites() const;
        void RequestRead( const Assets::Asset<Assets::AnimationAsset>& asset ) const;

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
    };

    /// What one population run put in the library, so a caller can say which half is empty.
    struct LibraryPopulation
    {
        size_t FromFiles  = 0; ///< clips registered out of `.anim` assets the scan created
        size_t Procedural = 0; ///< the engine's built-in humanoid locomotion clips
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