#pragma once

#include <Editor/Core/Selection/ModelingState.hpp>

#include <Common/Core/ResultStr.hpp>
#include <Common/Core/UUID.hpp>

#include <functional>

namespace Desert::Editor::Tools
{
    // What Accept and Cancel do to the Cube Grid's in-progress piece and to the tool, apart from ImGui and
    // the scene so a suite can drive them. CG1: one refused Accept had already ended the tool (the tool bar
    // ended it before the tool resolved the request), so the piece stayed the tool's - never selected or
    // recorded, its overlay kept drawing, and the next grid's Cancel destroyed it with the new cells. So:
    //  - Accept commits and forgets the piece only when Output's write succeeded; a refusal keeps the piece
    //    AND the tool and is returned for the tool to show.
    //  - Only a successful Accept that asked to end the tool (the tool bar's Accept, not "Accept and Start
    //    New") ends it.
    //  - Cancel destroys only the piece the session still holds - never an accepted one.
    using BlockoutWrite  = std::function<Common::BoolResultStr( const Common::UUID& piece )>;
    using BlockoutCommit = std::function<void( const Common::UUID& piece )>;

    [[nodiscard]] inline Common::BoolResultStr AcceptBlockout( Common::UUID& piece, Core::ModelingState& ms,
                                                               bool endTool, const BlockoutWrite& write,
                                                               const BlockoutCommit& commit )
    {
        if ( piece != Common::UUID::Null() )
        {
            if ( auto written = write( piece ); !written.IsSuccess() )
                return written;
            commit( piece );
            piece = Common::UUID::Null();
        }
        if ( endTool )
            ms.ActiveTool = Core::ModelingState::Tool::None;
        return Common::MakeSuccess( true );
    }

    inline void CancelBlockout( Common::UUID& piece, const BlockoutCommit& destroy )
    {
        if ( piece != Common::UUID::Null() )
            destroy( piece );
        piece = Common::UUID::Null();
    }
} // namespace Desert::Editor::Tools
