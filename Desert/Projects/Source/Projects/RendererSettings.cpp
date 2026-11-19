#include "RendererSettings.hpp"

#include "EngineConfig.hpp"

namespace Desert::Project
{
    // Config/Engine.json is read and written whole by EngineConfig.cpp (the renderer's keys, the WorldPartition
    // section and this platform's layer over both); these are the renderer's doors into it.
    const RendererSettings& CurrentRendererSettings()
    {
        return CurrentEngineConfig().Effective.Renderer.get();
    }

    const RendererSettings& ProjectRendererSettings()
    {
        return CurrentEngineConfig().Project.Renderer.get();
    }

    Common::BoolResultStr SaveRendererSettings( const RendererSettings& toSave )
    {
        EngineConfig project   = CurrentEngineConfig().Project;
        project.Renderer.get() = toSave;
        return SaveEngineConfig( project );
    }
} // namespace Desert::Project
