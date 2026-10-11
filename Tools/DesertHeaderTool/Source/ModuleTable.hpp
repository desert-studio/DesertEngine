#pragma once

// The engine's module table (BuildScripts/DesertModules.lua) as C++ reads it: the same file premake
// validates, executed with the Lua VM so the two readers can never parse it differently. Read by
// DesertHeaderTool (one RegisterReflection_<Module>() per module) and by the ModuleBoundary suite.

#include <Common/Core/ResultStr.hpp>

#include <filesystem>
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
        // Runs the file, then its Validate(): a dependency on an unknown or later module, a duplicate name or a
        // placement row naming an unknown module is the error, with the table's own message.
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
        std::string                  m_LegacyRoot;
    };
} // namespace Desert::HeaderTool
