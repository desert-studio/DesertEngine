#pragma once

#include <Common/Core/ResultStr.hpp>

#include <filesystem>

// THE ONE PLACE A HOST STARTS THE MACHINE'S QUALITY (SCAL1). The editor and the packaged game call this once,
// after the device exists and before their first SceneRenderer, so both boot the same way:
//   1. parse the level table, Resources/Config/Scalability.json (Common::Constants::Path::CONFIG_PATH);
//   2. load machine.json with it (MachineSettings::Load needs the High values to migrate the retired keys);
//   3. Scalability::QualityState::Initialize on the device's CapabilityCatalog, with machine.json as its Saver;
//   4. subscribe the one global push the sampler path reads (RenderConfig::TextureFilter / AnisotropyLevel) and
//      apply it once for the starting resolution.
// A table that cannot be read or parsed is an ERROR the host returns from OnAttach: there is no built-in copy of
// the levels to fall back to (Scalability.hpp, QualityState::Initialize).
namespace Desert::Graphic::QualityBoot
{
    [[nodiscard]] Common::BoolResultStr Start( const std::filesystem::path& machineJson );
} // namespace Desert::Graphic::QualityBoot
