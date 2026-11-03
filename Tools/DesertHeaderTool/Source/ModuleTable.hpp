#pragma once

// The engine's module table (cmake/DesertModuleTable.cmake) as C++ reads it: <build>/DesertModules.json, which
// configure writes from the table's rows together with the module CMake placed every legacy file into, so the
// rows are written once and the build's placement is there to check this reader's std::regex placement against.
// Read by DesertHeaderTool (one RegisterReflection_<Module>() per module) and by the ModuleBoundary suite.

#include <Common/Core/ResultStr.hpp>

#include <filesystem>
#include <map>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::HeaderTool
{
    struct ModuleInfo
    {
        std::string              Name;
        std::string              Folder; // repo-relative, '/'-separated: the module's home
        std::vector<std::string> Deps;   // direct dependencies
    };

    struct ModulePlacement
    {
        std::string Module;
        std::string Pattern; // ECMAScript, searched in the path relative to the legacy root
        std::regex  Regex;
    };

    class ModuleTable
    {
    public:
        // Reads the file and validates it the way configure does: a dependency on an unknown or later module, a
        // duplicate name, a placement row naming an unknown module or a pattern std::regex refuses is the error.
        static Common::ResultStr<ModuleTable> Load( const std::filesystem::path& tableFile );

        [[nodiscard]] const std::vector<ModuleInfo>& Modules() const
        {
            return m_Modules;
        }
        [[nodiscard]] const std::vector<ModulePlacement>& Placement() const
        {
            return m_Placement;
        }
        [[nodiscard]] const std::string& LegacyRoot() const
        {
            return m_LegacyRoot;
        }
        // The repository the table describes (DESERT_ROOT at configure time).
        [[nodiscard]] const std::filesystem::path& RepoRoot() const
        {
            return m_RepoRoot;
        }
        // Legacy-relative path -> the module CMake placed it into, for every file the build compiles from the
        // legacy root; and module -> its table library, for each module with legacy files.
        [[nodiscard]] const std::map<std::string, std::string>& BuildPlacement() const
        {
            return m_BuildPlacement;
        }
        [[nodiscard]] const std::map<std::string, std::string>& LegacyLibraries() const
        {
            return m_LegacyLibraries;
        }

        [[nodiscard]] const ModuleInfo* Find( std::string_view name ) const;
        // Every module `name` may include (direct dependencies, transitively); not `name` itself.
        [[nodiscard]] std::set<std::string> Closure( std::string_view name ) const;

        // The module of a repo-relative, '/'-separated path: the module whose Folder holds it, else the first
        // placement row matching it when it lives under the legacy root; empty when it belongs to no module.
        [[nodiscard]] std::string ModuleOf( std::string_view repoRelative ) const;
        // The placement rows alone, for a path already relative to the legacy root ("Engine/ECS/Scene.hpp").
        [[nodiscard]] std::string ModuleOfLegacy( std::string_view legacyRelative ) const;

    private:
        std::vector<ModuleInfo>      m_Modules;
        std::vector<ModulePlacement> m_Placement;
        std::string                        m_LegacyRoot;
        std::filesystem::path              m_RepoRoot;
        std::map<std::string, std::string> m_BuildPlacement;
        std::map<std::string, std::string> m_LegacyLibraries;
    };
} // namespace Desert::HeaderTool
