#include "ThumbnailService.hpp"

#include <Editor/Widgets/CloudThumbnail.hpp>
#include <Engine/Core/RendererSlotBudget.hpp>
#include <Editor/Widgets/ThumbnailCache.hpp>
#include <Editor/Widgets/ThumbnailFreshness.hpp>
#include <Editor/Widgets/ThumbnailKey.hpp>

#include <Engine/Core/EngineContext.hpp>
#include <Engine/Graphic/SceneRenderer.hpp>

#include <Common/Core/JobSystem.hpp>
#include <Common/Core/Logger.hpp>

#include <chrono>
#include <filesystem>

namespace Desert::Editor
{
    namespace
    {
        // A capture takes two frames (render, then read back). If a request has not produced its PNG well
        // past that, something is wrong with the asset — a .demat that fails to parse, a mesh the service
        // never registers — and retrying it forever would keep the queue permanently busy. Generous enough
        // that a slow first-time shader compile is not mistaken for a failure.
        constexpr int kInFlightGiveUpTicks = 240;

        // How long the queue must stay empty before the renderer — and with it one of the six renderer
        // slots — is given back. ~5 s at 60 fps.
        //
        // The two costs it sits between are not symmetric, which is why it is generous rather than eager.
        // Re-creating the renderer is a device-idle wait plus a full framebuffer set, and requests arrive in
        // bursts as an asset grid scrolls, so a short delay would tear the renderer down and build it again
        // between two rows of the same folder — a visible hitch, repeatedly, to reclaim a slot nobody wanted
        // in that second. Holding it costs nothing at all until a SIXTH surface is opened, and the surfaces
        // that compete for slots (a scene view, the material editor, the photogrammetry preview) are opened
        // by hand, seconds apart. Five seconds is past any scroll burst and far inside any two deliberate
        // clicks.
        constexpr int kIdleTicksBeforeRelease = 300;

    } // namespace

    ThumbnailService& ThumbnailService::Get()
    {
        static ThumbnailService s_Instance;
        return s_Instance;
    }

    bool ThumbnailService::ShouldQueue( const std::string& identity, const std::string& png,
                                        const std::string& source )
    {
        if ( identity.empty() )
            return false;
        if ( m_Failed.count( identity ) || m_Queued.count( identity ) )
            return false;

        return NeedsCapture( png, source );
    }

    bool ThumbnailService::NeedsCapture( const std::string& png, const std::string& source )
    {
        // THROUGH THE SHARED RULE, and this is the whole of M8's defect. This used to be
        // `exists(png) -> nothing to do`, with a comment saying staleness was "the caller's call via
        // Invalidate()". The callers do make that comparison — and they make a DIFFERENT one: a PNG whose
        // asset is more than three seconds newer is unusable to every reader and already-cached to this
        // gate. Nothing draws it and nothing replaces it, for the rest of the project's life.
        //
        // Measured on this tree before the change: `touch`ing one .demat whose 961 KB PNG was on disk left
        // the Details slot showing a flat colour swatch, across a restart, with the PNG's modification time
        // unchanged and no capture ever logged. Invalidate() cannot rescue it either — it clears the two
        // process-local sets and the file it would have to look past is still there.
        return ThumbnailFreshness::Judge( ThumbnailFreshness::Observe( png, source ) ) ==
               ThumbnailFreshness::Verdict::Capture;
    }

    std::string ThumbnailService::RequestMaterial( const Assets::AssetHandle& material,
                                                   const std::string& assetPath, ThumbnailSubject::Preview how )
    {
        // The deduplication sets are keyed on the asset's IDENTITY, for the same reason the PNG is
        // (ThumbnailKey): panels do not agree on how to spell a path, and a set that remembered spellings
        // would let one panel's Invalidate leave another panel's entry standing — which is a queue slot
        // that never drains and a thumbnail that never refreshes.
        const std::string identity = ThumbnailKey::Identity( assetPath );
        const std::string png      = ThumbnailKey::DiskPath( assetPath );
        if ( ShouldQueue( identity, png, assetPath ) )
        {
            m_Queue.push_back( { Kind::Material, material, Assets::AssetHandle( static_cast<uint64_t>( 0 ) ),
                                 identity, assetPath, png, how } );
            m_Queued.insert( identity );
        }
        return png;
    }

