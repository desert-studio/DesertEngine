#pragma once

// THE ONE PLACE THE ENGINE LOADS ITS SHADING MODELS: the registry of the shader root the compiler includes from
// (Common::Constants::Path::SHADERDIR_PATH), scanned once per root per process. Every consumer reaches the set
// through here — the parser resolving a template's `ShadingModel <Name>`, the shader cache keys mixing the
// Guid->index layout, the includer serving kGeneratedInclude — so the editor and DShaderTool cannot hold two
// different sets, and the generated include is written before anything reads it.
//
// A failed scan is the answer for that root: every template, key and include that needs the set carries the
// scan's error (there is no partial registry and no fallback model).
//
// READ THROUGH THE VFS, so the packaged game reads the set the cook shipped: the whole shader root (every
// .shadingmodel, SurfaceTypes.glslh and kGeneratedInclude as the cook wrote it) is a PackagedContentTrees row
// and lives in the pak, where no loose directory exists to scan. There the generated include already holds the
// loaded set's bytes, so nothing is written; a pak whose include disagrees with its own models is refused by the
// attempted write, naming the path.
//
// ONE SET PER ROOT AT A TIME, replaced whole by ReloadShaderRootShadingModels (the editor's hot reload of a
// .shadingmodel). Callers hold the shared_ptr for the length of one question, so a reload on another thread
// never pulls a set out from under a parse or a key in flight.

#include <Engine/Core/ShaderCompiler/ShadingModels/ShadingModelRegistry.hpp>

#include <Common/Core/ResultStr.hpp>

#include <memory>
#include <string>
#include <string_view>

namespace Desert::Core::ShadingModels
{
    struct LoadedShadingModels
    {
        ShadingModelRegistry Registry;
        std::string          GeneratedGlsl;  // Registry.GenerateGlsl(), also written to kGeneratedInclude
        std::string          IndexLayoutKey; // Registry.IndexLayoutKey()
    };

    using ShaderRootSet = std::shared_ptr<const Common::ResultStr<LoadedShadingModels>>;

    // The set of the current shader root (SHADERDIR_PATH against the current directory). The first call for a
    // root scans it and writes kGeneratedInclude beside the models when its bytes differ; later calls return
    // the same object until a reload replaces it. Never null.
    ShaderRootSet ShaderRootShadingModels();

    // Scans the current root again (a .shadingmodel was edited). Success replaces the set and rewrites
    // kGeneratedInclude when its bytes changed, so every program including it is a changed program to the
    // hot reload's include watch and a new key to the caches. A failed scan is returned and the previous set
    // stays: a half-saved manifest must not unload every surface of a live editor.
    Common::BoolResultStr ReloadShaderRootShadingModels();

    // `SHADING_MODEL_INDEX_<UPPER_SNAKE_NAME>` — the define the generated include gives a model, and the one a
    // surface cell's DESERT_SHADING_MODEL_INDEX names. "DefaultLit" -> SHADING_MODEL_INDEX_DEFAULT_LIT.
    std::string ShadingModelIndexDefine( std::string_view modelName );
} // namespace Desert::Core::ShadingModels
