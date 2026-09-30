#include <Editor/Panels/AnimationEditor/SkeletonReferenceSlots.hpp>

#include <Common/Core/Core.hpp>
#include <Editor/Core/ImGuiUtilities.hpp>
#include <Editor/Widgets/AssetFieldOpen.hpp>

#include <Engine/Animation/SkeletonReference.hpp>
#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/Mesh/AnimationAsset.hpp>
#include <Engine/Assets/Mesh/SkeletonAsset.hpp>
#include <Engine/Assets/Mesh/SkeletonReferenceAssets.hpp>
#include <Engine/Assets/Mesh/SkinnedMeshAsset.hpp>

#include <ImGui/imgui.h>

#include <algorithm>
#include <cctype>
#include <format>
#include <vector>

namespace Desert::Editor::SkeletonSlots
{
    using Common::Content::AssetGuid;
    using Common::Content::ContentKind;

    namespace
    {
        std::string Lower( std::string text )
        {
            std::ranges::transform( text, text.begin(),
                                    []( const unsigned char c ) { return static_cast<char>( std::tolower( c ) ); } );
            return text;
        }

        std::optional<Assets::ContentRegistry::PickerRow> RowOfGuid( const ContentKind kind, const AssetGuid& guid )
        {
            if ( guid.IsNull() )
                return std::nullopt;
            return Assets::ContentRegistry::RowOf( kind, HandleOf( guid ) );
        }
    } // namespace

    uint64_t HandleOf( const AssetGuid& guid )
    {
        return guid.IsNull() ? 0U : static_cast<uint64_t>( Common::Content::HandleForGuid( guid ) );
    }

    std::string NameOf( const ContentKind kind, const AssetGuid& guid )
    {
        if ( guid.IsNull() )
            return "None";
        if ( const auto row = RowOfGuid( kind, guid ) )
            return row->Path.filename().string();
        return std::format( "{:016x}{:016x} (not registered)", guid.Hi, guid.Lo );
    }

