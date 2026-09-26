#pragma once

#include <Editor/Core/SubjectEditorRegistry.hpp>
#include <Editor/Panels/IPanel.hpp>

#include <string>

namespace Desert::Editor
{
    // The subject a skybox viewer is about: the SkyboxAsset's handle under the Skybox type. One function, called
    // by the document's constructor and by the registration's factory, so the window a request names and the
    // window the document says it is cannot be spelled two ways.
    [[nodiscard]] inline SubjectId SkyboxViewerSubject( const Assets::AssetHandle& skybox )
    {
        return AssetSubject( skybox, static_cast<uint32_t>( Assets::AssetTypeID::Skybox ) );
    }

    /**
     * @brief The renderer-free half of SkyboxViewerDocument: its identity and its answers to the slot census.
     *
     * SPLIT OUT FOR ONE REASON: the whole document needs a PreviewViewport, which needs a device, and the suites
     * that assert open-or-focus and the slot census (AssetDocumentIdentity) link nothing from the engine. What
     * those suites ask of the document — which subject it is, whether it will claim a slot, whether it holds one
     * — is answered HERE and nowhere else, so the suite exercises the viewer's own answers rather than a stub
     * that agrees with them today.
     *
     * A CLAIMANT FROM BIRTH. The picture is a PreviewViewport (a Scene and a SceneRenderer), so the window
     * claims one of the six slots whether or not it has drawn yet; `final` so a derived viewer cannot quietly
     * answer otherwise. It HOLDS one exactly while its preview exists — the derived class raises the flag when
     * it builds the preview and lowers it when it gives the slot back.
     */
    class SkyboxViewerBase : public ISubjectDocument
    {
    public:
        SkyboxViewerBase( const std::string& name, const Assets::AssetHandle& skybox )
             : ISubjectDocument( name, SkyboxViewerSubject( skybox ) )
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

        // A viewer: nothing in this window writes the asset, so it cannot differ from the file.
        [[nodiscard]] DiskState GetDiskState() const override
        {
            return DiskState::Clean;
        }

    protected:
        bool m_PreviewLive = false;
    };
} // namespace Desert::Editor
