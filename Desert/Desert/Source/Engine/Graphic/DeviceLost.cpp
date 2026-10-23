#include <Engine/Core/TextTemplate.hpp>
#include <Engine/Graphic/DeviceLost.hpp>

#include <Common/Core/Logger.hpp>

#include <atomic>
#include <mutex>
#include <string_view>

namespace Desert::Graphic
{
    namespace
    {
        // Atomic because the latch is read from the render thread and from whatever thread a driver
        // callback lands on. It is a one-way door — set once, never cleared outside a suite — so the
        // relaxed/acquire distinction buys nothing here and sequential consistency costs nothing at the
        // frequency this is asked at (once per gated entry point per frame).
        std::atomic<bool>          s_Lost{ false };
        std::atomic<std::uint64_t> s_Refused{ 0 };
        std::atomic<std::uint32_t> s_Reports{ 0 };

        // Guards the two strings only. They are written once, under the same flag that latches, and read
        // afterwards; the mutex is what makes "written once" true rather than hoped for.
        std::mutex  s_TextMutex;
        std::string s_FirstSite;
        std::string s_Explanation;

        // THE ONE MESSAGE. Composed here rather than at the call site so that every route into device loss
        // produces the same words — the value of a single explanation is that it is recognisable.
        constexpr std::string_view kExplanationText =
             "[DeviceLost] The GPU device was lost ({{ detail }}), first seen at {{ site }}.\n"
             "  WHAT HAPPENED: the graphics device this process was using no longer exists. On macOS this is "
             "almost never our doing — the driver resets the whole GPU when ANY client on the machine trips "
             "it, and every other client is discarded alongside (kIOGPUCommandBufferCallbackErrorInnocentVictim, "
             "\"innocent victim\"). Another editor, another application, a driver hiccup or a wake from sleep all "
             "produce this.\n"
             "  WHAT WE DID: stopped. A lost VkDevice cannot be revived, only replaced, so the engine issues no "
             "further GPU work and is closing down in order instead of dying mid-frame.\n"
             "  YOUR WORK: this session is marked as an unclean exit, so the next start offers to reopen your "
             "latest autosave. If you have unsaved changes and the editor is still on screen, it is already on "
             "its way out — the picture is frozen, not working.\n"
             "  WHAT TO DO: start the editor again. If it happens repeatedly while nothing else is using the GPU, "
             "the driver itself is the suspect, not this scene.\n"
             "  READING THE REST OF THIS LOG: every Vulkan error printed AFTER this line is a consequence of it, "
             "not an independent defect. \"pFences[...] is in use\" and \"Semaphore must not have any pending "
             "operations\" are what a dead device looks like from inside the validation layer — it says so "
             "itself, \"(a VK_ERROR_DEVICE_LOST has occurred)\". Do not go looking for a synchronisation bug; "
             "there isn't one.";

        std::string Compose( std::string_view site, std::string_view detail )
        {
            static const Desert::Core::TextTemplate kExplanation( "DeviceLost", kExplanationText );
            Common::Json::ObjectBuilder             model;
            model.Set( "site", site ).Set( "detail", detail );
            return kExplanation.RenderOrLog( Common::Json::Value( model.Build() ) );
        }
    } // namespace

    bool DeviceLost::Report( std::string_view site, std::string_view detail )
    {
        // exchange, not "read then write": two threads can discover the same loss in the same instant, and
        // the whole promise of this class is that exactly one of them prints.
        if ( s_Lost.exchange( true ) )
            return false;

        std::string text = Compose( site, detail );
        {
            std::lock_guard<std::mutex> lock( s_TextMutex );
            s_FirstSite   = std::string( site );
            s_Explanation = text;
        }
        s_Reports.fetch_add( 1 );

        // LOG_ERROR and not LOG_WARN: the run is ending. The text is passed as an ARGUMENT and never as the
        // format string — it contains braces from quoted validation-layer messages, and fmt would try to
        // parse them as placeholders and throw from inside the code reporting a device loss.
        LOG_ERROR( "{}", text );
        return true;
    }

    bool DeviceLost::IsLost() noexcept
    {
        return s_Lost.load();
    }

    bool DeviceLost::AllowWork() noexcept
    {
        if ( !s_Lost.load() )
            return true;
        s_Refused.fetch_add( 1 );
        return false;
    }

    std::uint64_t DeviceLost::RefusedCalls() noexcept
    {
        return s_Refused.load();
    }

