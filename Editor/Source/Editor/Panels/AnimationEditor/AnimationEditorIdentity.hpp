#pragma once

#include <Editor/Core/SubjectEditorRegistry.hpp>
#include <Editor/Panels/IPanel.hpp>

#include <string>

namespace Desert::Editor
{
    // The subject an Animation Editor window is about: the clip asset's handle under the Animation type. One
    // function, called by the document's constructor and by the registration's factory, so the window a
    // request names and the window the document says it is cannot be spelled two ways.
    [[nodiscard]] inline SubjectId AnimationEditorSubject( const Assets::AssetHandle& clip )
    {
        return AssetSubject( clip, static_cast<uint32_t>( Assets::AssetTypeID::Animation ) );
    }

    /**
     * @brief The renderer-free half of AnimationEditorDocument: its identity and its answers to the slot census.
     *
     * Split out for the reason StaticMeshViewerBase is: the document needs a PreviewViewport (a device), and the
     * suites that pin open-or-focus and the slot census link nothing from the engine. What they ask — which
     * subject, will it claim a slot, does it hold one, does closing give it back — is answered HERE.
     *
     * CLOSING GIVES THE SLOT BACK, AND ONLY DESTRUCTION DOES. A preview that is merely hidden keeps its
     * SceneRenderer, and the renderer IS the slot. So ReleaseView is final here: it always destroys the preview
     * through DestroyPreview and lowers the flag after it, and a derived window cannot answer "released" while
     * its scene is still alive.
     */
    class AnimationEditorBase : public ISubjectDocument
    {
    public:
        AnimationEditorBase( const std::string& name, const Assets::AssetHandle& clip )
             : ISubjectDocument( name, AnimationEditorSubject( clip ) )
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

        void ReleaseView() final
        {
            DestroyPreview();
            m_PreviewLive = false;
        }

        // A viewer in this part: nothing in the window writes the clip, so it cannot differ from the file.
        [[nodiscard]] DiskState GetDiskState() const override
        {
            return DiskState::Clean;
        }

    protected:
        // Destroy the preview scene and its renderer (idling the device), which is what returns the slot.
        virtual void DestroyPreview() = 0;

        bool m_PreviewLive = false;
    };
} // namespace Desert::Editor
