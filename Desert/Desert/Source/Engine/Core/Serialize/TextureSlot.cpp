#include <Engine/Core/Serialize/TextureSlot.hpp>

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/TextureAsset.hpp>

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/Constants.hpp>
#include <Common/Core/Logger.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <filesystem>

namespace Desert::Core::Serialize
{
    std::string TextureSlotToPath( uint64_t handle )
    {
        if ( handle == 0 )
            return "";

        // THE MANAGER PARAMETER IS GONE, and its absence is what T2.4 bought here. This used to be
        // `manager.FindByHandle<TextureAsset>( h )->GetMetadata().Filepath` put back through
        // `StableKeyForPath` — i.e. it recovered the key by finding the asset, and an asset is only
        // findable if something REGISTERED it, which until this slice meant the boot's directory walk.
        // The registry answers the same question from a file, so a texture nothing has loaded still
        // names itself, and the answer is the row's own key rather than a reconstruction of it.
        // NOT `const`: this string is returned, and a const local cannot be moved out of — the copy
        // would be silent.
        std::string key = Assets::ContentRegistry::KeyForHandle( handle );
        if ( key.empty() )
        {
            LOG_ERROR( "[Textures] Handle {0} is set on a texture slot and the cooked asset registry has "
                       "no row for it, so the slot is being written out EMPTY and the reference is lost. "
                       "Cooked textures live under '{1}'; if the file is there, the registry is stale — "
                       "run 'AssetRegistryTool cook'.",
                       handle, Common::Constants::Path::TEXTURE_PATH_COOKED.string() );
            return "";
        }

        return key;
    }

    uint64_t TextureSlotFromPath( Assets::AssetManager& manager, const std::string& stored )
    {
        if ( stored.empty() )
            return 0;

        const std::filesystem::path full = Common::AssetHandle::PathForStableKey( stored );

        auto asset = manager.FindByPath<Assets::TextureAsset>( full );

        // Asked BEFORE CreateAsset on purpose: a missing texture then produces the log line below —
        // which names the search roots a texture is expected under — instead of registering an asset
        // shell whose load failed with only the raw path in the log. (Historically this check was
        // load-bearing in a harder way: ReadFileContent used to abort the process on a missing file.)
        if ( !asset && Common::Utils::FileSystem::Exists( full ) )
            asset = manager.CreateAsset<Assets::TextureAsset>( Assets::AssetPriority::High, full );

        if ( !asset )
        {
            LOG_ERROR( "[Textures] Texture '{0}' named by the scene did not resolve (it expands to '{1}'; "
                       "cooked textures live under '{2}', content textures under '{3}'). The slot stays "
                       "unset.",
                       stored, full.string(), Common::Constants::Path::TEXTURE_PATH_COOKED.string(),
                       Common::Constants::Path::TEXTUREDIR_PATH.string() );
            return 0;
        }

        return static_cast<uint64_t>( asset->GetMetadata().Handle );
    }
} // namespace Desert::Core::Serialize
