#pragma once

// AI-STATETREE CONTRACT (header only, owner review pending) - docs/contracts/AI-STATETREE.md.
//
// A STATE TREE on disk (`.dstatetree`, UE UStateTree StateTree.h:133), in the engine's one asset system - the
// AnimGraphAsset pattern: the asset owns the authored tree and the compiled tree, every NPC naming the file
// shares the compiled one (each keeps its own StateTreeExecution), and the revision belongs to the asset, so
// one edit re-starts every NPC running it.
//
// AI-ST-ASSET adds `AssetTypeID::StateTree` above `Count` in Assets/Common.hpp together with its entry in the
// AssetHandleStability catalogue (adding the enumerator alone turns that census red), and defines GetTypeID.

#include <Engine/AI/StateTree/StateTree.hpp>
#include <Engine/Assets/AssetBase.hpp>

#include <memory>
#include <string_view>

namespace Desert::Assets
{
    inline constexpr std::string_view kStateTreeExtension = AI::StateTree::kStateTreeExtension;

    class StateTreeAsset final : public AssetBase
    {
    public:
        explicit StateTreeAsset( const Common::Filepath& filepath );

        /// Reads, parses and COMPILES. A file that does not parse or a tree that does not compile is an error
        /// carrying every compiler message - never an empty tree that leaves an NPC standing still.
        Common::BoolResultStr LoadFromFile() override;
        Common::BoolResultStr Unload() override;

        [[nodiscard]] bool IsReadyForUse() const override
        {
            return m_Compiled != nullptr;
        }

        /// The authored tree (the editor edits this object, then calls MarkEdited).
        [[nodiscard]] const std::shared_ptr<AI::StateTree::StateTreeDesc>& GetDesc() const
        {
            return m_Desc;
        }

        /// The compiled tree every execution shares. Null until a successful Load.
        [[nodiscard]] const std::shared_ptr<const AI::StateTree::CompiledStateTree>& GetCompiled() const
        {
            return m_Compiled;
        }

        /// Warnings of the last successful compile (the editor's validation list).
        [[nodiscard]] const std::vector<AI::StateTree::CompileMessage>& GetMessages() const
        {
            return m_Messages;
        }

        [[nodiscard]] uint32_t GetRevision() const
        {
            return m_Revision;
        }

        /// The authored tree was edited: recompile. On success the compiled tree is replaced and the revision
        /// bumped; on failure the previous compiled tree stays (running NPCs keep running) and the errors are
        /// returned for the editor to show.
        Common::BoolResultStr MarkEdited();

        static AssetTypeID GetTypeID();

        static Common::BoolResultStr Save( const Common::Filepath&             filepath,
                                           const AI::StateTree::StateTreeDesc& tree );

    private:
        std::shared_ptr<AI::StateTree::StateTreeDesc>           m_Desc;
        std::shared_ptr<const AI::StateTree::CompiledStateTree> m_Compiled;
        std::vector<AI::StateTree::CompileMessage>              m_Messages;
        uint32_t                                                m_Revision = 0;
    };
} // namespace Desert::Assets
