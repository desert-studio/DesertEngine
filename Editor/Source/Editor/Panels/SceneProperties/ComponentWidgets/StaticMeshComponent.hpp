#pragma once

#include "IComponentWidget.hpp"

#include <Editor/Panels/PropertyEditor/ComponentWidgetRegistry.hpp>
#include <Engine/Geometry/PrimitiveType.hpp>

namespace Desert::Editor
{
    class StaticMeshComponentWidget final : public IComponentWidget
    {
    public:
        StaticMeshComponentWidget( Assets::AssetManager* assetManager, const ComponentEditContext* ctx = nullptr );

        bool CanRemove() const override
        {
            return false;
        }

        void Render( ECS::Entity& entity, ::Desert::Core::Scene* scene = nullptr ) override;

    private:
        void        SetMeshAsset( ECS::StaticMeshComponent& staticMesh, const Assets::AssetHandle& handle );
        std::string GetPrimitiveName( const ECS::StaticMeshComponent& staticMesh ) const;

        // The mesh thumbnail on the asset row: the asset browser's cached PNG, a neutral framed glyph
        // otherwise. Leaves the cursor on the same line for the slot field.
        void DrawMeshThumbnail( const ECS::StaticMeshComponent& staticMesh, float size ) const;

        // In-editor rigging section: place bones on an asset-backed static mesh and "Convert to Skinned".
        void RenderRigging( ECS::Entity& entity, ECS::StaticMeshComponent& staticMesh );

    private:
        Assets::AssetManager* m_AssetManager;
        // The Details panel's shared preview, when it lent one — the mesh row draws it as a thumbnail.
        const ComponentEditContext* m_Ctx = nullptr;
    };
} // namespace Desert::Editor