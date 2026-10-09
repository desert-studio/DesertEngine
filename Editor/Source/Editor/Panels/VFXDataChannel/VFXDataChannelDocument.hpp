#pragma once

#include <Editor/Core/SubjectEditorRegistry.hpp>
#include <Editor/Panels/IPanel.hpp>

#include <Common/Core/Core.hpp> // Common::Filepath, which AssetMetadata.hpp names without including
#include <Engine/Assets/AssetMetadata.hpp>
#include <Engine/Assets/Serialization/VFXDataChannel.hpp>

#include <Common/Core/ResultStr.hpp>

#include <array>
#include <cstddef>
#include <string>

namespace Desert::Assets
{
    class AssetManager;
}

namespace Desert::Editor
{
    /**
     * @brief One window per `.dfxch`: the channel's field list - UE's Niagara Data Channel asset editor (VFX-10c).
     *
     * A table of the channel's fields (name, type, order) with add / remove / rename / retype / move. EVERY widget
     * is a thin caller of the command layer in Editor/Core/Commands/VFXDataChannelEdit.hpp: the window owns no
     * rule of its own about what a channel may hold - a refused edit (an empty or repeated name, a 17th field,
     * removing the last one) is ValidateVFXDataChannelData's refusal, shown in the window, and changes nothing.
     * Each accepted edit is one CommandHistory entry whose EditedObject is m_Working, so the destructor forgets
     * this window's entries with DropFor(&m_Working).
     *
     * WORKING AND SAVED: the document edits a copy of the asset's data; Dirty is m_Working.Fields !=
     * m_Saved.Fields (derived, never a flag), and Save writes m_Working through the asset's own serializer
     * (VFXDataChannelAsset::Save), then has the held asset re-read its file so every reader sees what was written.
     *
     * NO RENDERER SLOT: an ImGui table only, no PreviewViewport.
     */
    class VFXDataChannelDocument final : public ISubjectDocument
    {
    public:
        VFXDataChannelDocument( const Assets::AssetHandle& subject, Assets::AssetManager* assets );
        ~VFXDataChannelDocument() override;

        [[nodiscard]] glm::vec2 GetDefaultSize() const override
        {
            return { 560.0f, 420.0f };
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

        bool SaveDocument() override;

    private:
        void DrawFieldRow( std::size_t index );
        void Report( const Common::BoolResultStr& result );

        Assets::AssetManager*                     m_Assets = nullptr;
        bool                                      m_Loaded = false; // the asset was found and read at open
        std::string                               m_LoadError;
        Assets::Serialization::VFXDataChannelData m_Working; // what the window edits (CommandHistory target)
        Assets::Serialization::VFXDataChannelData m_Saved;   // what the file holds
        std::string                               m_Refusal; // the last refused edit or save, shown inline

        // The row whose name is being typed, and its text: ImGui hands the edit back only while the item is
        // active, so the text lives here until it deactivates and is committed as ONE rename.
        static constexpr std::size_t kNoRow    = static_cast<std::size_t>( -1 );
        std::size_t                  m_NameRow = kNoRow;
        std::array<char, 64>         m_NameBuffer{};
    };

    // The `.dfxch` path opener: find-or-create the VFXDataChannelAsset, make sure it is read, then open it through
    // the one handle route, Core::RequestOpenAsset. NotMine for any other extension or a path with no file;
    // Failed, with the reason logged, when it will not load or open.
    [[nodiscard]] SubjectEditorRegistry::PathOpenOutcome
    RequestVFXDataChannelDocument( Assets::AssetManager* assets, const std::string& path,
                                   const SubjectEditorRegistry& editors );
} // namespace Desert::Editor
