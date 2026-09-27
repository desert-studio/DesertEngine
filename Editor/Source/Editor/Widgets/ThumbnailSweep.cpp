#include "ThumbnailSweep.hpp"

#include <Editor/Widgets/ThumbnailService.hpp>
#include <Editor/Widgets/ThumbnailSubject.hpp>

#include <Common/Core/JobSystem.hpp>
#include <Common/Core/Logger.hpp>

#include <chrono>

namespace Desert::Editor
{
    namespace
    {
        // A material that was pending when the sweep reached it: its bytes arrived on a later frame, and
        // it goes to the service exactly as a resident one would have. A refusal is the same one warning a
        // resident material's would have been — once per material, because only one read is ever issued.
        void HandOverArrivedMaterial( const std::string& assetPath, const Common::ResultStr<ThumbnailSubject::Material>& resolved )
        {
            if ( !resolved )
            {
                LOG_WARN( "[Thumbnails] sweep skipped '{}': {}", assetPath, resolved.GetError() );
                return;
            }
            ThumbnailService::Get().RequestMaterial( resolved.GetValue().Handle, assetPath, resolved.GetValue().How );
        }
    } // namespace

    // The half that needs the editor's device-bound service. Its sibling, ThumbnailScan.cpp, holds the
    // scan and the per-frame budget and includes none of this — see the note at the top of that file.

