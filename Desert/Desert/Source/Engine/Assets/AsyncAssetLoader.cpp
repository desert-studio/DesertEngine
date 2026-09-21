#include "AsyncAssetLoader.hpp"

#include <Common/Core/JobSystem.hpp>
#include <Common/Core/Logger.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace Desert::Assets
{
    namespace
    {
        /// One request, from `Request()` to the delegate call that ends it.
        ///
        /// HELD BY `shared_ptr` AND NOT BY VALUE IN THE MAP, because a worker needs to keep writing into
        /// it after `Release()` has taken it out of the live set: releasing must not make a running read
        /// write through a dangling reference, and it must not make the read *wait* either.
        struct Record
        {
            uint64_t         Id = 0;
            AssetHandle      Handle{ 0 };
            Asset<AssetBase> Payload; ///< THE KEEP-ALIVE. The request owning this record is why the asset
                                      ///< stays in memory; there is no second reference count to forget.
            AsyncAssetLoader::OnReady  Ready;
            AsyncAssetLoader::OnCancel Cancel;

            bool        CancelRequested = false;
            bool        Settled         = false;
            LoadOutcome Outcome         = LoadOutcome::Failed;
            std::string Error;
        };

        struct LoaderState
        {
            mutable std::mutex                                    Lock;
            std::unordered_map<uint64_t, std::shared_ptr<Record>> Live;

            /// Ids whose read has landed and whose completion delegate is owed a `Pump()`.
            std::vector<uint64_t> Done;
            /// Ids whose `Cancel()` is owed a `Pump()`. Kept apart from `Done` so that a cancel arriving
            /// after a completion still wins — see `Pump()`.
            std::vector<uint64_t> Cancelled;

            /// WHICH HANDLES HAVE A WORKER ON THEM, and who is waiting for it.
            ///
            /// Without this, two requests for one asset submit two jobs that both call `Load()` on the
            /// same object at the same time — a data race inside every `LoadFromFile` in the engine, none
            /// of which was written to be reentrant. The second and later requests for a handle get no
            /// job at all; they settle together with the first when it lands, with its outcome. That is
            /// also the honest answer: they asked for the same file and the same read served them.
            std::unordered_map<AssetHandle, std::vector<uint64_t>> Waiting;

            uint64_t              NextId = 1;
            std::atomic<uint64_t> Started{ 0 };
            std::atomic<uint64_t> Cancels{ 0 };
            /// Workers currently inside `AssetBase::Load()`. `ShutdownAndDrain` waits on it, so tearing
            /// the process down cannot free an asset a worker is still reading into.
            std::atomic<int> InFlight{ 0 };
        };

        LoaderState& State()
        {
            static LoaderState state;
            return state;
        }

        /// Settle @p handle's whole waiting list with one outcome, under the caller's lock.
        void SettleWaitingLocked( LoaderState& state, const AssetHandle& handle, const LoadOutcome outcome,
                                  const std::string& error )
        {
            const auto waiting = state.Waiting.find( handle );
            if ( waiting == state.Waiting.end() )
                return;

            for ( const uint64_t id : waiting->second )
            {
                const auto live = state.Live.find( id );
                if ( live == state.Live.end() )
                    continue; // released while the read was running; nothing is owed to nobody

                live->second->Settled = true;
                live->second->Outcome = outcome;
                live->second->Error   = error;
                state.Done.push_back( id );
            }
            state.Waiting.erase( waiting );
        }
    } // namespace

    LoadRequest::~LoadRequest()
    {
        Release();
    }

    LoadRequest::LoadRequest( LoadRequest&& other ) noexcept : m_Id( other.m_Id ), m_Handle( other.m_Handle )
    {
        other.m_Id = 0;
    }

    LoadRequest& LoadRequest::operator=( LoadRequest&& other ) noexcept
    {
        if ( this == &other )
            return *this;

        // THE OLD REQUEST IS RELEASED, NOT CANCELLED. Overwriting a handle is the caller saying "this
        // slot is about the new thing now"; it is not the caller asking to be told about the old one.
        Release();
        m_Id       = other.m_Id;
        m_Handle   = other.m_Handle;
        other.m_Id = 0;
        return *this;
    }

    bool LoadRequest::IsValid() const noexcept
    {
        return m_Id != 0 && AsyncAssetLoader::Get().IsLive( m_Id );
    }

    void LoadRequest::Cancel()
    {
        if ( m_Id == 0 )
            return;
        AsyncAssetLoader::Get().CancelById( m_Id );
        // ZEROED HERE, so the destructor that follows cannot turn this cancel into a release and swallow
        // the delegate the caller just asked for.
        m_Id = 0;
    }

    void LoadRequest::Release()
    {
        if ( m_Id == 0 )
            return;
        AsyncAssetLoader::Get().ReleaseById( m_Id );
        m_Id = 0;
    }

    AsyncAssetLoader& AsyncAssetLoader::Get()
    {
        static AsyncAssetLoader loader;
        return loader;
    }

    LoadRequest AsyncAssetLoader::Request( const Asset<AssetBase>& asset, OnReady onReady, OnCancel onCancel )
    {
        // THREE REFUSALS, ALL LOGGED, NONE OF THEM SILENT. A request with no completion delegate loads a
        // file nobody will hear about; a request with no cancel delegate is T2.3's named wedge; a request
        // with no asset has nothing to read. Each returns an invalid handle, which the caller can see.
        if ( !asset )
        {
            LOG_ERROR( "[AsyncLoad] a request was made with no asset; nothing was queued." );
            return {};
        }
        if ( !onReady )
        {
            LOG_ERROR( "[AsyncLoad] '{}' was requested with no completion delegate; nothing was queued.",
                       asset->GetMetadata().Filepath.string() );
            return {};
        }
        if ( !onCancel )
        {
            LOG_ERROR( "[AsyncLoad] '{}' was requested with no CANCEL delegate. Binding only a completion "
                       "is the wedge GAP_ANALYSIS T2.3 makes the cancel delegate mandatory to prevent: a "
                       "cancelled request would leave the caller waiting for a call that never comes. "
                       "Nothing was queued.",
                       asset->GetMetadata().Filepath.string() );
            return {};
        }

        LoaderState& state = State();

        auto record     = std::make_shared<Record>();
        record->Handle  = asset->GetMetadata().Handle;
        record->Payload = asset;
        record->Ready   = std::move( onReady );
        record->Cancel  = std::move( onCancel );

        bool submit = false;
        {
            const std::lock_guard<std::mutex> guard( state.Lock );
            record->Id             = state.NextId++;
            state.Live[record->Id] = record;

            if ( asset->IsReadyForUse() )
            {
                // ALREADY RESIDENT, AND STILL DEFERRED. See the header: the one case that could have been
                // answered inside this call is exactly the case that would let a delegate run before the
                // caller has stored the handle this function is about to return.
                record->Settled = true;
                record->Outcome = LoadOutcome::Loaded;
                state.Done.push_back( record->Id );
            }
            else if ( const auto waiting = state.Waiting.find( record->Handle ); waiting != state.Waiting.end() )
            {
                waiting->second.push_back( record->Id );
            }
            else
            {
                state.Waiting[record->Handle] = { record->Id };
                submit                        = true;
            }
        }

        if ( submit )
        {
            state.Started.fetch_add( 1, std::memory_order_relaxed );
            const AssetHandle handle = record->Handle;
            Common::JobSystem::Get().Submit(
                 [record, handle]
                 {
                     LoaderState& inner = State();
                     inner.InFlight.fetch_add( 1, std::memory_order_relaxed );

                     LoadOutcome outcome = LoadOutcome::Loaded;
                     std::string error;

                     bool skip = false;
                     {
                         const std::lock_guard<std::mutex> guard( inner.Lock );
                         // CANCELLED BEFORE A WORKER PICKED IT UP — so the read never happens at all.
                         // This is the half of `Cancel()` that actually saves work, and it is why a scene
                         // that was closed while loading does not sit through its own content.
                         skip = record->CancelRequested;
                     }

                     if ( !skip )
                     {
                         // `Load()` AND NOT `EnsureLoaded()`, because `EnsureLoaded` needs the
                         // `AssetManager` to resolve dependencies and the manager is not thread-safe.
                         // The only asset type in this tree that resolves anything is `CloudTypeAsset`,
                         // which stays eagerly loaded; a future type that needs resolving does it in its
                         // completion delegate, back on the main thread.
                         const AsyncLoadMarker marker;
                         if ( const auto loaded = record->Payload->Load(); !loaded )
                         {
                             outcome = LoadOutcome::Failed;
                             error   = loaded.GetError();
                         }
                     }

                     {
                         const std::lock_guard<std::mutex> guard( inner.Lock );
                         SettleWaitingLocked( inner, handle, outcome, error );
                     }

                     inner.InFlight.fetch_sub( 1, std::memory_order_relaxed );
                 } );
        }

        return LoadRequest( record->Id, record->Handle );
    }

    void AsyncAssetLoader::Pump()
    {
        LoaderState& state = State();

        std::vector<std::shared_ptr<Record>> cancelled;
        std::vector<std::shared_ptr<Record>> completed;
        {
            const std::lock_guard<std::mutex> guard( state.Lock );

            // CANCELS FIRST, AND THAT ORDER IS THE CONTRACT. A request whose read landed in the same
            // tick it was cancelled has an id in BOTH lists; taking it out of the live set here is what
            // makes the completion loop below skip it. One request, one delegate, chosen by the caller.
            for ( const uint64_t id : state.Cancelled )
            {
                const auto live = state.Live.find( id );
                if ( live == state.Live.end() )
                    continue;
                cancelled.push_back( live->second );
                state.Live.erase( live );
            }
            state.Cancelled.clear();

            for ( const uint64_t id : state.Done )
            {
                const auto live = state.Live.find( id );
                if ( live == state.Live.end() )
                    continue;
                completed.push_back( live->second );
                state.Live.erase( live );
            }
            state.Done.clear();
        }

        // OUTSIDE THE LOCK. A delegate is allowed to issue the next request — a cloud type's completion
        // asking for the volume it names is the shape this whole tier is built for — and doing that under
        // our own mutex would deadlock on the first one.
        for ( const auto& record : cancelled )
            record->Cancel();

        for ( const auto& record : completed )
            record->Ready( record->Payload, record->Outcome, record->Error );
    }

    size_t AsyncAssetLoader::Outstanding() const
    {
        const LoaderState&                state = State();
        const std::lock_guard<std::mutex> guard( state.Lock );
        return state.Live.size();
    }

    uint64_t AsyncAssetLoader::StartedCount() const
    {
        return State().Started.load( std::memory_order_relaxed );
    }

    uint64_t AsyncAssetLoader::CancelledCount() const
    {
        return State().Cancels.load( std::memory_order_relaxed );
    }

    bool AsyncAssetLoader::IsRequested( const AssetHandle& handle ) const
    {
        const LoaderState&                state = State();
        const std::lock_guard<std::mutex> guard( state.Lock );
        for ( const auto& [id, record] : state.Live )
        {
            if ( record->Handle == handle )
                return true;
        }
        return false;
    }

    void AsyncAssetLoader::CancelById( const uint64_t id )
    {
        LoaderState&                      state = State();
        const std::lock_guard<std::mutex> guard( state.Lock );

        const auto live = state.Live.find( id );
        if ( live == state.Live.end() || live->second->CancelRequested )
            return;

        live->second->CancelRequested = true;
        state.Cancelled.push_back( id );
        state.Cancels.fetch_add( 1, std::memory_order_relaxed );
    }

    void AsyncAssetLoader::ReleaseById( const uint64_t id )
    {
        LoaderState&                      state = State();
        const std::lock_guard<std::mutex> guard( state.Lock );
        // The worker, if there is one, keeps its own `shared_ptr<Record>` and finishes the read it
        // started; `SettleWaitingLocked` will find no live entry for this id and owe nothing.
        state.Live.erase( id );
    }

    bool AsyncAssetLoader::IsLive( const uint64_t id ) const
    {
        const LoaderState&                state = State();
        const std::lock_guard<std::mutex> guard( state.Lock );
        return state.Live.find( id ) != state.Live.end();
    }

    void AsyncAssetLoader::ShutdownAndDrain()
    {
        LoaderState& state = State();
        {
            const std::lock_guard<std::mutex> guard( state.Lock );
            state.Live.clear();
            state.Done.clear();
            state.Cancelled.clear();
            state.Waiting.clear();
        }

        // SPIN RATHER THAN CONDITION-VARIABLE, and it is a deliberate trade for a path that runs twice in
        // a process. A worker inside a read holds the asset alive through its own `shared_ptr`, so the
        // only thing this wait buys is that nobody is inside `LoadFromFile` when the allocator goes away.
        while ( state.InFlight.load( std::memory_order_relaxed ) > 0 )
            std::this_thread::yield();
    }

    void AsyncAssetLoader::ResetForTest()
    {
        ShutdownAndDrain();
        LoaderState&                      state = State();
        const std::lock_guard<std::mutex> guard( state.Lock );
        state.NextId = 1;
        state.Started.store( 0, std::memory_order_relaxed );
        state.Cancels.store( 0, std::memory_order_relaxed );
    }
} // namespace Desert::Assets