    std::string ThumbnailService::RequestMesh( const Assets::AssetHandle& mesh, const std::string& assetPath,
                                               const Assets::AssetHandle& material )
    {
        const std::string identity = ThumbnailKey::Identity( assetPath );
        const std::string png      = ThumbnailKey::DiskPath( assetPath );
        if ( ShouldQueue( identity, png, assetPath ) )
        {
            m_Queue.push_back(
                 { Kind::Mesh, mesh, material, identity, assetPath, png, ThumbnailSubject::Preview::Sphere } );
            m_Queued.insert( identity );
        }
        return png;
    }

    std::string ThumbnailService::RequestPainted( const std::string& assetPath )
    {
        // THE SAME GATE AS THE OTHER TWO ENTRY POINTS. A painted picture is cheaper to make, which is a
        // reason to schedule it more freely and NOT a reason to re-make one that is already on disk: the
        // freshness rule is about whether the file is a picture OF the asset, and that question does not
        // depend on who drew it.
        const std::string identity = ThumbnailKey::Identity( assetPath );
        const std::string png      = ThumbnailKey::DiskPath( assetPath );
        if ( ShouldQueue( identity, png, assetPath ) )
        {
            m_PaintQueue.push_back( { identity, assetPath, png } );
            m_Queued.insert( identity );
        }
        return png;
    }

    void ThumbnailService::Invalidate( const std::string& assetPath )
    {
        // Through the same identity the Request* entry points inserted under, so a caller holding any
        // spelling of the asset clears the right entries.
        const std::string identity = ThumbnailKey::Identity( assetPath );
        m_Failed.erase( identity );
        m_Queued.erase( identity );
    }

    void ThumbnailService::Shutdown()
    {
        // THE PAINT QUEUE IS DRAINED FIRST AND UNCONDITIONALLY, and the "unconditionally" is the part
        // worth writing down: this function used to open with `if ( !m_Renderer ) return;`, on the sound
        // reasoning that a session which previewed nothing had nothing to release. That stopped being
        // true the moment a second kind of work existed which needs no renderer — a session that painted
        // four hundred cloud thumbnails and never photographed a mesh has no renderer AND a job in
        // flight, and the early return would have walked straight past it.
        //
        // A running paint holds nothing of this object (the job captures its three strings by value), so
        // waiting is about ORDER rather than safety: the future's destructor from `Async` does not block,
        // and a JobSystem shutdown that ran while a paint was mid-`stbi_write_png` would leave a `.part`
        // file behind. Waiting here costs the milliseconds one fill takes.
        m_PaintQueue.clear();
        if ( m_PaintInFlight.valid() )
        {
            m_PaintInFlight.wait();
            m_PaintInFlight = {};
            m_PaintInFlightIdentity.clear();
            m_PaintInFlightSource.clear();
        }
        m_Painted = 0;

        // See the header for why the rest of this exists at all. Nothing to say when the renderer was
        // never built — that session pays nothing, here as everywhere else in this file.
        if ( !m_Renderer )
            return;

        // The queue goes first, so the state left behind is one a Tick() could legitimately act on rather
        // than a half-cancelled capture: an in-flight PNG that no longer has a renderer behind it would be
        // reported as "never completed" by the give-up path if the service were ever ticked again.
        m_Queue.clear();
        m_Queued.clear();
        m_InFlight.clear();
        m_InFlightPng.clear();
        m_InFlightPngBefore.reset();
        m_InFlightTicks = 0;
        m_Captured      = 0;
        m_Skipped       = 0;
        m_SlotRefused   = false;

        // ~AssetThumbnailRenderer idles the device and releases the scene before the renderer, which is what
        // returns the slot. Identical to the idle path above — the DIFFERENCE is only that this one is not
        // waiting for 300 frames that a quitting editor will never draw.
        m_Renderer.reset();
        m_IdleTicks = 0;

        LOG_INFO( "[Thumbnails] renderer released on shutdown ({}/{} renderer slots in use).",
                  Graphic::SceneRenderer::GetLiveRendererCount(), EngineContext::kMaxRendererSlots );
    }