    void ThumbnailSweeper::Tick( Assets::AssetManager* manager, const std::filesystem::path& root )
    {
        if ( root.empty() )
            return;

        // A DIFFERENT PROJECT IS A DIFFERENT QUESTION. Without this a pass started under the old root
        // would go on queueing captures for files that are no longer in front of anybody, and — because
        // the paths are absolute — would keep the old project's thumbnail cache warm instead of the new
        // one's.
        if ( m_Root != root )
        {
            Reset();
            m_Root = root;
        }

        // ── Collect ───────────────────────────────────────────────────────────────────────────────────
        if ( m_Scan.valid() && m_Scan.wait_for( std::chrono::seconds( 0 ) ) == std::future_status::ready )
        {
            m_Pending         = m_Scan.get();
            m_Scan            = {};
            m_Next            = 0;
            m_OfferedThisPass = 0;
            m_Announced       = false;
        }

        // ── Hand out this frame's share ───────────────────────────────────────────────────────────────
        if ( !m_Pending.empty() )
        {
            Drain(
                 [this, manager]( const ThumbnailSweepCandidate& candidate )
                 {
                     // NEW TO THIS SWEEP? Only for the announcement and for the one warning below — the
                     // hand-over itself happens either way, because an asset edited since it was offered
                     // has to be re-queued and only the service can decide whether that costs a capture.
                     //
                     // AN ASSET THAT CAN NEVER HAVE A PICTURE IS RE-FOUND FOR EVER, and that is by
                     // design rather than an oversight: the freshness rule answers "no usable picture"
                     // about it truthfully on every pass, and the service's failure set is what stops the
                     // work. What must not repeat is the LOG LINE — three refusable materials would
                     // otherwise print three warnings every three seconds for the whole session.
                     const bool firstOffer = m_Offered.insert( candidate.AssetPath ).second;
                     if ( firstOffer )
                         ++m_OfferedThisPass;

                     switch ( candidate.By )
                     {
                         case ThumbnailFormats::Producer::Painted:
                             // No manager, no handle, no renderer: the painter opens the file itself.
                             ThumbnailService::Get().RequestPainted( candidate.AssetPath );
                             return;

                         case ThumbnailFormats::Producer::RenderedMaterial:
                         {
                             if ( !manager )
                                 return;
                             const auto subject = ThumbnailSubject::ResolveMaterial(
                                  *manager, candidate.AssetPath, &HandOverArrivedMaterial );
                             if ( !subject )
                             {
                                 // A background pass must not shout. This is the ONLY line a failed
                                 // background resolve produces, it is a warning rather than an error
                                 // because nobody asked for this asset by name, and it is printed ONCE
                                 // per asset per session — see `firstOffer` above.
                                 if ( firstOffer )
                                     LOG_WARN( "[Thumbnails] sweep skipped '{}': {}", candidate.AssetPath,
                                               subject.GetError() );
                                 return;
                             }
                             // PENDING: the material's bytes are on a worker; OnMaterialArrived hands it
                             // over when they land, so nothing reads in this frame and nothing re-scans.
                             if ( !subject.GetValue() )
                                 return;
                             ThumbnailService::Get().RequestMaterial( subject.GetValue()->Handle,
                                                                      candidate.AssetPath,
                                                                      subject.GetValue()->How );
                             return;
                         }

                         case ThumbnailFormats::Producer::RenderedMesh:
                         {
                             if ( !manager )
                                 return;
                             const auto subject = ThumbnailSubject::ResolveMesh( *manager, candidate.AssetPath );
                             if ( !subject )
                             {
                                 // AN UNCOOKED MESH IS THE COMMON CASE AND IS NOT NEWS. Logging it would
                                 // put one line per uncooked source in the log every three seconds for
                                 // the life of the session, which is how a log stops being read.
                                 return;
                             }
                             ThumbnailService::Get().RequestMesh( subject.GetValue().Handle,
                                                                  subject.GetValue().CookedPath,
                                                                  subject.GetValue().Material );
                             return;
                         }

                         case ThumbnailFormats::Producer::Decoded:
                         case ThumbnailFormats::Producer::Authored:
                         case ThumbnailFormats::Producer::None:
                             // Unreachable: ScanForMissingThumbnails only emits generated producers. No
                             // default label, so a fifth producer added to the census makes THIS switch
                             // fail to compile rather than silently drop every asset of the new kind.
                             return;
                     }
                 } );

            if ( m_Pending.empty() && !m_Announced )
            {
                m_Announced = true;
                if ( m_OfferedThisPass > 0 )
                {
                    LOG_INFO( "[Thumbnails] background sweep of '{}' found {} asset(s) with no usable "
                              "picture. Nothing was clicked; they render as slots and workers come free.",
                              m_Root.generic_string(), m_OfferedThisPass );
                }
            }
            return; // one thing per frame: hand out, or scan. Never both.
        }

        // ── Start the next pass ───────────────────────────────────────────────────────────────────────
        if ( m_Scan.valid() )
            return;

        // NOT WHILE THE QUEUE IT FEEDS IS STILL FULL. A scan run against a queue of a hundred pending
        // captures re-finds exactly those hundred — their pictures have not been written yet, so the
        // freshness rule still says "capture" for every one of them — and hands them over again for the
        // service to reject. Measured before this gate: a 133-asset cold cache produced one full scan
        // every 1.4 seconds for the entire drain, each one a directory walk and 266 stat calls on a
        // worker, to discover nothing that was not already known.
        //
        // It also makes the INTERVAL mean what its comment says. Three seconds is the latency of "I just
        // dropped a file in"; without this gate the real interval was however long a pass took to hand
        // out, and the sweep spent the drain re-asking a question it was still waiting on the answer to.
        if ( ThumbnailService::Get().HasWork() )
            return;

        if ( --m_FramesUntilScan > 0 )
            return;
        m_FramesUntilScan = kFramesBetweenScans;

        // THROUGH THE JobSystem. A directory walk plus two stats per file is not free, and the one thing
        // it must never do is happen on the frame's thread — a 30 ms hitch every three seconds is exactly
        // the sort of cost a background convenience has no right to charge. `Async` captures the root by
        // value, so the future owns everything it reads.
        m_Scan = Common::JobSystem::Get().Async(
             [root = m_Root] { return ScanForMissingThumbnails( root, kCandidatesPerScan ); } );
    }
} // namespace Desert::Editor
