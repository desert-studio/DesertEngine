#pragma once

#include <Engine/Assets/Serialization/SubsurfaceProfile.hpp>
#include <Engine/Graphic/SubsurfaceProfiles/SubsurfaceProfileLayout.hpp>

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Core/ResultStr.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <map>
#include <utility>
#include <vector>

namespace Desert::Graphic
{
    using SubsurfaceProfileRow = std::array<glm::vec4, SubsurfaceProfileLayout::kAtlasWidth>;

    /// UE ComputeTransmissionProfileBurley + the Burley half of UE's FSubsurfaceProfileTexture row encoding
    /// (SubsurfaceProfileCommon.ush:49-58): one atlas row from a profile. The Separable kernel columns are left zero
    /// until the Separable pass lands (the card's REMAINDER); AFIS reads only the Burley columns.
    void EncodeBurleyRow( const Assets::Serialization::SubsurfaceProfileData& profile, SubsurfaceProfileRow& row );

    /**
     * @brief THE PROFILES THE MATERIALS NAME, one atlas row each (SSS-PROFILE): UE FSubsurfaceProfileTexture
     * (SubsurfaceProfile.h:201-300), keyed by the profile asset's GUID instead of allocation order (CONTRACT §2.3):
     * an edit re-encodes its row in place and the row index stays stable for the session.
     */
    class SubsurfaceProfileRegistry
    {
    public:
        static constexpr uint32_t kMaxProfiles  = 255; // UE MAX_SUBSURFACE_PROFILE_COUNT 256, 255 = none
        static constexpr uint32_t kInvalidIndex = 255; // UE SSS_PROFILE_ID_INVALID

        /// The profile's row, allocated on first ask. Past kMaxProfiles it is an error, never a shared row.
        [[nodiscard]] Common::ResultStr<uint32_t> Acquire( const Common::Content::AssetGuid&                    guid,
                                                           const Assets::Serialization::SubsurfaceProfileData& data );
        /// Edit in place: re-encodes the row and marks it dirty. A GUID never acquired is ignored.
        void Update( const Common::Content::AssetGuid& guid, const Assets::Serialization::SubsurfaceProfileData& data );
        /// Frees the row for a later Acquire (the index is not reused within the frame it was released in).
        void Release( const Common::Content::AssetGuid& guid );
        [[nodiscard]] uint32_t IndexOf( const Common::Content::AssetGuid& guid ) const;
        [[nodiscard]] uint32_t Count() const
        {
            return static_cast<uint32_t>( m_ByGuid.size() );
        }
        /// Every row, index-ordered (a released row stays encoded until reused); the atlas uploads these.
        [[nodiscard]] std::span<const SubsurfaceProfileRow> Rows() const
        {
            return m_Rows;
        }
        /// True once since the last call when any row changed: the renderer re-uploads the atlas then.
        [[nodiscard]] bool ConsumeDirty()
        {
            const bool dirty = m_Dirty;
            m_Dirty          = false;
            return dirty;
        }
        void Clear();

    private:
        std::map<std::pair<uint64_t, uint64_t>, uint32_t> m_ByGuid; // {Hi, Lo} of the profile GUID
        std::vector<SubsurfaceProfileRow>                        m_Rows;
        std::vector<uint32_t>                                    m_Free;
        bool                                                     m_Dirty = false;
    };
} // namespace Desert::Graphic
