#pragma once

// SP-MODEL CONTRACT. The start's registry gather with a watcher: the work of ContentRegistry::Gather (same result,
// same refusals) reporting into @p sink -- Expect( files the walk listed ) once after the directory walk and
// before the first cache check or header read, then one item per file (its file name) around that file's cache
// check or header read. ContentRegistry::Gather( refused, meshes ) is this with a null sink. Defined in
// ContentRegistryCook.cpp.

#include <Common/Core/ResultStr.hpp>

#include <cstddef>
#include <string>
#include <vector>

namespace Desert::Core
{
    class WorkSink;
}

namespace Desert::Assets::ContentRegistry
{
    [[nodiscard]] Common::ResultStr<std::size_t> GatherWatched( std::vector<std::string>* refused,
                                                                std::vector<std::string>* meshesWithoutHeaderBounds,
                                                                Core::WorkSink*           sink );
} // namespace Desert::Assets::ContentRegistry
