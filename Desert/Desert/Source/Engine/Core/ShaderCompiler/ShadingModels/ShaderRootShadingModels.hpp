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
// The set is read once per process: editing a .shadingmodel takes effect on the next start.

#include <Engine/Core/ShaderCompiler/ShadingModels/ShadingModelRegistry.hpp>

#include <Common/Core/ResultStr.hpp>

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

    // The set of the current shader root (SHADERDIR_PATH against the current directory). The first call for a
    // root scans it and writes kGeneratedInclude beside the models when its bytes differ; later calls return
    // the same object.
    const Common::ResultStr<LoadedShadingModels>& ShaderRootShadingModels();

    // `SHADING_MODEL_INDEX_<UPPER_SNAKE_NAME>` — the define the generated include gives a model, and the one a
    // surface cell's DESERT_SHADING_MODEL_INDEX names. "DefaultLit" -> SHADING_MODEL_INDEX_DEFAULT_LIT.
    std::string ShadingModelIndexDefine( std::string_view modelName );
} // namespace Desert::Core::ShadingModels
