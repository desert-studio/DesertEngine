#pragma once

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/Core.hpp>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Desert::Graphic
{
    /**
     * @brief WHO OWNS EVERY LIVE GPU OBJECT — one ledger, and the entry in it cannot outlive the object.
     *
     * WHY THIS EXISTS, IN THE ENGINE'S OWN WORDS. `DeviceLost.cpp` refuses device-loss recovery and names
     * the missing piece: "WHAT WOULD CHANGE THE ANSWER: a resource ownership registry that can enumerate
     * every live GPU object (wanted for other reasons too — teardown order, hot reload, and the 'which
     * slot is this?' question the renderer-slot work keeps asking)." Nothing enumerated them. There was no
     * counter of GPU objects, no counter of GPU memory, and `VulkanAllocator::CheckResourceLeaks()` had an
     * empty body. `ImageService` looked like the registry and is not: `Register()` is reached from eight
     * call sites, while every renderer LUT, framebuffer attachment, font atlas and panel image is created
     * through `Image2D::Create` and never seen again.
     *
     * ASSET EVICTION IS THE FIRST CONSUMER, NOT THE SUBJECT. An asset in this engine owns NOTHING on the
     * GPU — every `AssetBase` subclass holds CPU data and a path, and the GPU object built from it lives in
     * a `Runtime::*Service` keyed on the asset's handle. So a registry of ASSETS cannot answer "what is on
     * the device", and a registry that could was going to be written twice — once for eviction and once
     * for device loss. It is written once, here, and eviction reads it.
     *
     * ── THE SHAPE, AND WHY IT IS A TYPE RATHER THAN A PAIR OF CALLS ───────────────────────────────────
     *
     * `MappedMemory` (beside this file) is the model: the contract is guarded by the TYPE, not by checks
     * the caller has to remember. A `Register()`/`Unregister()` pair is thirteen places for a fourteenth
     * to be forgotten, and a forgotten `Unregister` is worse than no ledger at all — it reports objects
     * that are gone, so the number a decision rests on drifts upward for ever and nobody can tell that it
     * has. `ImageService::Unregister` is the existing proof: it is reached from four sites and `Clear()`
     * from one, and a fifth owner simply never gives its image back.
     *
     * So there is no `Unregister`. There is a move-only token, `ResourceOwnership`, that a resource holds
     * as a member: constructing it is the registration, destroying it is the removal, and a resource
     * cannot be alive without one or dead with one. It has no copy constructor, because two tokens for
     * one object would double-count and then double-remove.
     *
     * ── WHY THE OWNER IS CLAIMED AFTERWARDS AND "UNCLAIMED" IS AN ANSWER ──────────────────────────────
     *
     * The token is taken in the RESOURCE BASE CLASS's constructor, which is the only place that sees every
     * object of that kind whatever backend made it — and that constructor cannot possibly know who is
     * about to hold the pointer. `Image2D::Create` runs identically for a texture built from an asset, a
     * shadow cascade attachment and a panel's slice preview.
     *
     * Attribution therefore happens where the object is PUT somewhere: `Claim()`, called by the service,
     * the renderer or the cache that takes ownership of it. Anything nobody claims stays `Unclaimed`, and
     * that is deliberately a reportable state rather than a default bucket — §1.4 of the contract, applied
     * to a census: "nobody owns this" and "this owner has nothing" must not be the same number. The count
     * of unclaimed rows is the measurement device-loss recovery (Г7-A) has to plan against, and hiding it
     * behind a plausible default would make the ledger read complete while covering half the device.
     *
     * ── WHAT A ROW COSTS ─────────────────────────────────────────────────────────────────────────────
     *
     * 24 bytes and one mutex acquisition per GPU-object construction and destruction. GPU-object creation
     * already costs a driver round trip; this is not on any per-frame path, and the ledger is deliberately
     * NOT consulted while drawing — it answers questions between frames.
     */

    /// What KIND of device object a row stands for. A closed set with a name table below, for the reason
    /// `AssetTypeName` gives: a census that has to say WHICH kinds disagreed cannot print an integer.
    enum class ResourceKind : uint8_t
    {
        Image2D = 0,
        ImageCube,
        Image3D,
        VertexBuffer,
        IndexBuffer,
        UniformBuffer,
        StorageBuffer,
        GraphicsPipeline,
        ComputePipeline,
        Shader,
        Framebuffer,
        Material,

        // NOT a kind: the number of them. A new kind is added ABOVE this line and turns
        // Desert/Tests/Engine/AssetEviction red until it is named in ResourceKindName.
        Count,
    };

    /// Deliberately a switch with NO `default:` — see `Assets::AssetTypeName`, which this copies for the
    /// same reason: a `default:` swallows a new enumerator and puts a wrong name in the one message whose
    /// whole job is to be trusted.
    constexpr const char* ResourceKindName( const ResourceKind kind )
    {
        switch ( kind )
        {
            case ResourceKind::Image2D:
                return "Image2D";
            case ResourceKind::ImageCube:
                return "ImageCube";
            case ResourceKind::Image3D:
                return "Image3D";
            case ResourceKind::VertexBuffer:
                return "VertexBuffer";
            case ResourceKind::IndexBuffer:
                return "IndexBuffer";
            case ResourceKind::UniformBuffer:
                return "UniformBuffer";
            case ResourceKind::StorageBuffer:
                return "StorageBuffer";
            case ResourceKind::GraphicsPipeline:
                return "GraphicsPipeline";
            case ResourceKind::ComputePipeline:
                return "ComputePipeline";
            case ResourceKind::Shader:
                return "Shader";
            case ResourceKind::Framebuffer:
                return "Framebuffer";
            case ResourceKind::Material:
                return "Material";
            case ResourceKind::Count:
                return "Count";
        }
        return "Unknown";
    }

    /// WHO holds the object. The categories are LIFETIMES, not classes: what matters to both consumers —
    /// eviction and device-loss recovery — is who would have to rebuild the thing, not what C++ type it is.
    enum class ResourceOwner : uint8_t
    {
        /// Nobody has claimed it. The number this ledger exists to make visible; never a resting state
        /// for a resource somebody does own.
        Unclaimed = 0,

        /// A `Runtime::*Service` holds it, keyed on an asset handle. THE ONLY CATEGORY ASSET EVICTION MAY
        /// TOUCH: the asset's file is the recipe, so releasing it is recoverable by re-reading that file.
        AssetService,

        /// A `SceneRenderer` and the render systems inside it: framebuffers, LUTs, pipelines, the
        /// materials the passes own. Rebuilt only by rebuilding the renderer.
        SceneRenderer,

        /// The sky/IBL bake — panorama, radiance, irradiance, prefiltered. Recipe is the sky settings.
        Environment,

        /// The editor's own pictures: thumbnails, previews, panel slices, icon and font atlases.
        EditorTool,

        /// The 2D/UI backend and the ImGui layer.
        UserInterface,

        /// Swapchain, command pools, query pools, default and fallback textures — the device's own.
        Device,

        /// Built from no file at all: procedural meshes, runtime-generated images. There is no recipe on
        /// disk, so releasing one is DATA LOSS and eviction must never consider it.
        Procedural,

        Count,
    };

    constexpr const char* ResourceOwnerName( const ResourceOwner owner )
    {
        switch ( owner )
        {
            case ResourceOwner::Unclaimed:
                return "Unclaimed";
            case ResourceOwner::AssetService:
                return "AssetService";
            case ResourceOwner::SceneRenderer:
                return "SceneRenderer";
            case ResourceOwner::Environment:
                return "Environment";
            case ResourceOwner::EditorTool:
                return "EditorTool";
            case ResourceOwner::UserInterface:
                return "UserInterface";
            case ResourceOwner::Device:
                return "Device";
            case ResourceOwner::Procedural:
                return "Procedural";
            case ResourceOwner::Count:
                return "Count";
        }
        return "Unknown";
    }

    /**
     * @brief A LIVE ROW IN THE LEDGER, held BY the resource it describes. Move-only; destroying it is the
     *        removal.
     *
     * There is no `Register` and no `Unregister` anywhere in this header. `Take()` is the only way in and
     * `~ResourceOwnership` is the only way out, so the pair cannot be mismatched by anybody's discipline.
     *
     * A default-constructed token accounts for NOTHING and says so (`IsAccounted() == false`) rather than
     * pretending to be a row. That is what a resource constructed before the ledger's first use, or moved
     * out of, holds — and it is distinguishable from a live row, which is the whole of §1.4's rule.
     */
    class ResourceOwnership final
    {
    public:
        /// Accounts for nothing. Not a row; asking it anything says so.
        ResourceOwnership() = default;

        /// Open a row for a live object of @p kind. @p bytes is what the object costs on the device where
        /// the caller knows it and 0 where it does not — a COUNT is the number both consumers need, and a
        /// guessed size would be worse than an absent one.
        [[nodiscard]] static ResourceOwnership Take( ResourceKind kind, std::size_t bytes = 0 );

        ~ResourceOwnership();

        ResourceOwnership( ResourceOwnership&& other ) noexcept;
        ResourceOwnership& operator=( ResourceOwnership&& other ) noexcept;

        ResourceOwnership( const ResourceOwnership& )            = delete;
        ResourceOwnership& operator=( const ResourceOwnership& ) = delete;

        /// Is this token a row in the ledger?
        [[nodiscard]] bool IsAccounted() const noexcept
        {
            return m_Row != 0;
        }

        /// Say who holds the object, and — for an `AssetService` row — which asset's file is its recipe.
        /// Called by whoever takes ownership; see the header note on why this is not a constructor
        /// argument. Claiming an unaccounted token is a no-op, not a crash: a resource built before the
        /// first `Take()` is legal.
        void Claim( ResourceOwner owner, Common::AssetHandle asset = Common::AssetHandle{} );

        /// Correct the size once the object knows it (a Vulkan image learns its footprint only after the
        /// allocator answers). Silently ignored on an unaccounted token.
        void RecordBytes( std::size_t bytes );

        [[nodiscard]] ResourceOwner       GetOwner() const;
        [[nodiscard]] Common::AssetHandle GetAsset() const;

    private:
        explicit ResourceOwnership( uint64_t row ) noexcept : m_Row( row )
        {
        }

        /// The row's id, or 0 for "accounts for nothing". An id and not a pointer, because the ledger's
        /// storage is allowed to move and a token outliving a reallocation must not be holding an address
        /// into it.
        uint64_t m_Row = 0;
    };

    /**
     * @brief WHILE THIS OBJECT IS ALIVE, ANY ROW OPENED ON THIS THREAD BELONGS TO @p owner.
     *
     * The reason it exists is arithmetic. One `SceneRenderer` builds roughly a hundred and twenty device
     * objects across twenty render systems — framebuffers, LUTs, pipelines, the materials the passes own —
     * and every one of them is created by a different file. Claiming them one at a time means touching
     * twenty files to answer one question, and the twenty-first system, written next month, is attributed
     * by nobody and silently swells the "Unclaimed" figure that the whole ledger exists to make trustworthy.
     * One scope at the top of `EnsureRendererResources` attributes all of them, and a new system inside it
     * is attributed the day it is written.
     *
     * AN EXPLICIT `Claim()` ALWAYS WINS, because it happens later: a service that builds a texture inside
     * somebody's scope names the asset behind it afterwards, and that is the more specific truth. The scope
     * is a DEFAULT for rows nobody speaks for, not an override of the rows somebody does.
     *
     * Thread-local, and nested scopes restore the enclosing one — a bake inside a renderer's scope is still
     * the bake's.
     */
    class ResourceAttributionScope final
    {
    public:
        explicit ResourceAttributionScope( ResourceOwner owner ) noexcept;
        ~ResourceAttributionScope();

        ResourceAttributionScope( const ResourceAttributionScope& )            = delete;
        ResourceAttributionScope& operator=( const ResourceAttributionScope& ) = delete;
        ResourceAttributionScope( ResourceAttributionScope&& )                 = delete;
        ResourceAttributionScope& operator=( ResourceAttributionScope&& )      = delete;

    private:
        ResourceOwner m_Previous;
    };

    /// A whole-ledger answer, taken in ONE pass under ONE lock. A caller that asked for the total and then
    /// for the per-owner split in two calls would be handed two halves of two different instants, and the
    /// difference between them reads exactly like a leak.
    struct ResourceCensus
    {
        /// Every live row.
        uint32_t Live = 0;
        /// Rows nobody has claimed. THE number Г7-A plans against.
        uint32_t Unclaimed = 0;
        /// Rows with no asset behind them — everything asset eviction can never reach, claimed or not.
        uint32_t WithoutAsset = 0;
        /// Bytes, summed over the rows that reported a size. Rows that did not are counted in `Live` and
        /// contribute nothing here; `BytesKnownFor` says how many did, so a reader can tell a small total
        /// from an unmeasured one.
        uint64_t Bytes         = 0;
        uint32_t BytesKnownFor = 0;

        /// Live rows per kind and per owner, indexed by the enumerators above.
        uint32_t PerKind[static_cast<std::size_t>( ResourceKind::Count )]   = {};
        uint32_t PerOwner[static_cast<std::size_t>( ResourceOwner::Count )] = {};

        /// Live rows per (owner x kind) — the table that answers "who owns the objects nobody claimed".
        uint32_t PerOwnerKind[static_cast<std::size_t>( ResourceOwner::Count )]
                             [static_cast<std::size_t>( ResourceKind::Count )] = {};

        /// DEVICE BYTES per (owner x kind), and how many rows of each cell reported one.
        ///
        /// WHY COUNTING OBJECTS WAS NOT ENOUGH, AND IT IS A MEASUREMENT RATHER THAN A PREFERENCE.
        /// `Docs/World/PROGRAMME.md` §7 turns on one quantity — how much of a scene's device memory is
        /// TEXTURE. Until these two tables existed the ledger could say "AssetService holds one Image2D"
        /// and could not say that the Image2D costs 5 592 405 bytes, so the share had to be worked out
        /// by hand, outside the engine, from the texture's dimensions and a mip-tail fraction. An
        /// instrument built to observe memory could not observe the one number the step is decided on,
        /// and every answer about it was therefore somebody's arithmetic rather than a reading.
        ///
        /// EACH CELL CARRIES ITS OWN COVERAGE, for the reason the grand total already does: a cell with
        /// live rows and no known size sums to 0 bytes, and 0 reads as "this owner holds nothing on the
        /// device" rather than as "nobody measured these". Those are opposite diagnoses.
        uint64_t BytesPerOwnerKind[static_cast<std::size_t>( ResourceOwner::Count )]
                                  [static_cast<std::size_t>( ResourceKind::Count )] = {};
        uint32_t BytesKnownPerOwnerKind[static_cast<std::size_t>( ResourceOwner::Count )]
                                       [static_cast<std::size_t>( ResourceKind::Count )] = {};

        /// SUMMED FROM THE TABLE ON EVERY CALL, NEVER STORED BESIDE IT. A per-owner byte total kept as
        /// its own field is a second statement of a quantity the table already makes, and the two drift
        /// the day a row is added to one and not the other — this repository's most repeated defect
        /// shape. Summing twelve cells costs nothing at the rate this is read (between frames).
        [[nodiscard]] uint64_t BytesForOwner( const ResourceOwner owner ) const
        {
            uint64_t sum = 0;
            for ( std::size_t kind = 0; kind < static_cast<std::size_t>( ResourceKind::Count ); ++kind )
                sum += BytesPerOwnerKind[static_cast<std::size_t>( owner )][kind];
            return sum;
        }

        [[nodiscard]] uint32_t BytesKnownForOwner( const ResourceOwner owner ) const
        {
            uint32_t rows = 0;
            for ( std::size_t kind = 0; kind < static_cast<std::size_t>( ResourceKind::Count ); ++kind )
                rows += BytesKnownPerOwnerKind[static_cast<std::size_t>( owner )][kind];
            return rows;
        }

        /// The one cell the texture work asks for, and the pair of questions it answers together:
        /// `BytesFor( AssetService, Image2D )` is every byte of texture pixel data this process holds on
        /// the device, and `KnownFor` beside it says over how many of those images the figure is true.
        [[nodiscard]] uint64_t BytesFor( const ResourceOwner owner, const ResourceKind kind ) const
        {
            return BytesPerOwnerKind[static_cast<std::size_t>( owner )][static_cast<std::size_t>( kind )];
        }

        [[nodiscard]] uint32_t KnownFor( const ResourceOwner owner, const ResourceKind kind ) const
        {
            return BytesKnownPerOwnerKind[static_cast<std::size_t>( owner )][static_cast<std::size_t>( kind )];
        }
    };

    class ResourceLedger final
    {
    public:
        /// The census, at this instant.
        [[nodiscard]] static ResourceCensus Take();

        /// The census as lines a person reads, owners first and then the kinds under each. Written for the
        /// log and for the control channel; the numbers are the same object as `Take()`.
        [[nodiscard]] static std::string Report();

        // TWO MORE QUERIES WERE DRAFTED HERE AND ARE GONE: `AssetBackedHandles()`, which would have given
        // eviction its candidate set, and `RowsFor( handle )`. Neither ever had a caller — the sweep walks
        // the REGISTRY, which is the authority on what assets exist, and the ledger is the authority on
        // what is on the device; asking the ledger which assets to consider would have been the second
        // source of truth for a question that already has one. Written down rather than silently absent
        // because "the ledger cannot list an asset's rows" is a reasonable thing to expect of it.

    private:
        friend class ResourceOwnership;

        static uint64_t Open( ResourceKind kind, std::size_t bytes );
        static void     Close( uint64_t row );
        static void     Attribute( uint64_t row, ResourceOwner owner, Common::AssetHandle asset );
        static void     SetBytes( uint64_t row, std::size_t bytes );
        static bool     Read( uint64_t row, ResourceOwner& owner, Common::AssetHandle& asset );
    };

    // A NAMED detail namespace and INLINE functions, not an anonymous namespace. An anonymous namespace
    // in a header gives every translation unit its own copy of these statics — so the ledger would count
    // per-TU and every number it reports would be a fraction of the truth, silently. `inline` in a named
    // namespace is what gives the whole program one map.
    namespace LedgerDetail
    {
        struct LedgerRow
        {
            ResourceKind        Kind  = ResourceKind::Image2D;
            ResourceOwner       Owner = ResourceOwner::Unclaimed;
            Common::AssetHandle Asset;
            std::size_t         Bytes      = 0;
            bool                BytesKnown = false;
        };

        /// The rows, and the id that indexes them.
        ///
        /// A MAP AND NOT A VECTOR WITH A FREE LIST, and the reason is the defect this file's neighbour
        /// already has: `ImageService` indexes a vector by a recycled handle and resolves on the INDEX
        /// alone, so a stale handle silently resolves to whoever moved into the slot. A monotonically
        /// increasing id that is never reused cannot do that: a token from a released row finds nothing,
        /// which is the only safe answer. Ids are 64-bit, so exhausting them is not a scenario.
        ///
        /// Function-local statics rather than file-scope ones because GPU objects are constructed during
        /// static initialisation in some translation units (the default textures), and a file-scope map
        /// might not be alive yet. The `Meyers` form gives construction on first use in every order.
        inline std::mutex& Lock()
        {
            static std::mutex lock;
            return lock;
        }

        inline std::unordered_map<uint64_t, LedgerRow>& Rows()
        {
            static std::unordered_map<uint64_t, LedgerRow> rows;
            return rows;
        }

        inline uint64_t& NextRowId()
        {
            static uint64_t next = 1; // 0 is "accounts for nothing" and is never handed out
            return next;
        }

        /// The attribution a new row gets when nobody claims it. Thread-local: two threads building GPU
        /// objects at once must not attribute each other's, and the preloader does run staged work.
        inline ResourceOwner& AmbientOwner()
        {
            static thread_local ResourceOwner owner = ResourceOwner::Unclaimed;
            return owner;
        }
    } // namespace LedgerDetail

    inline ResourceAttributionScope::ResourceAttributionScope( const ResourceOwner owner ) noexcept
         : m_Previous( LedgerDetail::AmbientOwner() )
    {
        LedgerDetail::AmbientOwner() = owner;
    }

    inline ResourceAttributionScope::~ResourceAttributionScope()
    {
        LedgerDetail::AmbientOwner() = m_Previous;
    }

    // ────────────────────────────────────────────────────────────────────────────────────────────────
    // ResourceOwnership — the token
    // ────────────────────────────────────────────────────────────────────────────────────────────────

    inline ResourceOwnership ResourceOwnership::Take( const ResourceKind kind, const std::size_t bytes )
    {
        return ResourceOwnership( ResourceLedger::Open( kind, bytes ) );
    }

    inline ResourceOwnership::~ResourceOwnership()
    {
        if ( m_Row != 0 )
            ResourceLedger::Close( m_Row );
    }

    inline ResourceOwnership::ResourceOwnership( ResourceOwnership&& other ) noexcept : m_Row( other.m_Row )
    {
        other.m_Row = 0;
    }

    inline ResourceOwnership& ResourceOwnership::operator=( ResourceOwnership&& other ) noexcept
    {
        if ( this != &other )
        {
            // The row this token already held is closed FIRST. Overwriting it would leave a row with no
            // token — the exact "reports objects that are gone" drift the header refuses to allow.
            if ( m_Row != 0 )
                ResourceLedger::Close( m_Row );
            m_Row       = other.m_Row;
            other.m_Row = 0;
        }
        return *this;
    }

    inline void ResourceOwnership::Claim( const ResourceOwner owner, const Common::AssetHandle asset )
    {
        if ( m_Row != 0 )
            ResourceLedger::Attribute( m_Row, owner, asset );
    }

    inline void ResourceOwnership::RecordBytes( const std::size_t bytes )
    {
        if ( m_Row != 0 )
            ResourceLedger::SetBytes( m_Row, bytes );
    }

    inline ResourceOwner ResourceOwnership::GetOwner() const
    {
        ResourceOwner       owner = ResourceOwner::Unclaimed;
        Common::AssetHandle asset;
        if ( m_Row != 0 )
            (void)ResourceLedger::Read( m_Row, owner, asset );
        return owner;
    }

    inline Common::AssetHandle ResourceOwnership::GetAsset() const
    {
        ResourceOwner       owner = ResourceOwner::Unclaimed;
        Common::AssetHandle asset;
        if ( m_Row != 0 )
            (void)ResourceLedger::Read( m_Row, owner, asset );
        return asset;
    }

    // ────────────────────────────────────────────────────────────────────────────────────────────────
    // ResourceLedger — the storage
    // ────────────────────────────────────────────────────────────────────────────────────────────────

    inline uint64_t ResourceLedger::Open( const ResourceKind kind, const std::size_t bytes )
    {
        std::lock_guard<std::mutex> guard( LedgerDetail::Lock() );

        const uint64_t id = LedgerDetail::NextRowId()++;

        LedgerDetail::LedgerRow row;
        row.Kind = kind;
        // The ambient default, if a scope is open. Nothing else reads it: a later Claim() overwrites the
        // owner outright, because naming the asset behind an object is more specific than naming the
        // subsystem that happened to be building when it appeared.
        row.Owner      = LedgerDetail::AmbientOwner();
        row.Bytes      = bytes;
        row.BytesKnown = bytes != 0;
        LedgerDetail::Rows().emplace( id, row );

        return id;
    }

    inline void ResourceLedger::Close( const uint64_t row )
    {
        std::lock_guard<std::mutex> guard( LedgerDetail::Lock() );
        LedgerDetail::Rows().erase( row );
    }

    inline void ResourceLedger::Attribute( const uint64_t row, const ResourceOwner owner,
                                           const Common::AssetHandle asset )
    {
        std::lock_guard<std::mutex> guard( LedgerDetail::Lock() );
        if ( const auto it = LedgerDetail::Rows().find( row ); it != LedgerDetail::Rows().end() )
        {
            it->second.Owner = owner;
            it->second.Asset = asset;
        }
    }

    inline void ResourceLedger::SetBytes( const uint64_t row, const std::size_t bytes )
    {
        std::lock_guard<std::mutex> guard( LedgerDetail::Lock() );
        if ( const auto it = LedgerDetail::Rows().find( row ); it != LedgerDetail::Rows().end() )
        {
            it->second.Bytes      = bytes;
            it->second.BytesKnown = bytes != 0;
        }
    }

    inline bool ResourceLedger::Read( const uint64_t row, ResourceOwner& owner, Common::AssetHandle& asset )
    {
        std::lock_guard<std::mutex> guard( LedgerDetail::Lock() );
        const auto                  it = LedgerDetail::Rows().find( row );
        if ( it == LedgerDetail::Rows().end() )
            return false;
        owner = it->second.Owner;
        asset = it->second.Asset;
        return true;
    }

    inline ResourceCensus ResourceLedger::Take()
    {
        std::lock_guard<std::mutex> guard( LedgerDetail::Lock() );

        ResourceCensus census;
        for ( const auto& [id, row] : LedgerDetail::Rows() )
        {
            ++census.Live;
            ++census.PerKind[static_cast<std::size_t>( row.Kind )];
            ++census.PerOwner[static_cast<std::size_t>( row.Owner )];
            ++census.PerOwnerKind[static_cast<std::size_t>( row.Owner )][static_cast<std::size_t>( row.Kind )];

            if ( row.Owner == ResourceOwner::Unclaimed )
                ++census.Unclaimed;
            if ( static_cast<uint64_t>( row.Asset ) == 0 )
                ++census.WithoutAsset;
            if ( row.BytesKnown )
            {
                census.Bytes += row.Bytes;
                ++census.BytesKnownFor;
                // In the SAME pass as the counts above, deliberately. A second walk taken later would
                // attribute an instant that is not the one the totals describe, and the difference
                // between the two instants reads exactly like a leak — the reason this whole census is
                // one function rather than two queries.
                census.BytesPerOwnerKind[static_cast<std::size_t>( row.Owner )]
                                        [static_cast<std::size_t>( row.Kind )] += row.Bytes;
                ++census.BytesKnownPerOwnerKind[static_cast<std::size_t>( row.Owner )]
                                               [static_cast<std::size_t>( row.Kind )];
            }
        }
        return census;
    }

    inline std::string ResourceLedger::Report()
    {
        const ResourceCensus census = Take();

        std::string text;
        text += "live=" + std::to_string( census.Live );
        text += " unclaimed=" + std::to_string( census.Unclaimed );
        text += " without-asset=" + std::to_string( census.WithoutAsset );
        // Both numbers, always. "12 MiB" over 400 rows of which 9 reported a size is a different fact from
        // "12 MiB" over 400 rows that all did, and a reader who is not told cannot separate them.
        text += " bytes=" + std::to_string( census.Bytes ) + " (known for " +
                std::to_string( census.BytesKnownFor ) + " of " + std::to_string( census.Live ) + ")";

        for ( std::size_t owner = 0; owner < static_cast<std::size_t>( ResourceOwner::Count ); ++owner )
        {
            if ( census.PerOwner[owner] == 0 )
                continue;

            const auto asOwner = static_cast<ResourceOwner>( owner );

            text += "\n  ";
            text += ResourceOwnerName( asOwner );
            text += " = " + std::to_string( census.PerOwner[owner] );
            // BYTES AND THE ROWS THEY ARE TRUE OVER, on the owner line and on every kind under it. The
            // count alone said "AssetService holds one Image2D", which is the same sentence whether that
            // image is a 64x64 icon or a 2048x2048 albedo with its chain — a factor of a thousand the
            // reader could not see.
            text += " bytes=" + std::to_string( census.BytesForOwner( asOwner ) ) +
                    " known=" + std::to_string( census.BytesKnownForOwner( asOwner ) ) + ":";

            for ( std::size_t kind = 0; kind < static_cast<std::size_t>( ResourceKind::Count ); ++kind )
            {
                if ( census.PerOwnerKind[owner][kind] == 0 )
                    continue;
                text += ' ';
                text += ResourceKindName( static_cast<ResourceKind>( kind ) );
                text += '=' + std::to_string( census.PerOwnerKind[owner][kind] );
                text += " bytes=" + std::to_string( census.BytesPerOwnerKind[owner][kind] );
                text += " known=" + std::to_string( census.BytesKnownPerOwnerKind[owner][kind] );
            }
        }

        return text;
    }

} // namespace Desert::Graphic
