#pragma once

#include <Common/Settings/CapabilityCatalog.hpp>

#include <string>
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
        // refresh rate). Off = the lowest-latency mode the surface offers. Default OFF, as UE's r.VSync=0: the
        // editor's pacing before SCAL1 (IMMEDIATE, else MAILBOX, else FIFO) stays what a fresh machine gets.
        bool VSync = false;

        // HDR-OUTPUT (owner 2026-10-27; UE r.HDR.EnableHDROutput + r.HDR.Display.OutputDevice). The swapchain's
        // output encoding, chosen ONLY from CapabilityCatalog::DisplayOutputs: an entry the device/surface does
        // not list is a refusal (ResolveDisplayOutput), never a silent SDR. Readers: the swapchain (format +
        // colour space), SceneComposite (the output transform), the editor's display encode (UI on paper
        // white). Default SDR_sRGB: a fresh machine presents what it did before HDR existed.
        DisplayOutput Output = DisplayOutput::SDR_sRGB;

        // The luminance of UI/document white on an HDR output, in nits (UE r.HDR.UI.Level x the display's SDR
        // white). 0 = what the OS reports. macOS reports no nits for EDR white (only headroom ratios), so there
        // the authored value is required for a nits figure: ITU-R BT.2408 reference white, 203 nits, is the
        // default. Manual correction for a display whose OS report is wrong (contract: no hidden fallback).
        float PaperWhiteNits = 203.0f;

        // The display's peak in nits. 0 = from the OS (Mac: EDR headroom x paper white; Windows: DXGI
        // MaxLuminance). > 0 overrides it — the manual correction UE exposes as r.HDR.Display.MaxLuminance.
        float PeakNits = 0.0f;

        bool operator==( const DisplaySettings& ) const = default;
    };

    // The display output the swapchain builds with, or why it cannot (empty Error = Output as asked).
    // PURE. No fallback: an output the catalog does not list returns the reason, and the swapchain refuses to
    // build with it (logged) — the caller keeps the swapchain it has. HDR10_PQ is listed by the catalog but
    // its PQ output transform is not written yet (HDR-OUTPUT p2): it is refused here with that reason, not
    // presented as SDR.
    struct ResolvedDisplayOutput
    {
        DisplayOutput Output = DisplayOutput::SDR_sRGB;
        std::string   Error;

        bool operator==( const ResolvedDisplayOutput& ) const = default;
    };
    [[nodiscard]] ResolvedDisplayOutput ResolveDisplayOutput( const DisplaySettings&   settings,
                                                              const CapabilityCatalog& catalog );

    // The engine's name for an output (logs, settings UI).
    [[nodiscard]] std::string_view DisplayOutputName( DisplayOutput output );

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