    bool ThumbnailService::AcquireRenderer()
    {
        // THE ONE PLACE THIS SERVICE MAY TAKE A SLOT, so the condition cannot be true in one caller and
        // forgotten in the next — and so the refusal has exactly one voice.
        //
        // Background, and that is the whole entitlement: a capture is work nobody asked for by name, and
        // the picture it makes is what the Details row shows precisely when the live preview could not be
        // had. Taking the last slot would starve the surface the person is about to open AND would be
        // taking it to produce the consolation prize for not having it (Engine/Core/RendererSlotBudget.hpp).
        const uint32_t live = Graphic::SceneRenderer::GetLiveRendererCount();
        if ( !Engine::RendererSlotBudget::MayClaim( Engine::RendererSlotBudget::Demand::Background, live,
                                                    EngineContext::kMaxRendererSlots ) )
        {
            // IT SAYS SO. A queue that quietly stops draining is indistinguishable from a queue that has
            // nothing in it, and "nothing to do" is the reading a person will reach for — the same empty
            // successful answer the contract forbids. Once per stretch of scarcity, because the state ends
            // when a window is closed and a line per frame would bury the log it belongs in.
            if ( !m_SlotRefused )
            {
                m_SlotRefused = true;
                LOG_WARN( "[Thumbnails] {} of {} renderer slots are in use — {} preview(s) are waiting "
                          "rather than taking the last one. Close a scene view, a material window or an "
                          "asset preview and they will render.",
                          live, EngineContext::kMaxRendererSlots, m_Queue.size() );
            }
            return false;
        }

        if ( m_SlotRefused )
        {
            m_SlotRefused = false;
            LOG_INFO( "[Thumbnails] a renderer slot came free ({} of {} in use) — {} waiting preview(s) "
                      "will now render.",
                      live, EngineContext::kMaxRendererSlots, m_Queue.size() );
        }
        m_Renderer = std::make_unique<AssetThumbnailRenderer>();
        return true;
    }

    void ThumbnailService::TickPainted()
    {
        // Collect first, dispatch second, so a queue of one asset finishes in two frames rather than
        // three — and so the freshness re-check below sees the picture the previous paint just wrote.
        if ( m_PaintInFlight.valid() &&
             m_PaintInFlight.wait_for( std::chrono::seconds( 0 ) ) == std::future_status::ready )
        {
            const Common::BoolResultStr result = m_PaintInFlight.get();
            m_PaintInFlight                    = {};

            if ( result )
            {
                ++m_Painted;
            }
            else
            {
                // NAMED, AND NOT RETRIED. The reasons a paint fails are all about the FILE — a container
                // that will not decode, a layout with no pattern, a type whose profile is all zeros — and
                // none of them is fixed by asking again next frame. Same per-process memory as the
                // capture path, and for the same reason: a wipe of the cache is what clears it.
                LOG_WARN( "[Thumbnails] '{}' could not be painted: {} — not retrying.", m_PaintInFlightSource,
                          result.GetError() );
                m_Failed.insert( m_PaintInFlightIdentity );
            }
            m_Queued.erase( m_PaintInFlightIdentity );
            m_PaintInFlightIdentity.clear();
            m_PaintInFlightSource.clear();
        }

        if ( m_PaintInFlight.valid() || m_PaintQueue.empty() )
            return;

        // Drop anything the queue no longer owes, exactly as the capture path does and for the same
        // reason: two panels can name one asset, and a request can sit here while the other one's paint
        // lands.
        while ( !m_PaintQueue.empty() && !NeedsCapture( m_PaintQueue.front().Png, m_PaintQueue.front().Source ) )
        {
            m_Queued.erase( m_PaintQueue.front().Identity );
            m_PaintQueue.erase( m_PaintQueue.begin() );
            ++m_Skipped;
        }
        if ( m_PaintQueue.empty() )
            return;

        const PaintRequest req = m_PaintQueue.front();
        m_PaintQueue.erase( m_PaintQueue.begin() );

        m_PaintInFlightIdentity = req.Identity;
        m_PaintInFlightSource   = req.Source;

        // THROUGH THE JobSystem, not std::async and not a std::thread. Г9 removed exactly that from the
        // cloud bake: work outside the pool is invisible to the profiler, unbounded in thread count, and
        // obeys no shared budget. The lambda captures its two strings BY VALUE, so nothing it touches can
        // outlive or be outlived by this object.
        m_PaintInFlight = Common::JobSystem::Get().Async( [source = req.Source, png = req.Png]
                                                          { return CloudThumbnail::Write( source, png ); } );
    }

