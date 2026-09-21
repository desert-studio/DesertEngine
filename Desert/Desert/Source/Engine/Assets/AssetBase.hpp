#pragma once

#include <Common/Core/AssetPathIndex.hpp>
#include <Common/Core/ResultStr.hpp>
#include <Common/Core/Core.hpp>
#include <Common/Core/UUID.hpp>

#include "Common.hpp"
#include "AssetMetadata.hpp"
#include "SyncLoadLedger.hpp"

namespace Desert::Assets
{
    class MeshAsset;
    class AssetManager;

    class AssetBase
    {
    public:
        virtual ~AssetBase() = default;

        virtual const AssetMetadata& GetMetadata() const final
        {
            return m_Metadata;
        }

        virtual void ResolveDependencies( AssetManager& /*manager*/ )
        {
        }

        /**
         * @brief READ THIS ASSET'S FILE. Non-virtual on purpose — this is the timed chokepoint.
         *
         * Every synchronous asset load in the engine passes through exactly here, and it is the only
         * function that can say so. `Load()` was pure virtual until the world programme needed to know
         * how much of a boot, and how much of a FRAME, was spent reading files: at that point it was
         * reached from 45 call sites outside the tests, so instrumenting the callers meant editing 45
         * places and being wrong the moment a forty-sixth appeared. Instrumenting the function they all
         * call means being right by construction.
         *
         * A subclass cannot take this over. `override` does not apply to a non-virtual member, so the
         * old spelling fails to COMPILE rather than quietly bypassing the timer — and
         * `Desert/Tests/Engine/SyncLoadChokepoint` covers the one remaining route, a fresh declaration
         * without `override`, by asserting over the source text that no subclass declares `Load()` at
         * all. See Engine/Assets/SyncLoadLedger.hpp for what is counted and why nesting is timed once.
         */
        // DELIBERATELY NOT `NO_DISCARD`, even though ignoring a load failure is a defect. Nineteen of
        // the 45 existing call sites discard the result (`a->Load();`), and adding the attribute here
        // would emit nineteen new warnings in a tree built with warnings-as-errors on one platform — a
        // detector that does not compile has changed behaviour, which is the one thing it must not do.
        // Whether those nineteen should be checked is a real question and a separate one.
        Common::BoolResultStr Load()
        {
            // THE PATH IS TAKEN BEFORE THE CALL. Two asset types replace `m_Metadata.Handle` from an id
            // inside the file during the load, and `TextureAsset` also rewrites where it thinks its
            // source lives — reading the path afterwards would attribute the time to whatever the file
            // said rather than to the file we opened.
            const LoadTimingScope timing( m_Metadata.Filepath.string() );
            return LoadFromFile();
        }

        // RELEASE THIS ASSET'S PAYLOAD, KEEPING ITS IDENTITY.
        //
        // The contract every implementation now obeys, and none of them did before there was a caller:
        //
        //   1. It releases what the type OWNS on the heap, `shrink_to_fit` included — `clear()` alone
        //      keeps the capacity, which is most of the memory for a mesh or a prefab.
        //   2. It leaves `IsReadyForUse()` FALSE. That flag is what `EnsureLoaded` asks before deciding to
        //      parse, so an emptied asset that still reports "ready" is one nobody will ever reload. Five
        //      of the thirteen bodies failed this and were therefore not merely ineffective but
        //      irreversible.
        //   3. It does NOT touch `m_Metadata.Handle`. Two types replace their handle during Load from an
        //      id inside the file, and that id is what every service map and every `.demat` reference
        //      resolves against. An evicted asset keeps its identity or the reload is a different asset.
        //   4. It REFUSES, with a reason, when `IsReloadableFromFile()` is false. Releasing an asset that
        //      was filled in memory is data loss wearing eviction's clothes, and a silent no-op would be
        //      §1.4's "empty successful answer" — the caller would read it as released.
        //
        // It releases nothing on the GPU, because no asset in this engine owns anything on the GPU: the
        // device objects built from an asset live in a `Runtime::*Service` keyed on its handle, and it is
        // the service that releases them (Engine/Assets/AssetEviction.hpp). That is also why every one of
        // these bodies is safe to run with a frame in flight — the vectors they free were copied into
        // device buffers at build time and are read by nothing afterwards.
        virtual Common::BoolResultStr Unload() = 0;

        virtual bool IsReadyForUse() const = 0;

