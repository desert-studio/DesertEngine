#pragma once

#include <Common/Core/ResultStr.hpp>

#include <cstdint>

namespace Desert::Project
{
    // The project's rendering settings — UE's URendererSettings, the [/Script/Engine.RendererSettings] section of
    // DefaultEngine.ini — kept in `<project>/Config/Engine.json` beside Game.json. What the PROJECT decides about
    // its rendering, once, for every machine it runs on; what a machine can afford is Scalability, not this.
    // UE ETextureEncodeSpeed (TextureEncodingSettings.h): which of the two encode efforts a process builds with.
    enum class TextureEncodeSpeed : uint32_t
    {
        Final,            // always the final effort: a miss is built final
        FinalIfAvailable, // a final entry already in the DDC is used (a cook put it there); a miss is built fast
        Fast,             // always the fast effort
    };

    // UE UTextureEncodingProjectSettings ([/Script/Engine.TextureEncodingProjectSettings]): the block encoder's
    // two efforts and which process uses which. The editor derives on a DDC miss while the user waits, so it
    // builds fast and takes a final entry whenever one exists; the cook (TextureCook, the packager) builds
    // what ships. An effort is the BC7 kernel's quality, 0..1 (Compressonator CMP_Core SetQualityBC7); it is
    // part of every texture's DDC key, so a final entry and a fast entry never stand for each other by accident.
    struct TextureEncodingSettings
    {
        // Measured on dandelion_01_diff_2k (dilated, all levels, bcdec): 0.1 -> worst texel 28 in 8 s single
        // thread, 1.0 -> worst 20 in 114 s. Both clear the gate (ceiling 32); the final one is what ships.
        float              FastEffort  = 0.1f;
        float              FinalEffort = 1.0f;
        TextureEncodeSpeed EditorUses  = TextureEncodeSpeed::FinalIfAvailable;
        TextureEncodeSpeed CookUses    = TextureEncodeSpeed::Final;
    };

    struct RendererSettings
    {
        // UE r.TSR.ThinGeometryDetection (default 0): TSR detects sub-pixel geometry (foliage, wires) from the
        // base passes' coverage and relaxes its history rejection there (TemporalSuperResolution.hpp,
        // TSR.DetectThinGeometry). A project setting in UE too, not a scalability row: it costs a pass and a
        // history, and only a project with such content wants it.
        bool TsrThinGeometryDetection = false;
        // UE's high-DPI game viewport switch (r.SecondaryScreenPercentage.GameViewport, auto mode, off unless the
        // project opts in): false -- the GAME's view renders at the window's logical (point) resolution, the
        // display's pixels / Window::GetContentScale, and the present blit upscales it to the swapchain; on a
        // Retina panel that is half the pixels per side, a quarter of every resolution-proportional target and
        // history. true -- the view renders at the native pixels. The quality's Resolution.Percent applies on top
        // of either (the temporal upscaler reconstructs up to this extent). The editor's viewports ignore it.
        bool GameViewportHighDPI = false;
        // The texture cook's encode efforts (UE Project Settings > Engine > Texture Encoding).
        TextureEncodingSettings TextureEncoding;
    };

    // The open project's Config/Engine.json, read once per project directory. A project without the file gets
    // the defaults above (UE's DefaultEngine.ini without the key); a file that does not parse is logged with the
    // parser's words and also reads as the defaults.
    const RendererSettings& CurrentRendererSettings();

    // The one writer of `<project>/Config/Engine.json` (the editor's Project Settings panel, ▸ Rendering).
    // Atomic, canonical layout; on success CurrentRendererSettings() answers @p toSave. Refuses with the reason
    // (no project open, the write failed) and leaves the file and the cache as they were.
    Common::BoolResultStr SaveRendererSettings( const RendererSettings& toSave );
} // namespace Desert::Project
