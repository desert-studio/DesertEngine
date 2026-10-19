#pragma once

#include <Common/Core/ResultStr.hpp>

namespace Desert::Project
{
    // The project's rendering settings — UE's URendererSettings, the [/Script/Engine.RendererSettings] section of
    // DefaultEngine.ini — kept in `<project>/Config/Engine.json` beside Game.json. What the PROJECT decides about
    // its rendering, once, for every machine it runs on; what a machine can afford is Scalability, not this.
    struct RendererSettings
    {
        // UE r.TSR.ThinGeometryDetection (default 0): TSR detects sub-pixel geometry (foliage, wires) from the
        // base passes' coverage and relaxes its history rejection there (TemporalSuperResolution.hpp,
        // TSR.DetectThinGeometry). A project setting in UE too, not a scalability row: it costs a pass and a
        // history, and only a project with such content wants it.
        bool TsrThinGeometryDetection = false;
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
