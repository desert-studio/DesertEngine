#pragma once

// WHICH VENDOR UPSCALERS THIS BINARY CONTAINS — the one place the build's answer is read from.
//
// A vendor upscaler is compiled in only where its SDK can be built: FSR needs the FidelityFX SDK's shader
// compiler, a Windows binary (BuildScripts/ThirdParty/FidelityFX.lua sets DESERT_UPSCALER_FSR). Every reader —
// the device catalog's FSR gate (VulkanCapabilityCatalog: FsrDeviceSupport), SelectTemporalMethod and
// CreateTemporalUpscaler — asks this constant, so the three can never disagree about whether FSR exists.
namespace Desert::Graphic
{
#if defined( DESERT_UPSCALER_FSR )
    inline constexpr bool kFsrBuilt = true;
#else
    inline constexpr bool kFsrBuilt = false;
#endif
} // namespace Desert::Graphic
