#pragma once

#include <Projects/NetSettings.hpp>
#include <Projects/RendererSettings.hpp>
#include <Projects/WorldPartitionSettings.hpp>

#include <Common/Core/ResultStr.hpp>
#include <Common/Json/Json.hpp>

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::Project
{
    // `<project>/Config/Engine.json`, UE's DefaultEngine.ini: the engine settings a PROJECT decides, one file with
    // a section per subsystem. The renderer's keys sit at the top level (the file's first layout, kept as it was),
    // the streaming ones under "WorldPartition", the server's replication-by-distance ones under "Net" (WP-FAR-15).
    //
    // THE PLATFORM LAYER (UE's Config/<Platform>/<Platform>Engine.ini): `<project>/Config/<Platform>/Engine.json`,
    // <Platform> = Mac, Windows or Linux, the host this binary is built for. It states only the keys it changes:
    // each replaces the project's, member by member down nested objects, and what it does not state is the
    // project's. Both layers are read strictly: a key the struct does not declare is an error naming its path.
    //
    // THE COMMAND-LINE LAYER (NET-SCN-VERBS; UE's `-ini:Engine:[Section]:Key=Value`): items a process is started
    // with (DesertServer `--config Net.BudgetBytesPerSecond=2048`, repeatable), overlaid on the platform layer's
    // result the same way, key by key. It exists for a RUN - a scenario that lowers the budget or hands the server
    // bands it must refuse - and touches no file: a scenario never writes into the tree it plays in. The order
    // is project < platform < command line; SaveEngineConfig writes the project layer alone and never an override.
    struct EngineConfig
    {
        rfl::Flatten<RendererSettings> Renderer;
        WorldPartitionSettings         WorldPartition;
        NetSettings                    Net;
    };

    struct EngineConfigLayers
    {
        EngineConfig Project;   // Config/Engine.json alone: what Project Settings shows and writes
        EngineConfig Effective; // with this platform's layer and the command-line layer over it: what this host runs
        // The platform layer's file and every key path it sets ("WorldPartition.BlockOnSlowStreaming"); both
        // empty when the project has no layer for this platform.
        std::string              PlatformFile;
        std::vector<std::string> PlatformKeys;
        // Every key path the command-line layer sets ("Net.BudgetBytesPerSecond"), in the order given; empty when
        // SetEngineConfigOverrides was never called or was called with nothing.
        std::vector<std::string> OverrideKeys;
    };

    // The platform layer's folder for the host this binary is built for: Mac, Windows or Linux.
    std::string_view PlatformConfigName();

    // The open project's layers, read once per project directory and again after a save or a
    // SetEngineConfigOverrides. No project or no file: the defaults. A project layer that does not parse is logged
    // with the parser's words and reads as the defaults; a platform layer that does not parse is logged the same
    // way and this host runs the project layer. The command-line layer is verified when it is set (below), so its
    // application here cannot fail on its own.
    const EngineConfigLayers& CurrentEngineConfig();

    // THE COMMAND-LINE LAYER, set once by the process before it needs the config (DesertServer: right after its
    // arguments parse). Each item is "<Section>.<Key>=<JSON value>" - "Net.BudgetBytesPerSecond=2048",
    // "WorldPartition.UnloadMargin=0.5", `Net.Bands=[{"ToCm":5000,"EverySteps":2},{"ToCm":0,"EverySteps":4}]` -
    // the dotted path walks nested objects, the value is parsed as JSON (a bare word that is not JSON is a
    // refusal, not a string). Refuses, naming the item and the reason, when an item has no '=', its path is empty,
    // its value does not parse as JSON, or the path names a key the EngineConfig struct does not declare (checked
    // here by overlaying the item on the defaults and reading the result strictly, so a typo is a refusal at the
    // start and never a silently defaulted run). On refusal nothing changes. On success the overrides replace any
    // earlier set (an empty span clears them) and the cache is invalidated: the next CurrentEngineConfig() applies
    // them. The VALUES are not judged here: the "Net" section's rule is ValidateNetSettings and the server's start
    // applies it to the effective config (exit code 7) - that is how a scenario hands the server bands it must
    // refuse.
    Common::BoolResultStr SetEngineConfigOverrides( std::span<const std::string> items );

    // The one writer of `<project>/Config/Engine.json`: the whole project layer, atomic, canonical layout. The
    // platform layer is authored by hand (as UE's platform ini) and never written here; the command-line layer is
    // never written anywhere. Refuses with the reason (no project open, the write failed) and leaves the file and
    // the cache as they were.
    Common::BoolResultStr SaveEngineConfig( const EngineConfig& projectLayer );
} // namespace Desert::Project
