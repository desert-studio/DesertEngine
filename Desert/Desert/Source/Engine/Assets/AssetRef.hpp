#pragma once

#include <Common/Core/Core.hpp>

#include "Common.hpp"

#include <memory>
#include <utility>

namespace Desert::Assets
{
    /**
     * @brief A NAMED THING THAT MAY NOT BE HERE YET — three states, and `Get()` never loads.
     *
     * WHY A TYPE AND NOT A POINTER. Every cloud consumer in this engine asked a service for a raw
     * pointer and branched on null, and null answered two questions at once: *"the artist chose
     * nothing"* and *"the artist chose something that has not arrived"*. While every content kind was
     * loaded eagerly before the first frame those two could not be told apart because the second one
     * could not happen. The moment a kind becomes demand-driven it happens on every scene load, and
     * the existing branch turns a normal in-flight load into the diagnostic for a broken reference:
     * `VolumetricCloudRenderer::EnsureNoiseVolumes` logs *"the clouds will not render for this view
     * until one is registered"* and the sky silently falls back to its procedural form. That is the
     * quiet degradation `Docs/World/GAP_ANALYSIS.md` §3.1 names as the trap in this whole tier —
     * laziness without an expressible "not here yet" does not remove a cost, it hides one.
     *
     * THE THREE STATES ARE DERIVED, NOT STORED SEPARATELY. There is no `State` enum field that a
     * setter could leave disagreeing with the payload it describes; the two data members below are the
     * single source and the three predicates are three readings of them:
     *
     *   | `m_Payload` | `m_Requested` | state     | `Get()`      |
     *   |-------------|---------------|-----------|--------------|
     *   | non-null    | either        | **Ready** | the payload  |
     *   | null        | `true`        | **Pending** | `nullptr`  |
     *   | null        | `false`       | **Null**  | `nullptr`    |
     *
     * They are mutually exclusive and exhaustive by construction, which is the property
     * `AssetRefStates` asserts and the property a merge of any two of them breaks.
     *
     * WHY `Get()` MUST NOT LOAD, stated as a rule rather than as an optimisation: a getter that loads
     * is a getter whose cost depends on who called it first, and the whole of Tier 0's timing work
     * exists because such a getter (`MeshService::Get`) parses a 40 MB file inside whichever frame
     * happens to touch the mesh. A `Get()` that can block is a `Get()` no caller can be written
     * against. Ours returns `nullptr` and the caller asks `IsPending()` to find out whether waiting is
     * the right answer.
     *
     * WHAT `Null` MEANS WHEN A LOAD FAILED. A request that came back with a parse error resolves to
     * `Null`, not to a fourth state: for every consumer in this tree "the file is broken" and "there is
     * no file" lead to the same action — do not draw this, the reason is already in the log — and a
     * state nobody branches on differently is a state that only costs a branch. The service that
     * issued the request is where the failure is named, once, with the path in the message.
     *
     * @tparam T the payload. Deliberately NOT constrained to `AssetBase`: the three cloud services
     *           hand back a GPU image or a decoded layout rather than the asset object, and "the
     *           uploaded volume has not arrived" is the same statement as "the asset has not arrived".
     */
    template <typename T>
    class AssetRef final
    {
    public:
        AssetRef() = default;

        /// Nothing was ever asked for. The empty slot; for every cloud kind this is the shipped state.
        [[nodiscard]] static AssetRef Null()
        {
            return AssetRef{};
        }

        /// @p handle was asked for and has not arrived. The handle is kept for the diagnostic, never
        /// to load from: this type has no loader and cannot acquire one.
        [[nodiscard]] static AssetRef Pending( const AssetHandle& handle )
        {
            AssetRef ref;
            ref.m_Handle    = handle;
            ref.m_Requested = true;
            return ref;
        }

        /// @p payload is here. A null @p payload would be a Ready state with nothing in it — the empty
        /// successful answer this project has a rule against — so it is refused into `Null()`.
        [[nodiscard]] static AssetRef Ready( const AssetHandle& handle, std::shared_ptr<T> payload )
        {
            if ( !payload )
                return Null();

            AssetRef ref;
            ref.m_Handle    = handle;
            ref.m_Requested = true;
            ref.m_Payload   = std::move( payload );
            return ref;
        }

        /// The payload is here and `Get()` will answer with it.
        [[nodiscard]] bool IsValid() const noexcept
        {
            return m_Payload != nullptr;
        }

        /// Asked for, not here. The caller should wait rather than fall back, and must not report a
        /// missing reference.
        [[nodiscard]] bool IsPending() const noexcept
        {
            return m_Payload == nullptr && m_Requested;
        }

        /// Nothing was asked for, or what was asked for does not exist. The caller's fallback — a
        /// default volume, a procedural sky, no hero cloud — is the right answer here and only here.
        [[nodiscard]] bool IsNull() const noexcept
        {
            return m_Payload == nullptr && !m_Requested;
        }

        /// NEVER LOADS. `nullptr` unless `IsValid()`.
        [[nodiscard]] T* Get() const noexcept
        {
            return m_Payload.get();
        }

        /// The payload, kept alive. Empty unless `IsValid()`.
        [[nodiscard]] const std::shared_ptr<T>& Share() const noexcept
        {
            return m_Payload;
        }

        /// Which asset this names, in every state including `Null` — a pending ref that could not name
        /// its handle would be a diagnostic that says only "something is late".
        [[nodiscard]] const AssetHandle& Handle() const noexcept
        {
            return m_Handle;
        }

        /// ASSERTS, because reaching through a reference that is not here is a caller defect and not a
        /// runtime condition: the caller had `IsValid()` available and did not ask. In a build with
        /// assertions compiled out this dereferences null, which is the same crash the caller would
        /// have got from the raw pointer it used to hold — this type does not make that case safe, it
        /// makes it loud.
        [[nodiscard]] T* operator->() const noexcept
        {
            DESERT_VERIFY( IsValid() );
            return m_Payload.get();
        }

        [[nodiscard]] T& operator*() const noexcept
        {
            DESERT_VERIFY( IsValid() );
            return *m_Payload;
        }

        /// Deliberately absent: `if ( ref )` would read as "is there a reference" and answer "is the
        /// payload here", which is the exact conflation this type exists to remove.
        explicit operator bool() const = delete;

    private:
        AssetHandle        m_Handle{ 0 };
        std::shared_ptr<T> m_Payload;
        bool               m_Requested = false;
    };
} // namespace Desert::Assets
