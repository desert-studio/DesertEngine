#pragma once

#include <Engine/Assets/LandscapeLayerInfoAsset.hpp>
#include <Engine/Runtime/Services/OnDemandAssets.hpp>

#include <memory>
#include <string>
#include <unordered_map>

namespace Desert::Runtime
{
    /**
     * @brief The `.delayerinfo` assets landscapes name (LS-12b), loaded on demand.
     *
     * A landscape root holds its target layers as handles (ECS::LandscapeComponent::Layers). The first reader
     * that asks for a handle creates the asset from its registry row and requests the read through the
     * AsyncAssetLoader; until it lands the layer is Pending. The service holds the ASSET, not a copy of its
     * data, so an edit the landscape panel saves and re-reads into the same asset is seen by every reader on
     * the next ask. A layer an eviction sweep released is read again into the same shell on the next ask
     * (OnDemandAssets, the rule every type service shares).
     */
    class LandscapeLayerInfoService
    {
    public:
        using State = OnDemandAssets<Assets::LandscapeLayerInfoAsset>::State;

        void BindAssetManager( const std::weak_ptr<Assets::AssetManager>& assets );

        /// The layer's data when it is read; nullptr while it is pending or when it failed (StateOf says which).
        const Assets::Serialization::LandscapeLayerInfoData* Get( const Assets::AssetHandle& handle );

        /// The asset itself when it is read (what the panel saves an edit into); nullptr otherwise.
        Assets::Asset<Assets::LandscapeLayerInfoAsset> AssetOf( const Assets::AssetHandle& handle );

        /// Requests the handle when nobody has yet, then answers where it stands.
        State StateOf( const Assets::AssetHandle& handle );

        /// Why a Failed handle failed; empty for any other state.
        std::string ErrorOf( const Assets::AssetHandle& handle ) const;

        /// Writes @p data over the layer's file and re-reads it into the same asset through the loader, so
        /// every reader sees the edit. Refuses a handle that is not Ready.
        NO_DISCARD Common::BoolResultStr Save( const Assets::AssetHandle&                           handle,
                                               const Assets::Serialization::LandscapeLayerInfoData& data );

        void Clear();

    private:
        OnDemandAssets<Assets::LandscapeLayerInfoAsset> m_Layers{
             Common::Content::ContentKind::LandscapeLayerInfo, "[Landscape] Layer info",
             "the landscape's weight planes of that layer are neither drawn nor painted",
             "the layer slot names no asset" };
    };
} // namespace Desert::Runtime
