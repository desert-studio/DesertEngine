#include <Engine/Libraries/ModularCharacterLibrary.hpp>

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/Mesh/SkinnedMeshAsset.hpp>
#include <Engine/Assets/Mesh/StaticMeshAsset.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/Core/WorldContext.hpp>
#include <Engine/ECS/ModularCharacter.hpp>
#include <Engine/Libraries/ScriptWorld.hpp>

#include <cstdint>

namespace Desert::Libraries
{
    namespace
    {
        // The running world, when @p body lives in it.
        Common::ResultStr<Core::Scene*> WorldOf( const ECS::Entity& body, const char* call )
        {
            auto world = ScriptWorld( "ModularCharacter", call );
            if ( !world )
                return world;
            if ( !body || body.GetRegistry() != &world.GetValue()->GetRegistry() )
                return Common::MakeFormattedError<Core::Scene*>(
                     "ModularCharacter.{}: the body is not an entity of the running world", call );
            return world;
        }

        template <class TAsset>
        Common::ResultStr<Common::AssetHandle> MeshByPath( const char* call, const std::string& path )
        {
            const Core::WorldContext* context = Core::WorldContext::Current();
            if ( context == nullptr || context->Assets == nullptr )
                return Common::MakeFormattedError<Common::AssetHandle>(
                     "ModularCharacter.{}: no AssetManager runs this call", call );
            auto asset = context->Assets->FindByPath<TAsset>( path );
            if ( !asset )
                asset = context->Assets->CreateAsset<TAsset>( path );
            if ( !asset )
                return Common::MakeFormattedError<Common::AssetHandle>( "ModularCharacter.{}: no mesh at '{}'",
                                                                        call, path );
            return Common::MakeSuccess(
                 Common::AssetHandle( static_cast<uint64_t>( asset->GetMetadata().Handle ) ) );
        }

        Common::ResultStr<uint32_t> IdOf( int id, const char* call )
        {
            if ( id <= 0 )
                return Common::MakeFormattedError<uint32_t>( "ModularCharacter.{}: {} is not an id", call, id );
            return Common::MakeSuccess( static_cast<uint32_t>( id ) );
        }

        template <class TId>
        Common::ResultStr<int> AsNumber( const Common::ResultStr<TId>& result )
        {
            if ( !result )
                return Common::MakeError<int>( result.GetError() );
            return Common::MakeSuccess( static_cast<int>( result.GetValue().Value ) );
        }
    } // namespace

    Common::ResultStr<int> ModularCharacterLibrary::AttachFollowerMesh( ECS::Entity        body,
                                                                        const std::string& skinnedMesh )
    {
        auto world = WorldOf( body, "attachFollowerMesh" );
        if ( !world )
            return Common::MakeError<int>( world.GetError() );
        auto mesh = MeshByPath<Assets::SkinnedMeshAsset>( "attachFollowerMesh", skinnedMesh );
        if ( !mesh )
            return Common::MakeError<int>( mesh.GetError() );
        ECS::ModularCharacter         view( *world.GetValue(), body.GetHandle() );
        Animation::IModularCharacter& character = view;
        return AsNumber( character.AttachFollowerMesh( mesh.GetValue() ) );
    }

    Common::BoolResultStr ModularCharacterLibrary::DetachFollowerMesh( ECS::Entity body, int follower )
    {
        auto world = WorldOf( body, "detachFollowerMesh" );
        auto id    = IdOf( follower, "detachFollowerMesh" );
        if ( !world )
            return Common::MakeError<bool>( world.GetError() );
        if ( !id )
            return Common::MakeError<bool>( id.GetError() );
        ECS::ModularCharacter         view( *world.GetValue(), body.GetHandle() );
        Animation::IModularCharacter& character = view;
        return character.DetachFollowerMesh( Animation::FollowerMeshId{ id.GetValue() } );
    }

    Common::ResultStr<int>
    ModularCharacterLibrary::AttachToSocket( ECS::Entity body, const std::string& staticMesh,
                                             const std::string& socket, const glm::vec3& translation,
                                             const glm::vec3& rotation, const glm::vec3& scale )
    {
        auto world = WorldOf( body, "attachToSocket" );
        if ( !world )
            return Common::MakeError<int>( world.GetError() );
        auto mesh = MeshByPath<Assets::StaticMeshAsset>( "attachToSocket", staticMesh );
        if ( !mesh )
            return Common::MakeError<int>( mesh.GetError() );
        Animation::SocketAttachmentDesc desc;
        desc.Mesh              = mesh.GetValue();
        desc.SocketName        = socket;
        desc.OffsetTranslation = translation;
        desc.OffsetRotation    = rotation;
        desc.OffsetScale       = scale;
        ECS::ModularCharacter         view( *world.GetValue(), body.GetHandle() );
        Animation::IModularCharacter& character = view;
        return AsNumber( character.AttachToSocket( desc ) );
    }

    Common::BoolResultStr ModularCharacterLibrary::DetachFromSocket( ECS::Entity body, int attachment )
    {
        auto world = WorldOf( body, "detachFromSocket" );
        auto id    = IdOf( attachment, "detachFromSocket" );
        if ( !world )
            return Common::MakeError<bool>( world.GetError() );
        if ( !id )
            return Common::MakeError<bool>( id.GetError() );
        ECS::ModularCharacter         view( *world.GetValue(), body.GetHandle() );
        Animation::IModularCharacter& character = view;
        return character.DetachFromSocket( Animation::SocketAttachmentId{ id.GetValue() } );
    }

    Common::BoolResultStr ModularCharacterLibrary::SetBodySectionVisible( ECS::Entity body, int section,
                                                                          bool visible )
    {
        auto world = WorldOf( body, "setBodySectionVisible" );
        if ( !world )
            return Common::MakeError<bool>( world.GetError() );
        if ( section < 0 )
            return Common::MakeFormattedError<bool>( "ModularCharacter.setBodySectionVisible: section {}",
                                                     section );
        ECS::ModularCharacter         view( *world.GetValue(), body.GetHandle() );
        Animation::IModularCharacter& character = view;
        return character.SetBodySectionVisible( static_cast<uint32_t>( section ), visible );
    }

    Common::ResultStr<int> ModularCharacterLibrary::CreateCloth( ECS::Entity body, int follower,
                                                                 const std::string& clothAsset,
                                                                 const std::string& backend )
    {
        auto world = WorldOf( body, "createCloth" );
        auto id    = IdOf( follower, "createCloth" );
        if ( !world )
            return Common::MakeError<int>( world.GetError() );
        if ( !id )
            return Common::MakeError<int>( id.GetError() );
        ECS::ModularCharacter         view( *world.GetValue(), body.GetHandle() );
        Animation::IModularCharacter& character = view;
        return AsNumber( character.CreateCloth( Animation::FollowerMeshId{ id.GetValue() },
                                                Common::AssetHandle::FromCookedPath( clothAsset ), backend ) );
    }

    Common::BoolResultStr ModularCharacterLibrary::DestroyCloth( ECS::Entity body, int cloth )
    {
        auto world = WorldOf( body, "destroyCloth" );
        auto id    = IdOf( cloth, "destroyCloth" );
        if ( !world )
            return Common::MakeError<bool>( world.GetError() );
        if ( !id )
            return Common::MakeError<bool>( id.GetError() );
        ECS::ModularCharacter         view( *world.GetValue(), body.GetHandle() );
        Animation::IModularCharacter& character = view;
        return character.DestroyCloth( Animation::ClothInstanceId{ id.GetValue() } );
    }
} // namespace Desert::Libraries
