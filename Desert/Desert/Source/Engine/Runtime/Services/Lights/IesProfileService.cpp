#include "IesProfileService.hpp"

#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Core/Formats/ImageFormat.hpp>
#include <Engine/Graphic/Image.hpp>

#include <Common/Core/Logger.hpp>
#include <Common/Utilities/FileSystem.hpp>
#include <Common/Utilities/VFS.hpp>

#include <filesystem>

namespace Desert::Runtime
{
    namespace
    {
        Common::ResultStr<Graphic::IesProfileGrid> ReadProfile( std::string_view reference )
        {
            const std::filesystem::path file =
                 Assets::ContentRegistry::FileToOpen( std::filesystem::path( reference ) );
            std::string text;
            if ( const auto packed =
                      Common::Utils::VFS::Exists( file ) ? Common::Utils::VFS::ReadFile( file ) : std::nullopt;
                 packed.has_value() )
            {
                text = packed.value();
            }
            else if ( auto read = Common::Utils::FileSystem::ReadFileContent( file ); read )
            {
                text = read.ExtractValue();
            }
            if ( text.empty() )
                return Common::MakeFormattedError<Graphic::IesProfileGrid>( "'{}' is empty or could not be opened",
                                                                            file.string() );
            auto parsed = Graphic::ParseIesProfile( text );
            if ( !parsed )
                return Common::MakeError<Graphic::IesProfileGrid>( parsed.GetError() );
            return Graphic::ResampleIesProfile( parsed.GetValue() );
        }
    } // namespace

    int32_t IesProfileService::AcquireRow( std::string_view path )
    {
        if ( path.empty() )
            return kNoProfile;

        const std::lock_guard<std::mutex> lock( m_Mutex );
        if ( const auto known = m_ByPath.find( path ); known != m_ByPath.end() )
            return known->second.Row;

        Entry entry;
        if ( auto grid = ReadProfile( path ); grid )
        {
            const Graphic::IesProfileGrid& profile = grid.GetValue();
            entry.Row                              = static_cast<int32_t>( m_Profiles * Graphic::kIesProfileRows );
            m_Texels.insert( m_Texels.end(), profile.Candela.begin(), profile.Candela.end() );
            ++m_Profiles;
            m_Dirty = true;
            LOG_INFO( "[IES] Profile '{}' -> atlas row {} (peak {} cd).", std::string( path ), entry.Row,
                      profile.PeakCandela );
        }
        else
        {
            LOG_ERROR( "[IES] Profile '{}' not usable, the light emits unshaped: {}", std::string( path ),
                       grid.GetError() );
        }
        m_ByPath.emplace( std::string( path ), entry );
        return entry.Row;
    }

    int32_t IesProfileService::AtlasTexture()
    {
        const std::lock_guard<std::mutex> lock( m_Mutex );
        if ( m_Dirty )
        {
            m_Dirty                                         = false;
            const Core::Formats::Image2DSpecification spec  = { .Tag        = "IesProfileAtlas",
                                                                .Width      = Graphic::kIesProfileColumns,
                                                                .Height     = m_Profiles * Graphic::kIesProfileRows,
                                                                .Format     = Core::Formats::ImageFormat::R32F,
                                                                .Data       = m_Texels,
                                                                .Usage      = Core::Formats::Image2DUsage::Image2D,
                                                                .Properties = Core::Formats::Sample };
            std::shared_ptr<Graphic::Image2D>         atlas = Graphic::Image2D::Create( spec );
            if ( !atlas || !atlas->GetBindlessSlot().IsValid() )
            {
                LOG_ERROR( "[IES] The profile atlas ({} profiles) was not created; IES lights emit unshaped.",
                           m_Profiles );
            }
            else
            {
                if ( m_Atlas )
                    m_Replaced.push_back( std::move( m_Atlas ) );
                m_Atlas = std::move( atlas );
            }
        }
        return m_Atlas ? static_cast<int32_t>( m_Atlas->GetBindlessSlot().Index ) : 0;
    }

    void IesProfileService::Clear()
    {
        const std::lock_guard<std::mutex> lock( m_Mutex );
        m_ByPath.clear();
        m_Texels.clear();
        m_Profiles = 0;
        m_Dirty    = false;
        m_Atlas.reset();
        m_Replaced.clear();
    }
} // namespace Desert::Runtime
