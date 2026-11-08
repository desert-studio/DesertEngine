#include "IesProfileService.hpp"

#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/IesProfileAsset.hpp>
#include <Engine/Core/Formats/ImageFormat.hpp>
#include <Engine/Graphic/Image.hpp>

#include <Common/Core/Logger.hpp>

#include <string>

namespace Desert::Runtime
{
    namespace
    {
        Common::ResultStr<Graphic::IesProfileGrid> ReadProfile( const std::string& key )
        {
            if ( key.empty() )
                return Common::MakeError<Graphic::IesProfileGrid>(
                     "no content registry row holds the handle (the .dies is missing or not yet discovered)" );
            const auto read = Assets::IesProfileAsset::ReadFile( Common::AssetHandle::PathForStableKey( key ) );
            if ( !read )
                return Common::MakeError<Graphic::IesProfileGrid>( read.GetError() );
            return Graphic::ResampleIesProfile( Assets::Serialization::PhotometryOf( read.GetValue().Data ) );
        }
    } // namespace

    int32_t IesProfileService::AcquireRow( const Assets::AssetHandle profile )
    {
        const uint64_t handle = static_cast<uint64_t>( profile );
        if ( handle == 0 )
            return kNoProfile;

        const std::lock_guard<std::mutex> lock( m_Mutex );
        if ( const auto known = m_ByHandle.find( handle ); known != m_ByHandle.end() )
            return known->second.Row;

        const std::string key = Assets::ContentRegistry::KeyForHandle( handle );
        Entry             entry;
        if ( auto grid = ReadProfile( key ); grid )
        {
            const Graphic::IesProfileGrid& profile = grid.GetValue();
            entry.Row                              = static_cast<int32_t>( m_Profiles * Graphic::kIesProfileRows );
            m_Texels.insert( m_Texels.end(), profile.Candela.begin(), profile.Candela.end() );
            ++m_Profiles;
            m_Dirty = true;
            LOG_INFO( "[IES] Profile '{}' -> atlas row {} (peak {} cd).", key, entry.Row,
                      profile.PeakCandela );
        }
        else
        {
            LOG_ERROR( "[IES] Profile {:#x} ('{}') not usable, the light emits unshaped: {}", handle, key,
                       grid.GetError() );
        }
        m_ByHandle.emplace( handle, entry );
        return entry.Row;
    }

    int32_t IesProfileService::AtlasTexture()
    {
        const std::lock_guard<std::mutex> lock( m_Mutex );
        if ( m_Dirty )
        {
            m_Dirty = false;
            const Core::Formats::Image2DSpecification spec = { .Tag        = "IesProfileAtlas",
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
        m_ByHandle.clear();
        m_Texels.clear();
        m_Profiles = 0;
        m_Dirty    = false;
        m_Atlas.reset();
        m_Replaced.clear();
    }
} // namespace Desert::Runtime
