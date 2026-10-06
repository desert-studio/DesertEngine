#pragma once

/**
 * THE CONTROL RIG EDITOR — one window per `.derig`, as UE's Control Rig Editor (07_panels_design §11.2).
 *
 *   RIG ELEMENTS │ CANVAS (the Forwards solve, RigGraph) │ INSPECTOR (the selected control or node)
 *   status line: what Save would refuse with, and what the rig's skeleton binds it to
 *
 * Everything it edits goes through ControlRigDocumentModel, which owns the value, the undo records and the
 * write; this class is only the three views and the gestures that call the model. The ControlRigPanel on a
 * SELECTED ENTITY stays what it is — the live pose of a rig instance in a scene; this window is the asset.
 */

#include <Editor/Core/SubjectEditorRegistry.hpp>
#include <Editor/Panels/ControlRig/ControlRigDocumentModel.hpp>
#include <Editor/Panels/IPanel.hpp>

#include <Common/Core/Core.hpp>
#include <Engine/Assets/AssetMetadata.hpp>

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ax::NodeEditor
{
    struct EditorContext;
}

namespace Desert::Assets
{
    class AssetManager;
    class SkeletonAsset;
} // namespace Desert::Assets

namespace Desert::Editor
{
    class ControlRigDocument final : public ISubjectDocument
    {
    public:
        ControlRigDocument( const Assets::AssetHandle& subject, Assets::AssetManager* assets );
        ~ControlRigDocument() override;

        [[nodiscard]] glm::vec2 GetDefaultSize() const override
        {
            return { 1280.0f, 760.0f };
        }
        void OnUIRender() override;

        [[nodiscard]] bool IsSubjectAlive() const override;
        [[nodiscard]] bool HoldsView() const override
        {
            return false;
        }
        [[nodiscard]] bool ClaimsView() const override
        {
            return false;
        }
        [[nodiscard]] DiskState GetDiskState() const override;
        bool                    SaveDocument() override;

    private:
        struct PinRef
        {
            std::string Node;
            std::string Pin;
            bool        Input = false;
        };

        void                                   DrawToolbar();
        void                                   DrawElements();
        void                                   DrawCanvas();
        void                                   DrawInspector();
        void                                   DrawControlInspector( const std::string& name );
        void                                   DrawNodeInspector( const std::string& name );
        void                                   DrawStatus();
        void                                   Report( const Common::BoolResultStr& result );
        void                                   LoadSkeleton();
        [[nodiscard]] std::vector<std::string> BoneNames() const;

        Assets::AssetManager*                    m_Assets = nullptr;
        std::unique_ptr<ControlRigDocumentModel> m_Model;
        std::string                              m_OpenError;
        std::shared_ptr<Assets::SkeletonAsset>   m_Skeleton;
        std::string                              m_SkeletonError;
        std::vector<std::string>                 m_ShapeNames;

        ax::NodeEditor::EditorContext*        m_Canvas = nullptr;
        std::unordered_set<std::string>       m_Placed;
        std::unordered_map<uintptr_t, PinRef> m_Pins;

        std::string m_SelectedControl;
        std::string m_SelectedNode;
        std::string m_NewControlName;

        // The inspector edits a DRAFT while a widget is held, so a drag is one undo record, not sixty.
        std::optional<Assets::Serialization::ControlElementData> m_Draft;
        std::string                                              m_DraftOf;
        uint64_t                                                 m_DraftRevision = 0;
        std::optional<Assets::Serialization::RigGraphInputData>  m_LiteralDraft;
        std::string                                              m_LiteralDraftNode;

        uint64_t    m_StatusRevision = UINT64_MAX;
        std::string m_Status;
        bool        m_StatusError = false;
        std::string m_LastRefusal;
    };

    /// The browser's door: registers @p path as a ControlRigAsset and opens its document.
    [[nodiscard]] SubjectEditorRegistry::PathOpenOutcome
    RequestControlRigDocument( Assets::AssetManager* assets, const std::string& path,
                               const SubjectEditorRegistry& editors );
} // namespace Desert::Editor
