#pragma once

#include <Projects/RendererSettings.hpp>
#include <Projects/WorldPartitionSettings.hpp>

#include <Common/Core/ResultStr.hpp>
#include <Common/Json/Json.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace Desert::Project
{
    // `<project>/Config/Engine.json`, UE's DefaultEngine.ini: the engine settings a PROJECT decides, one file with
    // a section per subsystem. The renderer's keys sit at the top level (the file's first layout, kept as it was),
    // the streaming ones under "WorldPartition".
    //
    // THE PLATFORM LAYER (UE's Config/<Platform>/<Platform>Engine.ini): `<project>/Config/<Platform>/Engine.json`,
    // <Platform> = Mac, Windows or Linux, the host this binary is built for. It states only the keys it changes:
    // each replaces the project's, member by member down nested objects, and what it does not state is the
    // project's. Both layers are read strictly: a key the struct does not declare is an error naming its path.
    struct EngineConfig
    {
        rfl::Flatten<RendererSettings> Renderer;
        WorldPartitionSettings         WorldPartition;
    };

    struct EngineConfigLayers
    {
        EngineConfig Project;   // Config/Engine.json alone: what Project Settings shows and writes
        EngineConfig Effective; // with this platform's layer over it: what this host runs with
        // The platform layer's file and every key path it sets ("WorldPartition.BlockOnSlowStreaming"); both
        // empty when the project has no layer for this platform.
        std::string              PlatformFile;
        std::vector<std::string> PlatformKeys;
    };

    // The platform layer's folder for the host this binary is built for: Mac, Windows or Linux.
    std::string_view PlatformConfigName();

    // The open project's two layers, read once per project directory and again after a save. No project or no
    // file: the defaults. A project layer that does not parse is logged with the parser's words and reads as the
    // defaults; a platform layer that does not parse is logged the same way and this host runs the project layer.
    const EngineConfigLayers& CurrentEngineConfig();

    // The one writer of `<project>/Config/Engine.json`: the whole project layer, atomic, canonical layout. The
    // platform layer is authored by hand (as UE's platform ini) and never written here. Refuses with the reason
    // (no project open, the write failed) and leaves the file and the cache as they were.
    Common::BoolResultStr SaveEngineConfig( const EngineConfig& projectLayer );
} // namespace Desert::Project