        // CAN THIS ASSET BE REBUILT BY READING ITS FILE AGAIN?
        //
        // For almost every asset, yes: the file is the recipe and `Load()` is the whole of the rebuild.
        // Three types have a SECOND producer that fills them from memory with no file behind it — a prefab
        // captured from a live entity, a procedurally generated animation clip, the Material Editor's
        // working copy — and for those, releasing the payload destroys the only copy that exists.
        //
        // It is a QUESTION ON THE ASSET rather than a list inside the evictor because the evictor cannot
        // see how an asset was filled, and a list somewhere else is a second place that has to agree with
        // this one. `Desert/Tests/Engine/AssetEviction` asserts that every type answering `false` here
        // also refuses `Unload()`, so the two halves of the guard cannot drift apart.
        virtual bool IsReloadableFromFile() const
        {
            return true;
        }

        // LOADING AND RESOLVING ARE ONE STEP, and this is the only entry point that says so.
        //
        // A dependency named by a field INSIDE the file cannot be resolved before the file is parsed.
        // AssetManager::CreateAsset resolves at construction, which is correct for an eagerly-loaded asset
        // and MEANINGLESS for a lazy shell: the field the lookup keys on is still zero, so the resolve runs,
        // finds nothing, and is never repeated. That is exactly how every skinned mesh in a scene became
        // invisible — SkinnedMeshAsset's skeleton is matched by a signature stored in the .skmesh, the shell
        // carried 0, and the only code that ever asked a second time was the editor's drag-and-drop.
        //
        // Callers therefore never write `Load()` and `ResolveDependencies()` as two statements: the second
        // one is what gets forgotten, and nothing fails loudly when it is. `Load()` stays public for the
        // reload paths, which re-resolve deliberately because the EDIT may have been the dependency.
        NO_DISCARD Common::BoolResultStr EnsureLoaded( AssetManager& manager )
        {
            if ( IsReadyForUse() )
            {
                return BOOLSUCCESS;
            }

            if ( const auto loaded = Load(); !loaded )
            {
                return loaded;
            }

            ResolveDependencies( manager );
            return BOOLSUCCESS;
        }

        // EVERY asset's identity is derived from its path, here, once.
        //
        // This used to be `Common::UUID()`, which minted a fresh random id per construction, so an asset's
        // handle was a property of the LAUNCH rather than of the file. Five types lived with that — prefab,
        // skybox, shader, skeleton, animation — while mesh, texture, material and the three cloud types had
        // each grown their own copy of the path-derived line to escape it. Nine copies of one rule is how
        // the tenth type gets forgotten, and the symptom when it is forgotten is a reference that resolves
        // to nothing after a restart with nothing logged.
        //
        // Subclasses whose FILE carries an id of its own (a cooked `.tex` header, a `.demat`'s MaterialId)
        // still overwrite this in Load: an id stored in the file additionally survives a rename, which a
        // path-derived one cannot. That is a strictly better identity for the same asset, not a second way
        // of doing the same thing.
        explicit AssetBase( const AssetPriority priority, const Common::Filepath& filepath, AssetTypeID assetType )
             : m_Metadata{ Common::AssetHandle::FromCookedPath( filepath ), filepath, priority, assetType }
        {
        }

    protected:
        /// Installs an identity that came OUT OF THE FILE, over the path-derived one the constructor
        /// above put there, and records the inverse for it.
        ///
        /// WHY A HELPER FOR TWO CALLERS. The recording is the half that is easy to forget, and forgetting
        /// it is silent: the asset works, its references resolve, and only a handle->path lookup comes
        /// back empty — a whole session later, with no filename in the miss. The constructor's own comment
        /// is about exactly this failure ("nine copies of one rule is how the tenth type gets
        /// forgotten"), so the rule that replaces one of those copies gets one spelling, not two.
        ///
        /// `stableKey` is a PARAMETER because the two adopters do not agree about which file the adopted
        /// number names, and both are right: a `.demat`'s MaterialId names the `.demat`, while a cooked
        /// `.tex`'s stored Handle names its SOURCE IMAGE (TextureImporter mints it from that path). A
        /// helper that derived the key from `m_Metadata.Filepath` would quietly make the texture case
        /// wrong, which is the one with 95 numeric references in shipped content.
        void AdoptHandleFromFile( const Common::UUID& handle, const std::string& stableKey )
        {
            m_Metadata.Handle = handle;
            // Discarded for the reason FromCookedPath discards it: the only failure is a collision, this
            // function cannot repair one, and AssetPathIndex::Record has already named both keys and the
            // number in the log.
            static_cast<void>( Common::AssetPathIndex::Record( static_cast<uint64_t>( handle ), stableKey ) );
        }

        /// THE PER-TYPE HALF OF `Load()`. Reads the file and fills the type; says nothing about timing.
        ///
        /// Protected rather than public because the public half is the whole contract: a caller reaching
        /// a `LoadFromFile` directly would be a load the ledger never saw, which is exactly the hole this
        /// split closes. `AssetBase::Load()` is its only caller in the engine.
        virtual Common::BoolResultStr LoadFromFile() = 0;

        AssetMetadata m_Metadata;
    };

} // namespace Desert::Assets