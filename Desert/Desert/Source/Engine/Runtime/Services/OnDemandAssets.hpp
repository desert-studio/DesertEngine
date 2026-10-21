#pragma once

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/AsyncAssetLoader.hpp>
#include <Engine/Assets/RegistryDiscovery.hpp>

#include <Common/Content/ContentKinds.hpp>
#include <Common/Core/Logger.hpp>

#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

namespace Desert::Runtime
{
    /**
     * @brief The assets a type service reads on demand by handle: created from the registry row on the first ask,
     * read off the frame through the AsyncAssetLoader, and READ AGAIN WHEN AN EVICTION RELEASED THEM.
     *
     * The one rule the type services (LandscapeGrassTypeService, LandscapeLayerInfoService, FoliageTypeService)
     * share instead of each keeping its own copy. It is UE's soft reference: TSoftObjectPtr resolves the object
     * again on the next use after a garbage collection dropped it; here an eviction sweep Unload()s the shell this
     * set holds (AssetEviction contract: build on miss), leaving it not ready and NOT broken, and the next ask
     * re-reads it into the SAME shell, so every holder sees the body come back. A broken shell (a hot reload or a
     * save whose read failed) stays as it is: only a good read clears it, a reader does not re-read it every
     * frame. A handle that cannot be created or read is Failed for the session and said once in the log.
     */
    template <typename TAsset>
    class OnDemandAssets
    {
    public:
        enum class State
        {
            Pending, // requested, not read yet
            Ready,
            Failed // ErrorOf() says why; said once in the log
        };

        /// @p noun names one asset in the log ("[Landscape] Grass type"); @p consequence says what a failure costs
        /// the scene; @p noAsset is the failure of handle 0 (a slot naming nothing).
        OnDemandAssets( const Common::Content::ContentKind kind, std::string noun, std::string consequence,
                        std::string noAsset )
             : m_Kind( kind ), m_Noun( std::move( noun ) ), m_Consequence( std::move( consequence ) ),
               m_NoAsset( std::move( noAsset ) )
        {
        }

        void BindAssetManager( const std::weak_ptr<Assets::AssetManager>& assets )
        {
            m_Assets = assets;
        }

        [[nodiscard]] std::shared_ptr<Assets::AssetManager> Manager() const
        {
            return m_Assets.lock();
        }

        /// Requests the handle when nobody has yet (or re-reads it when it was released), then answers where it
        /// stands.
        State StateOf( const Assets::AssetHandle& handle )
        {
            Request( handle );
            if ( m_Ready.contains( handle ) )
                return State::Ready;
            if ( m_Failed.contains( handle ) )
                return State::Failed;
            return State::Pending;
        }

        /// The held asset when Ready (it may be broken: a failed hot reload; IsReadyForUse() says); nullptr
        /// otherwise.
        Assets::Asset<TAsset> AssetOf( const Assets::AssetHandle& handle )
        {
            if ( StateOf( handle ) != State::Ready )
                return nullptr;
            return m_Ready.at( handle );
        }

        [[nodiscard]] std::string ErrorOf( const Assets::AssetHandle& handle ) const
        {
            const auto found = m_Failed.find( handle );
            return found == m_Failed.end() ? std::string() : found->second;
        }

        /// Marks @p handle Failed for the session, saying why once.
        void Fail( const Assets::AssetHandle& handle, const std::string& why )
        {
            LOG_ERROR( "{} {} cannot be used; {}: {}", m_Noun, static_cast<uint64_t>( handle ), m_Consequence,
                       why );
            m_Ready.erase( handle );
            m_Failed[handle] = why;
        }

        void Clear()
        {
            m_Requests.clear(); // released: no delegate fires into a cleared set
            m_Ready.clear();
            m_Failed.clear();
        }

    private:
        void Request( const Assets::AssetHandle& handle )
        {
            if ( m_Failed.contains( handle ) || m_Requests.contains( handle ) )
                return;
            if ( const auto ready = m_Ready.find( handle ); ready != m_Ready.end() )
            {
                const auto& held = ready->second;
                if ( held->IsReadyForUse() || held->IsBroken() )
                    return;
                LOG_INFO( "{} {} ('{}') was released; reading it again.", m_Noun, static_cast<uint64_t>( handle ),
                          held->GetMetadata().Filepath.generic_string() );
                const Assets::Asset<TAsset> shell = held;
                m_Ready.erase( ready );
                Read( handle, shell );
                return;
            }

            if ( handle == 0 )
            {
                Fail( handle, m_NoAsset );
                return;
            }
            auto created = Assets::CreateFromRegistryRow<TAsset>( m_Assets, handle, m_Kind );
            if ( !created )
            {
                Fail( handle, created.GetError() );
                return;
            }
            if ( created.GetValue()->IsReadyForUse() )
            {
                m_Ready[handle] = created.GetValue();
                return;
            }
            Read( handle, created.GetValue() );
        }

        /// Reads @p shell through the AsyncAssetLoader: the first read, or a re-read after an eviction.
        void Read( const Assets::AssetHandle& handle, const Assets::Asset<TAsset>& shell )
        {
            m_Requests[handle] = Assets::AsyncAssetLoader::Get().Request(
                 shell,
                 [this, handle]( const Assets::Asset<Assets::AssetBase>& loaded, const Assets::LoadOutcome outcome,
                                 const std::string& error )
                 {
                     m_Requests.erase( handle );
                     if ( outcome == Assets::LoadOutcome::Loaded )
                         m_Ready[handle] = std::static_pointer_cast<TAsset>( loaded );
                     else
                         Fail( handle, error );
                 },
                 [this, handle] { m_Requests.erase( handle ); } );
        }

        Common::Content::ContentKind                                   m_Kind;
        std::string                                                    m_Noun;
        std::string                                                    m_Consequence;
        std::string                                                    m_NoAsset;
        std::weak_ptr<Assets::AssetManager>                            m_Assets;
        std::unordered_map<Assets::AssetHandle, Assets::LoadRequest>   m_Requests;
        std::unordered_map<Assets::AssetHandle, Assets::Asset<TAsset>> m_Ready;
        std::unordered_map<Assets::AssetHandle, std::string>           m_Failed;
    };
} // namespace Desert::Runtime
