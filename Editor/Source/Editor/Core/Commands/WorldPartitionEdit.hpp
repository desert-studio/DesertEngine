#pragma once

#include "../CommandHistory.hpp"

#include <Engine/Core/Scene.hpp>

#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace Desert::Editor
{
    // One history entry for "the world's partition changed": the block before and the block after (a conversion,
    // a World Settings field).
    //
    // WEAK, not shared. A history entry that owned the scene would keep a closed world alive for as long as the
    // undo stack remembered it; ICommand's contract is that an entry whose target is gone returns false and is
    // discarded, and a weak pointer is how that is DETECTED rather than assumed. The scene is addressed as a
    // whole object because the partition is a property of the world itself and has no id of its own.
    class SetWorldPartitionCommand final : public ICommand
    {
    public:
        SetWorldPartitionCommand( std::weak_ptr<::Desert::Core::Scene>                    scene,
                                  std::optional<::Desert::Core::WorldPartitionSerialized> before,
                                  std::optional<::Desert::Core::WorldPartitionSerialized> after,
                                  std::string                                             label )
             : m_Scene( std::move( scene ) ), m_Before( std::move( before ) ), m_After( std::move( after ) ),
               m_Label( std::move( label ) )
        {
        }

        [[nodiscard]] std::string GetLabel() const override
        {
            return m_Label;
        }
        bool Undo() override
        {
            return Apply( m_Before );
        }
        bool Redo() override
        {
            return Apply( m_After );
        }

    private:
        bool Apply( const std::optional<::Desert::Core::WorldPartitionSerialized>& partition )
        {
            const std::shared_ptr<::Desert::Core::Scene> scene = m_Scene.lock();
            if ( !scene )
                return false;
            scene->SetWorldPartition( partition );
            return true;
        }

        std::weak_ptr<::Desert::Core::Scene>                    m_Scene;
        std::optional<::Desert::Core::WorldPartitionSerialized> m_Before;
        std::optional<::Desert::Core::WorldPartitionSerialized> m_After;
        std::string                                             m_Label;
    };
} // namespace Desert::Editor
