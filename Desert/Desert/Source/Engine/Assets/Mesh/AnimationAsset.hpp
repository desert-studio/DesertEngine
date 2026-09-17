#pragma once

#include <Engine/Assets/AssetBase.hpp>
#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/AssetEvents.hpp>

#include <Engine/Animation/AnimationClip.hpp>

namespace Desert::Assets
{
    class AnimationAsset : public AssetBase
    {
    public:
        AnimationAsset( const AssetPriority priority, const Common::Filepath& filepath );

        Common::BoolResultStr LoadFromFile() override;
        Common::BoolResultStr Unload() override;

        const Animation::AnimationClip& GetClip() const
        {
            return m_Clip;
        }

        uint64_t GetSkeletonSignature() const
        {
            return m_SkeletonSignature;
        }

        // Injects an in-memory clip (no file backing) — used for code-generated clips such as the procedural
        // character locomotion ([[procedural-character]]). Create the asset with loadAfterCreate=false, then
        // call this so it shows up in the AnimationLibrary / editor clip selector like a cooked clip.
        void SetInMemoryClip( const Animation::AnimationClip& clip )
        {
            m_Clip               = clip;
            m_Clip.TrackRevision = ++m_TrackRevision;
            m_SkeletonSignature  = clip.SkeletonSignature;
            m_HasClip            = true;
            // NO FILE EVER PRODUCED THIS ONE, so nothing can produce it again. See
            // AssetBase::IsReloadableFromFile.
            m_FromMemory = true;
        }

        // WAS A HARDCODED `return true`. That made the type both unloadable and un-re-loadable:
        // `EnsureLoaded` short-circuits on it, so a shell created with `loadAfterCreate = false` — the
        // documented path for procedural clips, two lines above — reported itself ready while holding an
        // empty clip and an UNINITIALISED `m_SkeletonSignature`, which `GetSkeletonSignature()` then
        // handed to the animation system.
        bool IsReadyForUse() const override
        {
            return m_HasClip;
        }

        bool IsReloadableFromFile() const override
        {
            return !m_FromMemory;
        }

        static AssetTypeID GetTypeID()
        {
            return AssetTypeID::Animation;
        }

    private:
        Animation::AnimationClip m_Clip;

        /**
         * @brief THIS ASSET OWNS THE TRACK-LIST STAMP, because this asset is what replaces the list.
         *
         * `AnimationClip::TrackRevision` needs exactly one writer, and it has to be the object whose
         * lifecycle does the replacing: `Load()` builds a new track list into the SAME `AnimationClip`
         * (same address), and `Unload()` frees it. Everything downstream — `Animator::TrackBinding` above
         * all — has no other way to tell one generation of the list from the next, because `Tracks.data()`
         * and `Tracks.size()` are both free to come back identical when the allocator reuses the block.
         */
        uint32_t m_TrackRevision = 0;
        // WAS UNINITIALISED. `GetSkeletonSignature()` on a shell that had not been loaded returned whatever
        // was on the heap, and the animation system matches rigs on that number.
        uint64_t m_SkeletonSignature = 0;
        bool     m_HasClip           = false;
        bool     m_FromMemory        = false;
    };

} // namespace Desert::Assets