#include "IesProfileService.hpp"

#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/IesProfileAsset.hpp>
#include <Engine/Core/Formats/ImageFormat.hpp>
#include <Engine/Graphic/Image.hpp>

#include <Common/Core/Logger.hpp>

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>

namespace Desert::Runtime
{
    namespace
    {
        struct ReadResult
        {
            Graphic::IesProfileGrid Grid;
            float                   PeakCandela = 0.0f;
        };

        Common::ResultStr<ReadResult> ReadProfile( const std::string& key )
        {
            if ( key.empty() )
                return Common::MakeError<ReadResult>(
                     "no content registry row holds the handle (the .dies is missing or not yet discovered)" );
            const auto read = Assets::IesProfileAsset::ReadFile( Common::AssetHandle::PathForStableKey( key ) );
            if ( !read )
                return Common::MakeError<ReadResult>( read.GetError() );
            const Assets::Serialization::IesProfileData& data = read.GetValue().Data;
            auto grid = Graphic::ResampleIesProfile( Assets::Serialization::PhotometryOf( data ) );
            if ( !grid )
                return Common::MakeError<ReadResult>( grid.GetError() );
            ReadResult result;
            result.Grid        = std::move( grid.GetValue() );
            result.PeakCandela = Assets::Serialization::IesProfilePeakCandela( data );
            return Common::MakeSuccess( std::move( result ) );
        }
    } // namespace

    IesProfileService::Slot IesProfileService::Acquire( const Assets::AssetHandle profile )
    {
        const uint64_t handle = static_cast<uint64_t>( profile );
        if ( handle == 0 )
            return Slot{};

        const std::lock_guard<std::mutex> lock( m_Mutex );
        if ( const auto known = m_ByHandle.find( handle ); known != m_ByHandle.end() )
            return known->second;

        const std::string key = Assets::ContentRegistry::KeyForHandle( handle );
        Slot              slot;
        if ( auto read = ReadProfile( key ); read )
        {
            const ReadResult& result = read.GetValue();
            slot.Row                 = static_cast<int32_t>( m_Profiles * Graphic::kIesProfileRows );
            slot.PeakCandela         = result.PeakCandela;
            m_Texels.insert( m_Texels.end(), result.Grid.Candela.begin(), result.Grid.Candela.end() );
            ++m_Profiles;
            m_Dirty = true;
            LOG_INFO( "[IES] Profile '{}' -> atlas row {} (peak {} cd).", key, slot.Row, slot.PeakCandela );
        }
        else
        {
            LOG_ERROR( "[IES] Profile {:#x} ('{}') not usable, the light emits unshaped: {}", handle, key,
                       read.GetError() );
        }
        m_ByHandle.emplace( handle, slot );
        return slot;
    }

    void IesProfileService::Reimported( const Assets::AssetHandle profile )
    {
        const uint64_t handle = static_cast<uint64_t>( profile );
        if ( handle == 0 )
            return;

        const std::lock_guard<std::mutex> lock( m_Mutex );
        const auto                        known = m_ByHandle.find( handle );
        if ( known == m_ByHandle.end() )
            return;
        if ( known->second.Row == kNoProfile )
        {
            m_ByHandle.erase( known );
            return;
        }

        const std::string key  = Assets::ContentRegistry::KeyForHandle( handle );
        auto              read = ReadProfile( key );
        if ( !read )
        {
            // The block keeps the profile as last read: the light keeps lighting as it did, the log says why.
            LOG_ERROR( "[IES] Re-imported profile {:#x} ('{}') not usable, the atlas keeps the previous one: {}",
                       handle, key, read.GetError() );
            return;
        }
        const ReadResult& result = read.GetValue();
        const size_t      first  = static_cast<size_t>( known->second.Row ) * Graphic::kIesProfileColumns;
        std::copy( result.Grid.Candela.begin(), result.Grid.Candela.end(),
                   m_Texels.begin() + static_cast<std::ptrdiff_t>( first ) );
        known->second.PeakCandela = result.PeakCandela;
        m_Dirty                   = true;
        LOG_INFO( "[IES] Profile '{}' re-read into atlas row {} (peak {} cd).", key, known->second.Row,
                  result.PeakCandela );
    }

    int32_t IesProfileService::AtlasTexture()
    {
        const std::lock_guard<std::mutex> lock( m_Mutex );
        if ( m_Dirty )
        {
            m_Dirty = false;
            const Core::Formats::Image2DSpecification spec = {
                 .Tag        = "IesProfileAtlas",
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