    void ThumbnailService::Tick()
    {
        // The slot-free half runs FIRST and unconditionally: every early return below is a statement
        // about a renderer, and a paint has no renderer to be blocked by. Putting it after them is how a
        // cloud thumbnail would come to depend on whether a mesh was being photographed.
        TickPainted();

        // WHAT THIS RUN DID, ONCE, ON THE FRAME EVERYTHING IS DONE — and asked of BOTH queues, which is
        // why it no longer lives inside the renderer's idle branch below. A session that only ever
        // painted cloud thumbnails has no renderer, so that branch is never reached, and the run would
        // have finished in silence: the same "the editor has stopped bothering" reading M8 was reported
        // as. All three numbers are needed to read the line — "8 captured, 12 painted, 0 already fresh"
        // is a cold cache doing its job, and "0, 0, 8" is the cache doing its job.
        if ( !HasWork() && ( m_Captured || m_Painted || m_Skipped ) )
        {
            LOG_INFO( "[Thumbnails] queue drained: {} captured, {} painted, {} already fresh on disk.", m_Captured,
                      m_Painted, m_Skipped );
            m_Captured = 0;
            m_Painted  = 0;
            m_Skipped  = 0;
        }

        // Nothing to preview this session -> never pay for the renderer (it owns a full SceneRenderer).
        if ( m_Queue.empty() && !m_Renderer )
            return;

        // Idle for long enough: hand the renderer slot back. Counted here rather than at the point the last
        // capture finished, because "the queue drained" and "no panel has asked for anything since" are
        // different facts and only the second one means the service is done.
        if ( m_Renderer && m_Queue.empty() && m_InFlight.empty() && !m_Renderer->HasPending() )
        {
            // The run's own report used to be here, and it moved to the top of this function when the
            // paint queue arrived — see the note there. What is left in this branch is only the thing
            // that IS about the renderer: giving its slot back.
            if ( ++m_IdleTicks >= kIdleTicksBeforeRelease )
            {
                // ~AssetThumbnailRenderer idles the device and releases the scene before the renderer, which
                // is what returns the slot.
                m_Renderer.reset();
                m_IdleTicks = 0;
                LOG_INFO( "[Thumbnails] idle for {} frames — renderer released ({}/{} renderer slots in use).",
                          kIdleTicksBeforeRelease, Graphic::SceneRenderer::GetLiveRendererCount(),
                          EngineContext::kMaxRendererSlots );
            }
            return;
        }
        m_IdleTicks = 0;

        if ( !m_Renderer && !AcquireRenderer() )
            return; // the queue is kept, not dropped: a slot freed later drains it

        m_Renderer->Tick();

        // Resolve the capture that was in flight.
        if ( !m_InFlight.empty() )
        {
            if ( !m_Renderer->HasPending() )
            {
                // THE FILE MUST HAVE MOVED, not merely be present. "Does the PNG exist?" was a sound test
                // only while a capture was never dispatched against an existing file; now that a STALE
                // thumbnail is re-captured (ShouldQueue), the old picture is sitting at that exact path
                // before the renderer starts, and an existence check would certify a capture that wrote
                // nothing at all — the failure would then be invisible AND the stale picture would be
                // queued again on the next Request, every frame, forever.
                const std::optional<std::filesystem::file_time_type> after = PngStamp( m_InFlightPng );
                if ( !after || after == m_InFlightPngBefore )
                {
                    // The renderer finished but produced nothing — the asset cannot be previewed. Remember
                    // it, or every frame from now on would re-queue the same doomed request.
                    LOG_WARN( "[Thumbnails] no preview produced for '{}' — not retrying (the file at '{}' "
                              "was {} by the capture)",
                              m_InFlight, m_InFlightPng, after ? "left unchanged" : "not written" );
                    m_Failed.insert( m_InFlight );
                }
                else
                {
                    ++m_Captured;
                }
                m_Queued.erase( m_InFlight );
                m_InFlight.clear();
                m_InFlightPng.clear();
                m_InFlightPngBefore.reset();
                m_InFlightTicks = 0;
            }
            else if ( ++m_InFlightTicks > kInFlightGiveUpTicks )
            {
                LOG_WARN( "[Thumbnails] '{}' never completed — giving up so the queue can drain",
                          m_InFlight );
                m_Failed.insert( m_InFlight );
                m_Queued.erase( m_InFlight );
                m_InFlight.clear();
                m_InFlightPng.clear();
                m_InFlightPngBefore.reset();
                m_InFlightTicks = 0;
            }
            return; // one capture at a time — the renderer has a single slot
        }

        if ( m_Renderer->HasPending() )
            return;

        // Drain anything the queue no longer owes. A request can sit here for seconds — the drain rate
        // measured on this machine is one capture per ~2 s — and in that time the same asset may have been
        // captured through another entry (two panels showing one material), or the panel that asked may
        // have called Invalidate() and asked again, leaving a duplicate behind it. Dispatching those would
        // re-render a picture that is already correct, at full cost, one after another.
        while ( !m_Queue.empty() && !NeedsCapture( m_Queue.front().Png, m_Queue.front().Source ) )
        {
            m_Queued.erase( m_Queue.front().Identity );
            m_Queue.erase( m_Queue.begin() );
            ++m_Skipped;
        }
        if ( m_Queue.empty() )
            return;

        const Request req = m_Queue.front();
        m_Queue.erase( m_Queue.begin() );

        // THE DISPATCH ANSWERS NOW. A refused request used to be indistinguishable from an accepted one:
        // both returned void, so the service marked the asset in-flight and then waited out
        // kInFlightGiveUpTicks (240 frames, four seconds of a blocked queue) before deciding it had
        // "never completed" — with no idea why. The renderer knows why at the moment it says no, and the
        // most common reason is one no amount of waiting fixes: a mesh whose geometry is not built, whose
        // capture would have written a photograph of empty sky and called it the asset.
        const auto queued = req.Type == Kind::Material
                                 ? m_Renderer->RequestMaterial( req.Handle, req.Png, req.How )
                                 : m_Renderer->RequestMesh( req.Handle, req.Png, req.Material );
        if ( !queued.IsSuccess() )
        {
            // A REFUSAL IS PERMANENT ONLY IF IT IS ABOUT THE ASSET. The renderer says no for two kinds of
            // reason: the asset cannot be photographed (no handle, no geometry) — which no amount of
            // waiting fixes — and "a capture is already in flight", which is a fact about the RENDERER and
            // is over in a few frames. The dispatch above cannot reach the second (it returns early on
            // HasPending), so the check below can only be true today; it is written anyway, because
            // remembering a transient failure for the rest of the process is precisely the shape M8 chose
            // the per-process failure set to avoid, and a future caller of this function would have no way
            // to know it was relying on an ordering three screens up.
            if ( m_Renderer->HasPending() )
            {
                LOG_WARN( "[Thumbnails] '{}' could not be dispatched yet: {} — kept in the queue.", req.Identity,
                          queued.GetError() );
                m_Queue.insert( m_Queue.begin(), req );
                return;
            }

            LOG_WARN( "[Thumbnails] '{}' was refused by the renderer: {} — not retrying.", req.Identity,
                      queued.GetError() );
            m_Failed.insert( req.Identity );
            m_Queued.erase( req.Identity );
            return;
        }

        m_InFlight          = req.Identity;
        m_InFlightPng       = req.Png;
        m_InFlightPngBefore = PngStamp( req.Png );
        m_InFlightTicks     = 0;
    }

    std::optional<std::filesystem::file_time_type> ThumbnailService::PngStamp( const std::string& png )
    {
        std::error_code ec;
        const auto      stamp = std::filesystem::last_write_time( png, ec );
        if ( ec )
            return std::nullopt;
        return stamp;
    }
} // namespace Desert::Editor