    std::optional<AssetGuid> DrawGuidSlot( const char* id, const ContentKind kind, const AssetGuid& current,
                                           const bool allowNone, std::array<char, 64>& filter,
                                           const std::function<bool( const Assets::ContentRegistry::PickerRow& )>& accept )
    {
        std::optional<AssetGuid> picked;
        ImGui::PushID( id );
        const float buttons = ImGui::GetFrameHeight() * 2.0f + ImGui::GetStyle().ItemSpacing.x * 2.0f;
        ImGui::SetNextItemWidth( std::max( 60.0f, ImGui::GetContentRegionAvail().x - buttons ) );
        ImGui::BeginGroup();
        if ( Utils::ImGuiUtilities::AssetSlot( "##slot", NameOf( kind, current ).c_str(), current.IsNull() ) )
        {
            filter.fill( '\0' );
            ImGui::OpenPopup( "##picker" );
        }
        ImGui::EndGroup();
        DrawAssetFieldOpen( HandleOf( current ) );
        ImGui::SameLine();
        DrawAssetFieldButtons( HandleOf( current ) );

        if ( ImGui::BeginPopup( "##picker" ) )
        {
            if ( ImGui::IsWindowAppearing() )
                ImGui::SetKeyboardFocusHere();
            ImGui::SetNextItemWidth( 280.0f );
            ImGui::InputTextWithHint( "##search", "Search Assets", filter.data(), filter.size() );
            const std::string needle = Lower( filter.data() );
            if ( allowNone && ImGui::Selectable( "None", current.IsNull() ) )
                picked = AssetGuid{};
            std::vector<std::pair<std::string, AssetGuid>> rows;
            for ( const auto& row : Assets::ContentRegistry::Rows( kind ) )
            {
                if ( !row.Guid || ( accept && !accept( row ) ) )
                    continue;
                std::string name = row.Path.filename().string();
                if ( needle.empty() || Lower( name ).find( needle ) != std::string::npos )
                    rows.emplace_back( std::move( name ), *row.Guid );
            }
            std::ranges::sort( rows, {}, []( const auto& row ) { return row.first; } );
            if ( rows.empty() )
                ImGui::TextDisabled( "No matching assets." );
            for ( const auto& [name, guid] : rows )
                if ( ImGui::Selectable( name.c_str(), guid == current ) )
                    picked = guid;
            if ( picked )
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        ImGui::PopID();
        return picked;
    }

    Common::ResultStr<std::shared_ptr<Assets::SkeletonAsset>> LoadSkeleton( Assets::AssetManager& assets,
                                                                          const AssetGuid&      skeleton )
    {
        using Result = std::shared_ptr<Assets::SkeletonAsset>;
        const auto row = RowOfGuid( ContentKind::Skeleton, skeleton );
        if ( !row )
            return Common::MakeError<Result>(
                 std::format( "skeleton {} is not a registered .skeleton", NameOf( ContentKind::Skeleton, skeleton ) ) );
        auto asset = assets.FindByPath<Assets::SkeletonAsset>( row->Path );
        if ( !asset )
            asset = assets.CreateAsset<Assets::SkeletonAsset>( row->Path, false );
        if ( !asset )
            return Common::MakeError<Result>(
                 std::format( "skeleton '{}' could not be registered", row->Path.generic_string() ) );
        if ( const auto loaded = asset->EnsureLoaded( assets ); !loaded )
            return Common::MakeError<Result>(
                 std::format( "skeleton '{}' would not load: {}", row->Path.generic_string(), loaded.GetError() ) );
        return Common::MakeSuccess( std::move( asset ) );
    }

    namespace
    {
        Common::BoolResultStr Check( Assets::AssetManager& assets, const AssetGuid& skeleton,
                                     const std::vector<Animation::RequiredBone>& required,
                                     const std::string&                          assetName )
        {
            if ( skeleton.IsNull() )
                return Common::MakeError<bool>(
                     std::format( "'{}': a skeleton reference cannot be cleared — every mesh and clip names one",
                                  assetName ) );
            const auto loaded = LoadSkeleton( assets, skeleton );
            if ( !loaded )
                return Common::MakeError<bool>( loaded.GetError() );
            const auto* bones = loaded.GetValue()->GetSkeleton();
            if ( bones == nullptr )
                return Common::MakeError<bool>(
                     std::format( "skeleton {} holds no bones", NameOf( ContentKind::Skeleton, skeleton ) ) );
            return Animation::CheckSkeletonAssignment( *bones, NameOf( ContentKind::Skeleton, skeleton ), required,
                                                       assetName );
        }
    } // namespace

    Common::BoolResultStr AssignMeshSkeleton( Assets::AssetManager& assets, Assets::SkinnedMeshAsset& mesh,
                                              const AssetGuid& skeleton )
    {
        const auto&       path = mesh.GetMetadata().Filepath;
        const std::string name = path.filename().string();
        if ( auto checked = Check( assets, skeleton, Assets::RequiredBonesOf( mesh ), name ); !checked )
            return checked;
        if ( auto saved = Assets::Serialization::SaveMeshSkeletonReference( path, skeleton ); !saved )
            return saved;
        mesh.SetSkeleton( skeleton );
        return BOOLSUCCESS;
    }

    Common::BoolResultStr AssignClipSkeleton( Assets::AssetManager& assets, Assets::AnimationAsset& clip,
                                              const AssetGuid& skeleton )
    {
        const std::string name = clip.GetMetadata().Filepath.filename().string();
        if ( auto checked = Check( assets, skeleton, Assets::RequiredBonesOf( clip ), name ); !checked )
            return checked;
        clip.SetSkeleton( skeleton );
        return BOOLSUCCESS;
    }
} // namespace Desert::Editor::SkeletonSlots
