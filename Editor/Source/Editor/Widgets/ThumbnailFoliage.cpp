#include "ThumbnailFoliage.hpp"

#include <Engine/Assets/Serialization/FoliageType.hpp>

#include <Common/Utilities/FileSystem.hpp>

namespace Desert::Editor::ThumbnailFoliage
{
    Common::ResultStr<std::filesystem::path> MeshSourceOf( const std::string&           defoliageText,
                                                           const std::filesystem::path& assetsRoot )
    {
        const auto parsed = Assets::Serialization::ParseFoliageType( defoliageText );
        if ( !parsed )
            return Common::MakeFormattedError<std::filesystem::path>( "the foliage type does not parse: {}",
                                                                      parsed.GetError() );
        const auto& data = parsed.GetValue();
        if ( data.Kind != Assets::Serialization::FoliageTypeKind::Mesh )
            return Common::MakeError<std::filesystem::path>(
                 "a Prefab foliage type places a prefab and names no mesh, so there is no mesh to photograph" );
        if ( data.Mesh.Path.empty() )
            return Common::MakeError<std::filesystem::path>( "the foliage type names no mesh yet (authored, not "
                                                             "paintable), so there is nothing to photograph" );
        return Common::MakeSuccess( ( assetsRoot / data.Mesh.Path ).lexically_normal() );
    }

    Common::ResultStr<std::filesystem::path> ReadMeshSource( const std::filesystem::path& defoliage,
                                                             const std::filesystem::path& assetsRoot )
    {
        auto text = Common::Utils::FileSystem::ReadFileContent( defoliage );
        if ( !text )
            return Common::MakeFormattedError<std::filesystem::path>(
                 "'{}' could not be read: {}", defoliage.generic_string(), text.GetError() );
        auto source = MeshSourceOf( text.GetValue(), assetsRoot );
        if ( !source )
            return Common::MakeFormattedError<std::filesystem::path>( "'{}': {}", defoliage.generic_string(),
                                                                      source.GetError() );
        return source;
    }
} // namespace Desert::Editor::ThumbnailFoliage
