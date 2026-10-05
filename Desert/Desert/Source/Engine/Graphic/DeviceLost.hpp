#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace Desert::Graphic
{
    /**
     * @brief THE RENDER DEVICE IS GONE, AND THE ENGINE HAS TO STOP TALKING TO IT.
     *
     * A `VK_ERROR_DEVICE_LOST` is not a bug in this engine and it is not an unreachable state. On macOS it
     * is a routine, expected event: when anything on the machine trips the GPU, the driver resets the whole
     * device and discards every client on it — `kIOGPUCommandBufferCallbackErrorInnocentVictim`, "you were
     * standing next to the accident". A machine running several editors at once meets it regularly, and a
     * machine running one meets it on a driver hiccup, a hibernate, or a heavy neighbour.
     *
     * WHAT THIS EXISTS TO PREVENT, and it is a DIAGNOSTIC failure at least as much as a crash. Before this
     * class the engine noticed nothing and kept going: it reset fences the dead device still held, acquired
     * an image with a semaphore that still had pending operations, tore the swapchain down and built a new
     * one, and finally died inside `VK_CHECK_RESULT` — treating an EXPECTED result as an impossible one.
     * The log that produced reads, from the bottom up, as a synchronisation defect of ours: "pFences[0] is
     * in use", "Semaphore must not have any pending operations". Both are TRUE, both are CONSEQUENCES, and
     * the validation layer says so in a clause that is easy to read past — "(a VK_ERROR_DEVICE_LOST has
     * occurred, the fence must be destroyed)". A reader who starts at the last line goes and fixes
     * synchronisation that was never broken. The lead nearly filed that task; this class is what stops the
     * next person from filing it.
     *
     * THE SHAPE. One latch, set once, never cleared:
     *
     *  - DISCOVERY. Every place the engine looks at a Vulkan result routes `VK_ERROR_DEVICE_LOST` here
     *    through `Report()`. The FIRST report is the one that owns the message; later ones are silent, so
     *    the human sees one explanation and not twenty consequences of it.
     *  - REFUSAL. Every entry point that would issue device work opens with `AllowWork()`. Once the latch
     *    is set it answers false and counts the refusal, so the number of things the engine tried to do
     *    after the device died is a number a test can read rather than a claim someone makes.
     *  - TEARDOWN IS DELIBERATELY EXEMPT. `vkDestroy*`, `vkFree*` and `vmaDestroy*` remain legal on a lost
     *    device — the specification requires them to work, and they return nothing to check. Refusing them
     *    too would mean leaking every GPU object and never destroying the device, which is the opposite of
     *    the orderly close this class exists to make possible. That is the one and only category of Vulkan
     *    call the engine still issues after the latch. `DeviceLostCensus` excludes it BY PREFIX RULE
     *    (`vkDestroy*`, `vkFree*`, `vma*Destroy/Free`) rather than by a typed list, so the exemption cannot
     *    quietly widen the way a hand-kept list of names would.
     *
     * WHY IT LIVES IN `Engine/Graphic` AND CARRIES NO VULKAN TYPE. Two of its readers may not see a Vulkan
     * header: `Engine::Application`, which ends the run, and the editor's crash recovery, which must not
     * call a device-lost session a clean exit. Layers do not mix, so the backend decides what counts as
     * device loss and hands this class plain text.
     *
     * NOT RECOVERABLE, ON PURPOSE. See `DeviceLost.cpp` for the measured argument: rebuilding the device
     * would mean re-deriving 200 Vulkan call sites' cached handles, 43 creation sites and 50 member
     * handles across 16 classes, with no registry that can even enumerate the live resources to rebuild.
     * The honest answer at the moment of loss is to get the user's work to disk and close.
     */
    class DeviceLost final
    {
    public:
        /// Latches the state and, on the FIRST call only, prints the one human-facing explanation.
        /// @param site   where it was noticed, e.g. "VulkanFrameLoop::Submit / vkQueueSubmit".
        /// @param detail the driver's own words for the result, e.g. "VK_ERROR_DEVICE_LOST".
        /// @return true iff this call was the first — the caller that gets true owns the event.
        static bool Report( std::string_view site, std::string_view detail );

        [[nodiscard]] static bool IsLost() noexcept;

        /// THE GATE. Asked FIRST by every entry point that would issue device work. Answers false once the
        /// device is lost, counting the refusal.
        [[nodiscard]] static bool AllowWork() noexcept;

        /// How many times `AllowWork()` refused — i.e. how much work the engine tried to issue at a dead
        /// device. Zero until the latch; after it, the count of everything that was stopped.
        [[nodiscard]] static std::uint64_t RefusedCalls() noexcept;

        /// How many times the explanation was printed. Exactly 1 after any number of reports: the whole
        /// point is that the human reads one cause and not twenty effects.
        [[nodiscard]] static std::uint32_t ReportCount() noexcept;

        /// Where it was first noticed, empty while the device is alive.
        [[nodiscard]] static std::string FirstSite();

        /// The same text `Report()` logged, for a caller that wants to show it rather than log it.
        [[nodiscard]] static std::string Explanation();

        /// ONLY for suites, which must be able to drive the latch from both sides in one process. Nothing
        /// in the engine may call it: a device that came back is not a thing that happens.
        static void ResetForTests() noexcept;
    };
} // namespace Desert::Graphic
