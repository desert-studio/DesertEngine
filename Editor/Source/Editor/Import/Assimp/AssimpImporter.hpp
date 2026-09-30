#pragma once

#include "../IAssetImporter.hpp"

namespace Desert::Editor
{
    class AssimpImporter : public IAssetImporter
    {
    public:
        ImportResult Import( const std::filesystem::path& path, ImportManager& manager ) override;
        Common::ResultStr<ImportContentKind> Probe( const std::filesystem::path& path ) override;
    };
} // namespace Desert::Editor