#include "TextureDnD.hpp"
#include "ImportManager.hpp"
#include "TextureImporter.hpp"

#include <Engine/Assets/TextureAsset.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <filesystem>

namespace Desert::Editor::TextureDnD
{
    namespace
    {
        // One shared importer for all drag-drop / Import-button cooks (its cache dedups by source path).
        ImportManager& Importer()
        {
            static ImportManager s_Importer;
            return s_Importer;
        }

        // A registered texture is identified by its `.detex` asset path -- the value stored in
        // AssetMetadata::Filepath. A dropped SOURCE image maps to the asset beside it; paths are compared
        // lexically-normalized so separator differences don't cause false misses.
        Assets::AssetHandle FindRegistered( const Assets::AssetManager& mgr, const std::string& sourcePath )
        {
            const auto cooked = TextureImporter::AssetPathFor( sourcePath ).lexically_normal();

            for ( const auto& [handle, tex] :
                  const_cast<Assets::AssetManager&>( mgr ).FindAllByType<Assets::TextureAsset>() )
            {
                if ( !tex )
                    continue;
                if ( std::filesystem::path( tex->GetMetadata().Filepath ).lexically_normal() == cooked )
                {
                    return handle;
                }
            }
            return Common::UUID::Null();
        }
    } // namespace

    Assets::AssetHandle ResolveExisting( const Assets::AssetManager& mgr, const std::string& sourcePath )
    {
        return FindRegistered( mgr, sourcePath );
    }

    Assets::AssetHandle ResolveOrImport( Assets::AssetManager& mgr, const std::string& sourcePath )
    {
        if ( const auto existing = FindRegistered( mgr, sourcePath ); !existing.IsNull() )
        {
            return existing;
        }

        return Importer().ImportAndRegisterTexture( mgr, sourcePath );
    }

    std::shared_ptr<Graphic::Image2D> ResolveImage( const Assets::AssetHandle& handle )
    {
        if ( static_cast<uint64_t>( handle ) == 0 )
            return nullptr;
        auto* tex = Runtime::ResourceRegistry::GetTextureService()->Get( handle );
        if ( !tex )
            return nullptr;
        auto* img = static_cast<Graphic::Image2D*>(
             Runtime::ResourceRegistry::GetImageService()->Resolve( tex->GetImageHandle() ) );
        if ( !img )
            return nullptr;
        // Non-owning: the image is owned by the image service. ImGui descriptor cache keys by VkImageView,
        // so re-wrapping the same image each frame is leak-free.
        return std::shared_ptr<Graphic::Image2D>( img, []( Graphic::Image2D* ) {} );
    }

} // namespace Desert::Editor::TextureDnD
