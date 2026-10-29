#pragma once

// WHICH VENDOR UPSCALERS THIS BINARY CONTAINS — the one place the build's answer is read from.
//
// A vendor upscaler is compiled in only where its SDK can be built, and Desert/Desert/CMakeLists.txt's Windows
// filter sets the define: FSR needs the FidelityFX SDK's shader compiler, a Windows binary (DESERT_UPSCALER_FSR,
// cmake/ThirdParty/FidelityFX.cmake); DLSS needs NVIDIA Streamline, shipped for Windows x64 only
// (DESERT_UPSCALER_DLSS, ThirdParty/Streamline). Every reader — the device catalog's gates, SelectTemporalMethod
// and CreateTemporalUpscaler — asks these constants, so they can never disagree about whether an upscaler exists.
namespace Desert::Graphic
{
    // Defined in UpscalerBuild.cpp, the one translation unit that reads the defines: a project that includes this
    // header without the engine's defines (a test, a tool) still gets the engine library's answer, never its own.
    extern const bool kFsrBuilt;
    extern const bool kDlssBuilt;
} // namespace Desert::Graphic
