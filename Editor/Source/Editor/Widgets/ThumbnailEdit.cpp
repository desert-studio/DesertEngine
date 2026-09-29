#include "ThumbnailEdit.hpp"

#include <Editor/Core/CommandHistory.hpp>
#include <Editor/Import/CookPaths.hpp>
#include <Editor/Import/ImportedMeshAsset.hpp>
#include <Editor/Widgets/ThumbnailFreshness.hpp>
#include <Engine/Assets/MaterialFormat.hpp>

#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

namespace Desert::Editor::ThumbnailEdit
{
    namespace
    {
        bool IsMaterial( const std::filesystem::path& asset )
        {
            return asset.extension() == ".demat";
        }

        // The file a mesh's orbit is keyed by (MeshThumbnailOrbit): the browser lists the source or the .stmesh.
        std::filesystem::path MeshFile( const std::filesystem::path& asset )
        {
            return ThumbnailFreshness::MeshFreshnessSource( CookPaths::MeshAsset( asset ) );
        }

        Common::ResultStr<Assets::ThumbnailInfo> ReadMaterialInfo( const std::filesystem::path& file )
        {
            std::ifstream in( file, std::ios::binary );
            if ( !in )
                return Common::MakeFormattedError<Assets::ThumbnailInfo>( "'{}' cannot be opened",
                                                                          file.generic_string() );
            const std::string text( ( std::istreambuf_iterator<char>( in ) ), std::istreambuf_iterator<char>() );
            const auto        parsed = Assets::ParseMaterialJson( file.generic_string(), text );
            if ( !parsed )
                return Common::MakeError<Assets::ThumbnailInfo>( parsed.GetError() );
            return Common::MakeSuccess( parsed.GetValue().ThumbnailOrDefault() );
        }

        class OrbitCommand final : public ICommand
        {
        public:
            OrbitCommand( std::filesystem::path asset, Assets::ThumbnailOrbit before,
                          Assets::ThumbnailOrbit after )
                 : m_Asset( std::move( asset ) ), m_Before( before ), m_After( after )
            {
            }
            bool Undo() override
            {
                return Apply( m_Before );
            }
            bool Redo() override
            {
                return Apply( m_After );
            }
            std::string GetLabel() const override
            {
                return std::format( "Edit Thumbnail: {}", m_Asset.filename().string() );
            }

        private:
            bool Apply( const Assets::ThumbnailOrbit& orbit ) const
            {
                const auto written = WriteOrbit( m_Asset, orbit );
                return static_cast<bool>( written );
            }

            std::filesystem::path  m_Asset;
            Assets::ThumbnailOrbit m_Before;
            Assets::ThumbnailOrbit m_After;
        };
    } // namespace

    Common::ResultStr<Assets::ThumbnailOrbit> ReadOrbit( const std::filesystem::path& asset )
    {
        if ( !IsMaterial( asset ) )
            return MeshThumbnailOrbit( MeshFile( asset ) );
        const auto info = ReadMaterialInfo( asset );
        if ( !info )
            return Common::MakeError<Assets::ThumbnailOrbit>( info.GetError() );
        return Common::MakeSuccess( info.GetValue().Orbit );
    }

    Common::BoolResultStr WriteOrbit( const std::filesystem::path& asset, const Assets::ThumbnailOrbit& orbit )
    {
        if ( !Assets::IsValidThumbnailOrbit( orbit ) )
            return Common::MakeFormattedError<bool>(
                 "'{}': the thumbnail orbit is not finite or zooms to -1 or in", asset.generic_string() );
        if ( !IsMaterial( asset ) )
            return SetMeshThumbnailOrbit( MeshFile( asset ), orbit );
        auto info = ReadMaterialInfo( asset );
        if ( !info )
            return Common::MakeError<bool>( info.GetError() );
        Assets::ThumbnailInfo next = info.GetValue();
        next.Orbit                 = orbit;
        return Assets::SetMaterialFileThumbnail( asset, next );
    }

    Common::BoolResultStr EditOrbit( const std::filesystem::path& asset, const Assets::ThumbnailOrbit& orbit )
    {
        const auto before = ReadOrbit( asset );
        if ( !before )
            return Common::MakeError<bool>( before.GetError() );
        if ( before.GetValue() == orbit )
            return Common::MakeSuccess( true );
        if ( auto written = WriteOrbit( asset, orbit ); !written )
            return written;
        CommandHistory::Get().PushCommand( std::make_unique<OrbitCommand>( asset, before.GetValue(), orbit ) );
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr EditOrbitStep( const std::filesystem::path& asset, OrbitStep step )
    {
        const auto stated = ReadOrbit( asset );
        if ( !stated )
            return Common::MakeError<bool>( std::format( "Edit Thumbnail: {}", stated.GetError() ) );
        return EditOrbit( asset, Stepped( stated.GetValue(), step ) );
    }
} // namespace Desert::Editor::ThumbnailEdit
