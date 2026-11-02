#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/ResourceLedger.hpp>
#include <Engine/Graphic/ViewResources.hpp>
#include <Engine/Graphic/RDG/RDGRecordPrerequisite.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Desert::ShaderResources
{
    /**
     * @brief One view's copy of a CPU-written block for one frame in flight — a mapped VkBuffer in
     * VulkanUniformBuffer and VulkanStorageBuffer, plain bytes in Tests/Engine/MaterialParamUpload.
     *
     * THE ID IS WHAT A DESCRIPTOR SET REMEMBERS, NOT THE VkBuffer. A set written for a copy that was later
     * dropped (its view closed) must be rewritten before the next bind, and the only way to know is to
     * compare what the set points at with what the view would bind now. A VkBuffer handle cannot answer
     * that: the allocator hands a freed handle straight back to the next allocation, so a set pointing at a
     * destroyed buffer would compare EQUAL to a new copy that happened to get the same handle, and the
     * rewrite would be skipped. The id comes from the same never-reused counter as ViewResourceKey.
     */
    class IBlockCopy : public Graphic::IViewResourceCopy
    {
    public:
        IBlockCopy() : m_Id( Graphic::ViewResourceKey::Allocate().Value() )
        {
        }

        [[nodiscard]] uint64_t GetId() const noexcept
        {
            return m_Id;
        }

        [[nodiscard]] virtual Common::BoolResultStr Write( const void* data, uint32_t size, uint32_t offset ) = 0;

        // The newest field version (Graphic::PropertyVersion) the field route has written into THIS copy. A new
        // copy starts at 0, behind every write, and takes each written field once (its seed already holds the
        // bytes, so that is idempotent). Kept per copy because "dirty" is a question about one (view x frame)
        // copy: a copy another view brought up to date is not up to date itself.
        //
        // RDG-PARALLEL-5: two chunks of one graph bind materials that share this copy in the same frame, so the
        // version is an atomic that only moves forward. Each writer has delivered every field up to the version
        // it notes, so the larger of two notes is what the copy holds, whichever lands last.
        [[nodiscard]] uint64_t GetAppliedVersion() const noexcept
        {
            return m_AppliedVersion.load( std::memory_order_acquire );
        }

        void NoteApplied( const uint64_t version ) noexcept
        {
            uint64_t current = m_AppliedVersion.load( std::memory_order_relaxed );
            while ( current < version &&
                    !m_AppliedVersion.compare_exchange_weak( current, version, std::memory_order_acq_rel,
                                                             std::memory_order_relaxed ) )
            {
            }
        }

    private:
        const uint64_t        m_Id;
        std::atomic<uint64_t> m_AppliedVersion{ 0 };
    };

    /**
     * @brief The per-view copies of one CPU-written block, plus the CPU image every NEW copy starts from.
     *
     * The policy both VulkanUniformBuffer and its test double run, in one place so the suite asserts the
     * production rule rather than a restatement of it:
     *
     *   - A write lands in the copy of the view that is ACTIVE (ViewResourceRegistry::Active), which is
     *     the frame context outside every ActiveViewScope. Never "the last view that recorded".
     *   - A write never touches another view's EXISTING copy — that is the isolation this whole layer is
     *     for (a preview's camera must not land on the viewport's).
     *   - A copy that does not exist yet is created on first use and SEEDED with the CPU image of the last
     *     write, not zeroed. Under renderer slots every copy existed from construction and a preview opened
     *     in a reused slot inherited the old copy's contents; lazily created copies would otherwise start
     *     at zero, and a material whose parameters were uploaded once (the field route, dirty for a window
     *     of frames and then clean) would render with zero parameters in any view opened after the window.
     */
    class ViewCopiedBlock
    {
    public:
        // Makes one copy for (view, frame). Returns the refusal with its cause; `out` is set only on success.
        using CopyMaker = std::function<Common::BoolResultStr( std::string_view viewName, uint32_t frameIndex,
                                                               std::unique_ptr<IBlockCopy>& out )>;

        explicit ViewCopiedBlock( uint32_t size ) : m_Contents( size, 0 ), m_Size( size )
        {
        }

        // Takes its copies back from every live view and the frame context: a destroyed block must not
        // leave copies behind that a view keeps paying for until it closes.
        ~ViewCopiedBlock()
        {
            Graphic::ViewResourceRegistry::Forget( m_Key );
        }

        ViewCopiedBlock( const ViewCopiedBlock& )            = delete;
        ViewCopiedBlock& operator=( const ViewCopiedBlock& ) = delete;
        ViewCopiedBlock( ViewCopiedBlock&& )                 = delete;
        ViewCopiedBlock& operator=( ViewCopiedBlock&& )      = delete;

        [[nodiscard]] Graphic::ViewResourceKey GetKey() const noexcept
        {
            return m_Key;
        }

        // RDG-PARALLEL-6: read from any thread. A chunk asks the size (ClassifyBufferWrite) while another may be
        // growing the block under m_Lock, so the answer is a mirror of m_Contents.size(), never the vector itself.
        // A block only grows between Resets, so a stale answer is smaller, and Write re-checks under the lock.
        [[nodiscard]] uint32_t GetSize() const noexcept
        {
            return m_Size.load( std::memory_order_acquire );
        }

        // Drops every copy and starts again from zeroes of `size` bytes (a rebuilt buffer).
        void Reset( const uint32_t size )
        {
            std::scoped_lock lock( m_Lock );
            Graphic::ViewResourceRegistry::Forget( m_Key );
            m_Contents.assign( size, 0 );
            m_Size.store( size, std::memory_order_release );
        }

        // Grows the block to `newSize` bytes (a storage buffer a write outgrew). Every copy is dropped —
        // each view re-makes its own at the new size on its next write or bind — but the CPU image KEEPS
        // its contents, so a view whose new copy is made later still starts from what was written before
        // the growth rather than from zeroes. A block never shrinks here: shrinking is Reset.
        void Grow( const uint32_t newSize )
        {
            std::scoped_lock lock( m_Lock );
            Graphic::ViewResourceRegistry::Forget( m_Key );
            if ( newSize > m_Contents.size() )
            {
                m_Contents.resize( newSize, 0 );
                m_Size.store( newSize, std::memory_order_release );
            }
        }

        // The CPU image new copies are seeded from: the bytes of every write so far, whichever view made it.
        // Read it where no pass records (setup, tests): a recording pass may be writing it under m_Lock.
        [[nodiscard]] const uint8_t* GetContents() const noexcept
        {
            return m_Contents.data();
        }

        // The active view's copy for `frameIndex`, created and seeded on first use.
        //
        // THREADING (RDG-PARALLEL-5). The lookup takes no lock of this block: ViewResources::Find is safe from
        // any chunk, and a recording pass only ever finds (the seal in MakeActive refuses a make). Making a copy
        // reads the CPU image to seed it, so that path runs under m_Lock, the lock every write of the image holds.
        [[nodiscard]] Common::BoolResultStr Resolve( const uint32_t frameIndex, const CopyMaker& make,
                                                     IBlockCopy*& out )
        {
            if ( FindActive( frameIndex, out ) )
                return Common::MakeSuccess( true );
            std::scoped_lock lock( m_Lock );
            return MakeActive( frameIndex, make, out );
        }

        // The version the ACTIVE view's copy for `frameIndex` has applied; 0 (behind every write) when that copy
        // does not exist yet.
        [[nodiscard]] uint64_t ActiveAppliedVersion( const uint32_t frameIndex ) const
        {
            const Graphic::IViewResourceCopy* existing =
                 Graphic::ViewResourceRegistry::Active().Find( m_Key, frameIndex );
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
            return existing != nullptr ? static_cast<const IBlockCopy*>( existing )->GetAppliedVersion() : 0;
        }

        // Records that the ACTIVE view's copy for `frameIndex` holds every field write up to `version`. A copy
        // that does not exist has applied nothing, so there is nothing to record.
        void NoteActiveApplied( const uint32_t frameIndex, const uint64_t version )
        {
            if ( Graphic::IViewResourceCopy* existing =
                      Graphic::ViewResourceRegistry::Active().Find( m_Key, frameIndex ) )
                // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
                static_cast<IBlockCopy*>( existing )->NoteApplied( version );
        }

        // Writes into the active view's copy and into the CPU image future copies are seeded from.
        //
        // THREADING (RDG-PARALLEL-5). Two chunks of one graph may bind materials that share this block in the
        // same frame: the same view, so the same copy. The CPU image and the copy's bytes are written as one
        // unit under m_Lock: no torn range in either, and the image always equals what the last write left in
        // the copy. Which of two DIFFERENT writes of one range lands last is the chunks' order, as it is for the
        // GPU, which reads the one copy for both passes; a block two passes need different bytes in is per-pass
        // state, not a shared block.
        [[nodiscard]] Common::BoolResultStr Write( const void* data, const uint32_t size, const uint32_t offset,
                                                   const uint32_t frameIndex, const CopyMaker& make )
        {
            if ( data == nullptr && size != 0 )
                return Common::MakeError<bool>( "a block write was given a null source" );

            std::scoped_lock lock( m_Lock );
            if ( static_cast<uint64_t>( offset ) + size > m_Contents.size() )
                return Common::MakeFormattedError<bool>( "{} byte(s) at offset {} do not fit a {}-byte block",
                                                         size, offset, m_Contents.size() );

            // Resolved BEFORE the image changes, so a copy created here is seeded with the previous
            // contents and then receives this write like any existing one: one path, not two.
            IBlockCopy* copy = nullptr;
            if ( !FindActive( frameIndex, copy ) )
            {
                auto made = MakeActive( frameIndex, make, copy );
                if ( !made.IsSuccess() )
                    return made;
            }

            if ( size != 0 )
                std::memcpy( m_Contents.data() + offset, data, size );
            return copy->Write( data, size, offset );
        }

    private:
        // The active view's existing copy for `frameIndex`, if any. No lock of this block: Find locks the view.
        [[nodiscard]] bool FindActive( const uint32_t frameIndex, IBlockCopy*& out ) const
        {
            Graphic::IViewResourceCopy* existing =
                 Graphic::ViewResourceRegistry::Active().Find( m_Key, frameIndex );
            if ( existing == nullptr )
                return false;
            // Only this class inserts under m_Key, and it only inserts IBlockCopy.
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
            out = static_cast<IBlockCopy*>( existing );
            return true;
        }

        // Makes, seeds and files the active view's copy for `frameIndex`. The caller holds m_Lock: the seed is
        // the CPU image, and a copy another caller filed while this one waited for the lock is taken as it is.
        [[nodiscard]] Common::BoolResultStr MakeActive( const uint32_t frameIndex, const CopyMaker& make,
                                                        IBlockCopy*& out )
        {
            if ( FindActive( frameIndex, out ) )
                return Common::MakeSuccess( true );
            Graphic::ViewResources& view = Graphic::ViewResourceRegistry::Active();

            // RENDER-THREADS M1: no copy is made while a sealed pass records (UE: a pass's uniform buffers
            // exist before the graph records). The pass prepares the material that owns the block in its setup.
            if ( Graphic::RDG::RecordingSeal::IsSealed() )
                return Common::MakeFormattedError<bool>(
                     "view '{}', frame {}: the buffer's copy does not exist and "
                     "a recording pass may not make it; the pass declares the "
                     "material in its setup (PassBuilder::Prepares)",
                     view.GetName(), frameIndex );

            // Every refusal is produced BEFORE Acquire: Acquire throws on a null copy, and a view must never
            // be handed a copy that could not be seeded.
            // A COPY'S ROW BELONGS TO THE VIEWRESOURCES IT IS FILED IN. A copy made outside every
            // ActiveViewScope is the frame context's, which lives for the process; made inside a view's
            // ledger scope (Init, the staged build, Resize, the Ensure* builders) it would otherwise carry
            // that view's instance and outlive the view's close as the view's rows. So the frame context's
            // copies are the device's rows, with no view instance — the way LocalShadowCache books its atlas
            // to the scene. Redirecting the write into the view instead (ActiveViewScope around those entry
            // points) blacked the sky (bisected to that change): writes keep going where they went.
            std::optional<Graphic::ResourceAttributionScope> frameContextRows;
            if ( &view == &Graphic::ViewResourceRegistry::FrameContext() )
                frameContextRows.emplace( Graphic::ResourceOwner::Device );
            std::unique_ptr<IBlockCopy> made;
            auto                        created = make( view.GetName(), frameIndex, made );
            frameContextRows.reset();
            if ( !created.IsSuccess() )
                return created;
            if ( !made )
                return Common::MakeFormattedError<bool>( "view '{}', frame {}: the copy maker reported success "
                                                         "and returned no copy",
                                                         view.GetName(), frameIndex );

            const auto seeded = made->Write( m_Contents.data(), GetSize(), 0 );
            if ( !seeded.IsSuccess() )
                return Common::MakeFormattedError<bool>( "view '{}', frame {}: seeding the new copy with {} "
                                                         "byte(s) failed: {}",
                                                         view.GetName(), frameIndex, GetSize(),
                                                         seeded.GetError() );

            HandOver handOver( std::move( made ) );
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
            out = static_cast<IBlockCopy*>( &view.Acquire( m_Key, frameIndex, handOver ) );
            return Common::MakeSuccess( true );
        }

        // Acquire takes a factory; the copy is already made (and seeded), so this one just hands it over.
        class HandOver final : public Graphic::IViewResourceCopyFactory
        {
        public:
            explicit HandOver( std::unique_ptr<IBlockCopy> copy ) : m_Copy( std::move( copy ) )
            {
            }

            std::unique_ptr<Graphic::IViewResourceCopy> CreateViewCopy( std::string_view, uint32_t ) override
            {
                return std::move( m_Copy );
            }

        private:
            std::unique_ptr<IBlockCopy> m_Copy;
        };

        Graphic::ViewResourceKey m_Key = Graphic::ViewResourceKey::Allocate();
        std::vector<uint8_t>     m_Contents;
        // m_Contents.size(), written under m_Lock with it, read without it (GetSize).
        std::atomic<uint32_t>    m_Size{ 0 };
        // Guards m_Contents and the write of the active copy's bytes (Write), and the make-and-seed of a copy
        // (MakeActive). Taken before any ViewResources lock, never the other way round.
        std::mutex m_Lock;
    };

    /**
     * @brief What each binding of one descriptor set was last written with: the resource (a buffer copy's id,
     * or an image view's handle) and the property version (Graphic::PropertyVersion) it carried.
     *
     * The set is rewritten when either differs from what the view would bind now. The resource half closes the
     * closed-preview use-after-free: a set pointing at a copy that was dropped with its view must be rewritten
     * although nothing was written to the property. The version half replaces the per-slot dirty countdown: a
     * set that has not applied the property's latest write is behind, however many frames later it is bound.
     */
    class DescriptorCopyRecord
    {
    public:
        // RDG-PARALLEL-9 (UE: the descriptor cache keys a material's set by what it holds): the process-unique
        // number of this record. With GetVersion it names the descriptors the record's set holds: a set copied
        // from it (VulkanRenderer's graph route, set 0 of a material draw) is cached under the pair and found
        // again only while the pair is unchanged. A copy or a move is another set's record and takes an identity
        // of its own (RecordIdentity), so two records never share one.
        [[nodiscard]] uint64_t GetIdentity() const noexcept
        {
            return m_Identity.Get();
        }

        // Raised by every NoteWritten: any write into the record's set, also of an equal resource, is a new version.
        [[nodiscard]] uint64_t GetVersion() const noexcept
        {
            return m_Version;
        }

        [[nodiscard]] bool NeedsWrite( const uint32_t binding, const uint64_t resourceId,
                                       const uint64_t propertyVersion ) const
        {
            const auto it = m_Bound.find( binding );
            return it == m_Bound.end() || it->second.ResourceId != resourceId ||
                   it->second.Version != propertyVersion;
        }

        void NoteWritten( const uint32_t binding, const uint64_t resourceId, const uint64_t propertyVersion )
        {
            if ( m_Bound.insert_or_assign( binding, Written{ resourceId, propertyVersion } ).second )
                m_Written.insert( std::lower_bound( m_Written.begin(), m_Written.end(), binding ), binding );
            ++m_Version;
        }

        // Every binding written with a resource of its own (a fresh set holds only fallbacks and has none),
        // sorted.
        // CPU-FRAME-5: kept sorted by NoteWritten (the one writer of m_Bound), so a draw reading it allocates
        // and sorts nothing. Valid until the next NoteWritten.
        [[nodiscard]] const std::vector<uint32_t>& GetWrittenBindings() const
        {
            return m_Written;
        }

    private:
        struct Written
        {
            uint64_t ResourceId = 0;
            uint64_t Version    = 0;
        };
        // A number no other record of the process holds: taken afresh on construction, copy and move (both
        // directions of an assignment included), so the record stays rule-of-zero.
        class RecordIdentity
        {
        public:
            RecordIdentity() noexcept : m_Value( Next() ) {}
            RecordIdentity( const RecordIdentity& /*other*/ ) noexcept : m_Value( Next() ) {}
            RecordIdentity( RecordIdentity&& /*other*/ ) noexcept : m_Value( Next() ) {}
            RecordIdentity& operator=( const RecordIdentity& /*other*/ ) noexcept
            {
                m_Value = Next();
                return *this;
            }
            RecordIdentity& operator=( RecordIdentity&& /*other*/ ) noexcept
            {
                m_Value = Next();
                return *this;
            }
            ~RecordIdentity() = default;

            [[nodiscard]] uint64_t Get() const noexcept
            {
                return m_Value;
            }

        private:
            static uint64_t Next() noexcept
            {
                static std::atomic<uint64_t> s_Next{ 1 };
                return s_Next.fetch_add( 1, std::memory_order_relaxed );
            }
            uint64_t m_Value;
        };

        std::unordered_map<uint32_t, Written> m_Bound;
        std::vector<uint32_t>                 m_Written; // m_Bound's bindings, sorted
        RecordIdentity                        m_Identity;
        uint64_t                              m_Version = 0; // raised by NoteWritten
    };
} // namespace Desert::ShaderResources
