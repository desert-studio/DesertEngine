#pragma once

#include <Common/Settings/CapabilityCatalog.hpp>

#include <string_view>

// DISPLAY SETTINGS — HOW FRAMES REACH THE SCREEN, OUTSIDE THE SCALABILITY GROUPS (SCAL1, owner 2026-10-06).
//
// UE keeps VSync, the window mode and the output resolution in UGameUserSettings, not in an sg.* group: they are
// not "quality" — no level of a group may turn VSync off, and "Overall: Low" must not change how the swapchain
// presents. The same split here: Scalability.hpp's groups never touch the present mode; this type does, and the
// swapchain is its reader. Persisted in machine.json beside QualitySelection (a machine's monitor, not the
// project's or the level's: two people on one project legitimately differ, contract §6 question 1).
namespace Common::Scalability
{
    struct DisplaySettings
    {
        // Wait for the display's vertical blank before presenting (no tearing, frame rate capped at the
        // refresh rate). Off = the lowest-latency mode the surface offers.
        bool VSync = true;

        bool operator==( const DisplaySettings& ) const = default;
    };

    // What the swapchain presents with, and why it is not what was asked (empty Reason = as asked).
    struct ResolvedPresentMode
    {
        PresentMode      Mode = PresentMode::Fifo;
        std::string_view Reason;

        bool operator==( const ResolvedPresentMode& ) const = default;
    };

    // PURE. VSync on -> Fifo (always offered: the Vulkan spec guarantees it). VSync off -> Immediate (tears,
    // lowest latency); else Mailbox (no tearing, uncapped, one frame more latency); else Fifo with the reason
    // "the surface offers no present mode without VSync". Replaces the swapchain's own silent
    // IMMEDIATE -> MAILBOX -> FIFO walk; the swapchain logs a non-empty Reason once.
    [[nodiscard]] ResolvedPresentMode ResolvePresentMode( const DisplaySettings&   settings,
                                                          const CapabilityCatalog& catalog );
} // namespace Common::Scalability
