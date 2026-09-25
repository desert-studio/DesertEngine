#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/ViewResources.hpp>

#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
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

    private:
        const uint64_t m_Id;
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

        explicit ViewCopiedBlock( uint32_t size ) : m_Contents( size, 0 )
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

        [[nodiscard]] uint32_t GetSize() const noexcept
        {
            return static_cast<uint32_t>( m_Contents.size() );
        }

        // Drops every copy and starts again from zeroes of `size` bytes (a rebuilt buffer).
        void Reset( const uint32_t size )
        {
            Graphic::ViewResourceRegistry::Forget( m_Key );
            m_Contents.assign( size, 0 );
        }

        // Grows the block to `newSize` bytes (a storage buffer a write outgrew). Every copy is dropped —
        // each view re-makes its own at the new size on its next write or bind — but the CPU image KEEPS
        // its contents, so a view whose new copy is made later still starts from what was written before
        // the growth rather than from zeroes. A block never shrinks here: shrinking is Reset.
        void Grow( const uint32_t newSize )
        {
            Graphic::ViewResourceRegistry::Forget( m_Key );
            if ( newSize > m_Contents.size() )
                m_Contents.resize( newSize, 0 );
        }

        // The CPU image new copies are seeded from: the bytes of every write so far, whichever view made it.
        [[nodiscard]] const uint8_t* GetContents() const noexcept
        {
            return m_Contents.data();
        }

        // The active view's copy for `frameIndex`, created and seeded on first use.
        [[nodiscard]] Common::BoolResultStr Resolve( const uint32_t frameIndex, const CopyMaker& make,
                                                     IBlockCopy*& out )
        {
            Graphic::ViewResources& view = Graphic::ViewResourceRegistry::Active();
            if ( Graphic::IViewResourceCopy* existing = view.Find( m_Key, frameIndex ) )
            {
                // Only this class inserts under m_Key, and it only inserts IBlockCopy.
                // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
                out = static_cast<IBlockCopy*>( existing );
                return Common::MakeSuccess( true );
            }

            // Every refusal is produced BEFORE Acquire: Acquire throws on a null copy, and a view must never
            // be handed a copy that could not be seeded.
            std::unique_ptr<IBlockCopy> made;
            auto                        created = make( view.GetName(), frameIndex, made );
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

        // Writes into the active view's copy and into the CPU image future copies are seeded from.
        [[nodiscard]] Common::BoolResultStr Write( const void* data, const uint32_t size, const uint32_t offset,
                                                   const uint32_t frameIndex, const CopyMaker& make )
        {
            if ( static_cast<uint64_t>( offset ) + size > m_Contents.size() )
                return Common::MakeFormattedError<bool>( "{} byte(s) at offset {} do not fit a {}-byte block",
                                                         size, offset, m_Contents.size() );
            if ( data == nullptr && size != 0 )
                return Common::MakeError<bool>( "a block write was given a null source" );

            // Resolved BEFORE the image changes, so a copy created here is seeded with the previous
            // contents and then receives this write like any existing one — one path, not two.
            IBlockCopy* copy     = nullptr;
            auto        resolved = Resolve( frameIndex, make, copy );
            if ( !resolved.IsSuccess() )
                return resolved;

            if ( size != 0 )
                std::memcpy( m_Contents.data() + offset, data, size );
            return copy->Write( data, size, offset );
        }

    private:
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
    };

    /**
     * @brief Which copy each binding of one descriptor set points at, and whether it has to be rewritten.
     *
     * A set was written only while its property was dirty. With per-view copies that is not enough: close
     * a preview (its copies go to deferred deletion), open another that reuses the same renderer slot, and
     * the slot's set still points at the destroyed buffer while the property reports itself clean — a
     * use-after-free on the GPU. So the set is also rewritten whenever the copy the view would bind now is
     * not the copy the set was last written with.
     */
    class DescriptorCopyRecord
    {
    public:
        [[nodiscard]] bool NeedsWrite( const uint32_t binding, const uint64_t copyId,
                                       const bool propertyDirty ) const
        {
            if ( propertyDirty )
                return true;
            const auto it = m_Bound.find( binding );
            return it == m_Bound.end() || it->second != copyId;
        }

        void NoteWritten( const uint32_t binding, const uint64_t copyId )
        {
            m_Bound[binding] = copyId;
        }

    private:
        std::unordered_map<uint32_t, uint64_t> m_Bound; // binding -> IBlockCopy::GetId()
    };
} // namespace Desert::ShaderResources
