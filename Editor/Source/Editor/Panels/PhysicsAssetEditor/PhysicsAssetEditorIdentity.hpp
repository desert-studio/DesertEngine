#pragma once

#include <Editor/Core/SubjectEditorRegistry.hpp>
#include <Editor/Panels/IPanel.hpp>

#include <Engine/Physics/PhysicsAssetFormat.hpp>

#include <memory>
#include <optional>
#include <string>

namespace Desert::Editor
{
    // The subject a physics asset editor is about: the `.dephysasset`'s handle under the PhysicsAsset type. One
    // function, called by the document's constructor and by the registration's factory.
    [[nodiscard]] inline SubjectId PhysicsAssetEditorSubject( const Assets::AssetHandle& asset )
    {
        return AssetSubject( asset, static_cast<uint32_t>( Assets::AssetTypeID::PhysicsAsset ) );
    }

    /**
     * @brief The renderer-free half of PhysicsAssetEditorDocument: its identity, its slot answers and its disk
     * state (StaticMeshViewerIdentity.hpp's split, for the same reason: the suites that ask these questions link
     * no device).
     *
     * A CLAIMANT FROM BIRTH: the preview is a PreviewViewport, so the window claims a renderer slot whether or not
     * it has drawn yet; it HOLDS one exactly while the preview exists.
     *
     * THIS WINDOW WRITES THE ASSET (unlike the static mesh viewer), so its disk state is a comparison: the working
     * copy every edit replaces against the data the file held when it was read or last saved. Untracked until the
     * file has been read.
     */
    class PhysicsAssetEditorBase : public ISubjectDocument
    {
    public:
        PhysicsAssetEditorBase( const std::string& name, const Assets::AssetHandle& asset )
             : ISubjectDocument( name, PhysicsAssetEditorSubject( asset ) )
        {
        }

        [[nodiscard]] bool ClaimsView() const final
        {
            return true;
        }

        [[nodiscard]] bool HoldsView() const final
        {
            return m_PreviewLive;
        }

        [[nodiscard]] DiskState GetDiskState() const final
        {
            if ( !m_Working || !m_OnDisk )
                return DiskState::Untracked;
            return *m_Working == *m_OnDisk ? DiskState::Clean : DiskState::Dirty;
        }

    protected:
        bool m_PreviewLive = false;
        // Shared: an undo record (PhysicsAssetEditCommand) holds it and outlives the window that pushed it.
        std::shared_ptr<Physics::PhysicsAssetData> m_Working;
        std::optional<Physics::PhysicsAssetData>   m_OnDisk;
    };
} // namespace Desert::Editor
