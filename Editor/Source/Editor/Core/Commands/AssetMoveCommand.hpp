#pragma once

#include <Editor/Core/CommandHistory.hpp>
#include <Engine/Assets/ContentRegistry.hpp>

#include <Common/Core/Logger.hpp>
#include <Common/Core/ResultStr.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <utility>

namespace Desert::Editor
{
    // A RENAME / MOVE OF ONE ASSET ON THE UNDO STACK (AF10c). Undo gives back the file, removes the redirector
    // and restores the registry row; redo moves it again with the recorded redirector identity, so the redone
    // move writes the same redirector bytes. Not volatile: it names files by path, not live objects, so it
    // survives a scene switch.
    class AssetMoveCommand final : public ICommand
    {
    public:
        AssetMoveCommand( Common::Content::AssetMoveRecord record, std::string label )
             : m_Record( std::move( record ) ), m_Label( std::move( label ) )
        {
        }

        bool Undo() override
        {
            if ( const auto undone = Assets::ContentRegistry::UndoMove( m_Record ); !undone )
            {
                LOG_ERROR( "[AssetMove] {}", undone.GetError() );
                return false;
            }
            return true;
        }

        bool Redo() override
        {
            auto redone =
                 Assets::ContentRegistry::MoveAsset( m_Record.From, m_Record.To, m_Record.Redirector.Self );
            if ( !redone )
            {
                LOG_ERROR( "[AssetMove] redo: {}", redone.GetError() );
                return false;
            }
            m_Record = redone.ExtractValue();
            return true;
        }

        [[nodiscard]] std::string GetLabel() const override
        {
            return m_Label;
        }

    private:
        Common::Content::AssetMoveRecord m_Record;
        std::string                      m_Label;
    };

    // The editor's route for renaming / moving one registry-known asset: moves it through the registry and
    // pushes the undo entry. Refusals come back by name and push nothing.
    [[nodiscard]] inline Common::ResultStr<std::filesystem::path>
    MoveAssetWithUndo( const std::filesystem::path& from, const std::filesystem::path& to, std::string label )
    {
        auto moved = Assets::ContentRegistry::MoveAsset( from, to );
        if ( !moved )
            return Common::MakeError<std::filesystem::path>( moved.GetError() );
        CommandHistory::Get().PushCommand(
             std::make_unique<AssetMoveCommand>( moved.ExtractValue(), std::move( label ) ) );
        return Common::MakeSuccess( to );
    }
} // namespace Desert::Editor
