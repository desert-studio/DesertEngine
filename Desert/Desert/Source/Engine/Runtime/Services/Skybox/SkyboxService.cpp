#include "SkyboxService.hpp"

#include <Common/Content/TextAssetHeader.hpp>
#include <Common/Core/Logger.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <chrono>

namespace Desert::Runtime
{
    void SkyboxService::Request( const std::shared_ptr<Assets::SkyboxAsset>& skyboxAsset )
    {
        if ( !skyboxAsset || !skyboxAsset->GetMetadata().IsValid() )
        {
            LOG_ERROR( "[Skybox] a skybox request carried no valid asset; nothing is loaded." );
            return;
        }
        const auto  handle = skyboxAsset->GetMetadata().Handle;
        const auto& path   = skyboxAsset->GetMetadata().Filepath;
        if ( m_Skyboxes.contains( handle ) || m_Pending.contains( handle ) )
            return;

        // Decision V3: a missing file is an error that names it, never a quiet empty sky.
        if ( !Common::Utils::FileSystem::Exists( path ) )
        {
            LOG_ERROR( "[Skybox] '{}' (GUID {}, handle {}) is not on disk and not in a mounted pak: the scene "
                       "has no environment from it.",
                       path.string(), Common::Content::AssetGuidToText( skyboxAsset->Guid() ),
                       static_cast<uint64_t>( handle ) );
            return;
        }

        auto request = Assets::AsyncAssetLoader::Get().Request(
             skyboxAsset,
             [this, handle]( const Assets::Asset<Assets::AssetBase>& asset, const Assets::LoadOutcome outcome,
                             const std::string& error )
             {
                 m_Pending.erase( handle );
                 const auto skybox = std::dynamic_pointer_cast<Assets::SkyboxAsset>( asset );
                 if ( outcome != Assets::LoadOutcome::Loaded || !skybox )
                 {
                     LOG_ERROR( "[Skybox] '{}' did not load, so the scene has no environment from it: {}",
                                asset ? asset->GetMetadata().Filepath.string() : std::string( "<null>" ), error );
                     return;
                 }
                 auto material = std::make_shared<Graphic::MaterialSkybox>( skybox );
                 if ( !material->IsConvolving() )
                 {
                     material->SettleConvolution();
                     m_Skyboxes[handle] = std::move( material );
                     return;
                 }

                 // A CACHE MISS: the cubes exist but the GPU is still convolving them. The skybox stays
                 // pending — and the loader keeps counting it, which is what ContentGate reads — until the
                 // batch's fence, and only then is the material handed out to be sampled.
                 const auto submittedAt = std::chrono::steady_clock::now();
                 auto       awaited     = Assets::AsyncAssetLoader::Get().Await(
                      handle, [material]() { return !material->IsConvolving(); },
                      [this, handle, material, submittedAt]()
                      {
                          m_Pending.erase( handle );
                          material->SettleConvolution();
                          LOG_INFO(
                               "[Skybox] '{}' finished convolving on the GPU; seen {:.1f} ms after its submit.",
                               material->GetEnvironment().Filepath.string(),
                               std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() -
                                                                                    submittedAt )
                                    .count() );
                          m_Skyboxes[handle] = material;
                      },
                      [this, handle]() { m_Pending.erase( handle ); } );
                 if ( awaited.IsValid() )
                     m_Pending.emplace( handle, std::move( awaited ) );
             },
             [this, handle]() { m_Pending.erase( handle ); } );
        if ( request.IsValid() )
            m_Pending.emplace( handle, std::move( request ) );
    }

    bool SkyboxService::IsPending( const Assets::AssetHandle& handle ) const
    {
        return m_Pending.contains( handle );
    }

    std::shared_ptr<Desert::Graphic::MaterialSkybox> SkyboxService::Get( const Assets::AssetHandle& handle ) const
    {
        auto it = m_Skyboxes.find( handle );
        return ( it != m_Skyboxes.end() ) ? it->second : nullptr;
    }

    void SkyboxService::Clear()
    {
        // A skybox material owns its cubemap images and descriptor sets; a pending read is cancelled
        // with it, so a completion can never land in a service that has been emptied.
        m_Pending.clear();
        m_Skyboxes.clear();
    }
} // namespace Desert::Runtime
