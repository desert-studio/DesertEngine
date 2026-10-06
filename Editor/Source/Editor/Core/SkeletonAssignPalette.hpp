#pragma once

#include <Editor/Core/CommandPalette.hpp>
#include <Common/Core/ResultStr.hpp>

#include <filesystem>
#include <format>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace Desert::Editor
{
    /**
     * @brief "Assets | Assign Skeleton…" (UE: Skeletal Mesh / Anim Sequence context menu -> Assign Skeleton).
     *
     * UE opens a skeleton picker on the selected mesh or clip; the palette's picker is one entry per registered
     * `.skeleton`, so a client on the control channel names the pair the way a click in the picker would, and the
     * skeleton need not sit in the Assets window's folder. The subject is every selected `.skmesh` / `.anim`.
     *
     * OFFERED UNCONDITIONALLY: with no mesh or clip selected (or no skeleton registered) the single entry
     * "Assign Skeleton…" stays and its refusal says what is missing — a vanished command cannot say why.
     *
     * `assign(subject, skeleton)` is the layer's CheckSkeletonAssignment path (SkeletonSlots::AssignMeshSkeleton /
     * AssignClipSkeleton): a refusal lists the missing and mis-parented bones and writes nothing. No ImGui, no
     * asset manager here — the census of entries is testable on its own.
     */
    inline constexpr const char* kAssignSkeletonLabel = "Assign Skeleton…";

    [[nodiscard]] inline bool IsSkeletonAssignSubject( const std::filesystem::path& path )
    {
        const std::string ext = path.extension().string();
        return ext == ".skmesh" || ext == ".anim";
    }

    using SkeletonAssignFn =
         std::function<Common::BoolResultStr( const std::string& subject, const std::string& skeleton )>;

    [[nodiscard]] inline std::vector<PaletteCommand>
    SkeletonAssignPaletteCommands( const std::vector<std::string>& selected,
                                   const std::vector<std::string>& skeletons, const SkeletonAssignFn& assign )
    {
        std::vector<std::string> subjects;
        for ( const std::string& path : selected )
            if ( IsSkeletonAssignSubject( path ) )
                subjects.push_back( path );

        std::vector<PaletteCommand> commands;
        if ( subjects.empty() || skeletons.empty() )
        {
            // The closures below hold their captures by value and NOT const: a const member is copied, not moved,
            // when std::function takes the closure, and that copy can throw from a move that must not.
            const bool  noSubject = subjects.empty();
            std::string reason    = noSubject ? "Assign Skeleton: select a .skmesh or .anim in the Assets window"
                                              : "Assign Skeleton: no .skeleton is registered in the project — "
                                                "import a skinned model first";
            commands.push_back( { "Assets", kAssignSkeletonLabel,
                                  [reason = std::move( reason )] { return Common::MakeError( reason ); } } );
            return commands;
        }
        for ( const std::string& subject : subjects )
            for ( const std::string& skeleton : skeletons )
                commands.push_back( { "Assets",
                                      std::format( "{} {} <- {}", kAssignSkeletonLabel,
                                                   std::filesystem::path( subject ).filename().string(),
                                                   std::filesystem::path( skeleton ).filename().string() ),
                                      [assign = assign, subject = subject, skeleton = skeleton]
                                      { return assign( subject, skeleton ); } } );
        return commands;
    }
} // namespace Desert::Editor
