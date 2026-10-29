#pragma once

#include <Common/Core/ResultStr.hpp>

#include <filesystem>

// THE ONE PLACE A HOST STARTS THE MACHINE'S QUALITY (SCAL1). The editor and the packaged game call this once,
// after the device exists and before their first SceneRenderer, so both boot the same way:
//   1. parse the level table, Resources/Config/Scalability.json (Common::Constants::Path::CONFIG_PATH);
//   2. machine.json is ALREADY loaded — by the Application, before the device
//   (ApplicationInfo::MachineSettingsFile,
//      the device reads HideRayTracing); Start refuses a path other than the one loaded;
//   3. Scalability::QualityState::Initialize on the device's CapabilityCatalog, with machine.json as its Saver;
//   4. subscribe the one global push the sampler path reads (RenderConfig::TextureFilter / AnisotropyLevel) and
//      apply it once for the starting resolution.
// A table that cannot be read or parsed is an ERROR the host returns from OnAttach: there is no built-in copy of
// the levels to fall back to (Scalability.hpp, QualityState::Initialize).
namespace Desert::Graphic::QualityBoot
{
    // Where a quality change goes. ThisProcess is an unattended run (a scenario or a capture, the editor's
    // IsUnattendedSession): changes apply live and machine.json is neither written nor migrated — the file is
    // byte-for-byte what it was before the run (UE: -ExecCmds / automation cvars never reach
    // GameUserSettings.ini).
    enum class Persistence
    {
        MachineFile,
        ThisProcess
    };
    [[nodiscard]] Common::BoolResultStr Start( const std::filesystem::path& machineJson, Persistence persistence );
} // namespace Desert::Graphic::QualityBoot
