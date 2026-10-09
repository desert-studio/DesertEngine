#pragma once

#include <Editor/Core/CommandHistory.hpp>
#include <Engine/Assets/ContentRegistry.hpp>

#include <Common/Core/Logger.hpp>
#include <Common/Core/ResultStr.hpp>

#include <filesystem>
#include <ranges>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

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
    // A FOLDER'S RENAME / MOVE AS ONE UNDO STEP (AF10c): undo takes back every asset and plain file the move
    // carried; redo moves the folder again with each asset's recorded redirector identity.
    class AssetFolderMoveCommand final : public ICommand
    {
    public:
        AssetFolderMoveCommand( Common::Content::AssetFolderMoveRecord record, std::string label )
             : m_Record( std::move( record ) ), m_Label( std::move( label ) )
        {
        }

        bool Undo() override
        {
            if ( const auto undone = Assets::ContentRegistry::UndoFolderMove( m_Record ); !undone )
            {
                LOG_ERROR( "[AssetMove] {}", undone.GetError() );
                return false;
            }
            return true;
        }

        bool Redo() override
        {
            std::map<std::filesystem::path, Common::Content::AssetGuid> selves;
            for ( const Common::Content::AssetMoveRecord& asset : m_Record.Assets )
                selves.emplace( asset.From, asset.Redirector.Self );
            auto redone = Assets::ContentRegistry::MoveFolder( m_Record.From, m_Record.To, selves );
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
        Common::Content::AssetFolderMoveRecord m_Record;
        std::string                            m_Label;
    };

    // The editor's route for renaming / moving a folder: all or nothing through the registry, one undo entry.
    [[nodiscard]] inline Common::ResultStr<std::filesystem::path>
    MoveFolderWithUndo( const std::filesystem::path& from, const std::filesystem::path& to, std::string label )
    {
        auto moved = Assets::ContentRegistry::MoveFolder( from, to );
        if ( !moved )
            return Common::MakeError<std::filesystem::path>( moved.GetError() );
        CommandHistory::Get().PushCommand(
             std::make_unique<AssetFolderMoveCommand>( moved.ExtractValue(), std::move( label ) ) );
        return Common::MakeSuccess( to );
    }
    // A DELETE ON THE UNDO STACK (ASSET-TRASH): the delete moved each asset into the project's trash, so undo
    // puts the very same bytes back at the very same paths with their registry rows (GUIDs and every reference
    // intact), last deleted first; redo trashes them again into new slots. One entry for a whole selection. Not
    // volatile: it names files by path, not live objects.
    class AssetTrashCommand final : public ICommand
    {
    public:
        AssetTrashCommand( std::vector<Common::Content::AssetTrashRecord> records, std::string label )
             : m_Records( std::move( records ) ), m_Label( std::move( label ) )
        {
        }

        bool Undo() override
        {
            bool all = true;
            for ( const auto& record : std::views::reverse( m_Records ) )
                if ( const auto restored = Assets::ContentRegistry::RestoreTrashed( record ); !restored )
                {
                    LOG_ERROR( "[Trash] {}", restored.GetError() );
                    all = false;
                }
            return all;
        }

        bool Redo() override
        {
            bool all = true;
            for ( Common::Content::AssetTrashRecord& record : m_Records )
            {
                auto trashed = Assets::ContentRegistry::TrashAsset( record.From, record.Slot.parent_path() );
                if ( !trashed )
                {
                    LOG_ERROR( "[Trash] redo: {}", trashed.GetError() );
                    all = false;
                    continue;
                }
                record = trashed.ExtractValue();
            }
            return all;
        }

        [[nodiscard]] std::string GetLabel() const override
        {
            return m_Label;
        }

    private:
        std::vector<Common::Content::AssetTrashRecord> m_Records;
        std::string                                    m_Label;
    };

    // The editor's delete: every path into the project's trash (Saved/Trash), one undo entry for what was
    // trashed. Each refusal is returned by name; the rest are still deleted (and undoable).
    [[nodiscard]] inline std::vector<std::string>
    DeleteAssetsWithUndo( const std::vector<std::filesystem::path>& paths )
    {
        std::vector<Common::Content::AssetTrashRecord> records;
        std::vector<std::string>                       refusals;
        for ( const std::filesystem::path& path : paths )
        {
            auto trashed = Assets::ContentRegistry::TrashAsset( path, Common::Content::ProjectTrashRoot() );
            if ( trashed )
                records.push_back( trashed.ExtractValue() );
            else
                refusals.push_back( trashed.GetError() );
        }
        if ( !records.empty() )
        {
            std::string label = records.size() == 1 ? "Delete " + records.front().From.filename().string()
                                                    : "Delete " + std::to_string( records.size() ) + " assets";
            CommandHistory::Get().PushCommand(
                 std::make_unique<AssetTrashCommand>( std::move( records ), std::move( label ) ) );
        }
        return refusals;
    }
} // namespace Desert::Editor