    std::uint32_t DeviceLost::ReportCount() noexcept
    {
        return s_Reports.load();
    }

    std::string DeviceLost::FirstSite()
    {
        std::lock_guard<std::mutex> lock( s_TextMutex );
        return s_FirstSite;
    }

    std::string DeviceLost::Explanation()
    {
        std::lock_guard<std::mutex> lock( s_TextMutex );
        return s_Explanation;
    }

    void DeviceLost::ResetForTests() noexcept
    {
        std::lock_guard<std::mutex> lock( s_TextMutex );
        s_Lost.store( false );
        s_Refused.store( 0 );
        s_Reports.store( 0 );
        s_FirstSite.clear();
        s_Explanation.clear();
    }
} // namespace Desert::Graphic

// WHY THE ENGINE DOES NOT RECOVER FROM A LOST DEVICE, WITH THE NUMBERS BEHIND IT.
//
// Three answers were on the table. (c) — today's behaviour, die inside VK_CHECK_RESULT having explained
// nothing and saved nothing — is not one of them; it is the defect. That leaves rebuilding the device, or
// closing cleanly.
//
// REBUILDING was costed before it was refused, over this tree on 2026-09-07:
//   * 200 Vulkan/VMA call sites across 21 of our files. Every one of them reaches a VkDevice, a VkQueue or
//     a VmaAllocator that was captured at startup; after a rebuild each of those handles is a dangling
//     value that still looks valid.
//   * 43 creation sites (vkCreate*/vkAllocate*/vmaCreate*) across 16 files, and 50 Vk* member handles in
//     16 classes. Each needs an invalidate-and-rebuild entry point; 11 resource classes have one today,
//     the rest do not.
//   * NOTHING CAN ENUMERATE THE LIVE RESOURCES. There is one global registry; everything else — a panel's
//     offscreen SceneRenderer, a thumbnail's framebuffer, a material's descriptor sets, the ImTextureID
//     every open panel is holding — is owned by whoever made it. Recovery would first have to build an
//     ownership census that does not exist, and a census that misses one owner produces a stale handle
//     that renders garbage instead of failing.
// That is a subsystem, not a task, and it is filed as one.
//
// CLOSING CLEANLY is what this file implements, and on this platform it is also the better answer rather
// than merely the cheaper one. An `kIOGPUCommandBufferCallbackErrorInnocentVictim` means the machine's
// whole GPU stack has just been through recovery. The user's data is on the CPU side and completely
// intact at that instant; the single most valuable thing the process can do is get it to disk and stop,
// which is exactly what a marked-unclean exit plus the existing autosave does.
//
// WHAT WOULD CHANGE THE ANSWER: a resource ownership registry that can enumerate every live GPU object
// (wanted for other reasons too — teardown order, hot reload, and the "which slot is this?" question the
// renderer-slot work keeps asking). Once that exists, rebuilding becomes a walk over a list instead of an
// archaeology exercise, and this refusal should be revisited.
//
// THAT CONDITION HAS NOW BEEN TESTED, AND THE REFUSAL STANDS — 2026-09-08, owner's decision, taken on the
// number rather than on an estimate.
//
// The asset-eviction work (A7) built exactly the kind of accounting this comment was waiting for, and its
// own obligation was to say, in a number, how much of the GPU it can actually see. The answer:
//
//     live GPU objects, tree-wide ........... 683
//     unreachable for the asset registry .... 330   (48 %)
//
// So the registry the condition above asked for arrived and covers barely half the problem. The other 330
// are precisely the objects listed further up — preview render targets, thumbnails, descriptor sets,
// pipelines, frame buffers — the ones with no asset behind them to be registered against. Recovery would
// still have to build a second ownership registry for that half, from scratch, with the same failure mode
// as before: a census that misses one owner yields a stale handle that renders garbage instead of failing.
//
// The measurement is the deliverable here. A7 made this task cheaper, not free, and "cheaper" turned out
// not to be enough — which is a result, not a stall. Clean closure plus the emergency save stays the
// shipped behaviour.
//
// WHAT WOULD CHANGE THE ANSWER NOW is narrower and therefore more useful than what it replaced: not "an
// ownership registry" in general, but a registry that owns the 330 — the objects created by panels,
// thumbnails and the renderer's own per-frame state. If any other task ever needs to enumerate those (a
// live memory budget per panel, or preview teardown that is not a leak, would both need it), then the cost
// of this one collapses and it should be re-costed on the spot. Until then, do not re-derive this: the
// number above is the argument.
