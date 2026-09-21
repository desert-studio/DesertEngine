#include <Engine/Assets/RetargetAsset.hpp>

#include <Common/Core/Constants.hpp>
#include <Common/Core/Logger.hpp>
#include <Common/Utilities/FileSystem.hpp>
#include <Common/Utilities/VFS.hpp>

#include <filesystem>

namespace Desert::Assets
{
    RetargetAsset::RetargetAsset( AssetPriority priority, const Common::Filepath& filepath )
         : AssetBase( priority, filepath, AssetTypeID::Retarget )
    {
        m_DisplayName = m_Metadata.Filepath.stem().string();
    }

    Common::BoolResultStr RetargetAsset::LoadFromFile()
    {
        const std::string path = m_Metadata.Filepath.string();

        // Through the VFS first, so a packaged build reads the retarget out of its .dpak exactly like
        // every other asset, then off the disk for a loose file the pak does not carry.
        std::string text;
        if ( const auto packed = Common::Utils::VFS::Exists( m_Metadata.Filepath )
                                      ? Common::Utils::VFS::ReadFile( m_Metadata.Filepath )
                                      : std::nullopt;
             packed.has_value() )
        {
            text = packed.value();
        }
        else
        {
            if ( auto read = Common::Utils::FileSystem::ReadFileContent( m_Metadata.Filepath ); read )
            {
                text = read.ExtractValue();
            }
            // A failed read leaves `text` empty on purpose: the branch below is the one refusal that names
            // both shapes ("empty or could not be opened").
        }

        if ( text.empty() )
        {
            m_Ready = false;
            return Common::MakeFormattedError<bool>( "retarget '{}' is empty or could not be opened", path );
        }

        auto parsed = Serialization::ParseRetarget( text );
        if ( !parsed )
        {
            m_Ready = false;
            return Common::MakeFormattedError<bool>( "retarget '{}' is not usable: {}", path,
                                                     parsed.GetError() );
        }

        m_Data        = parsed.ExtractValue();
        m_DisplayName = m_Data.Name.empty() ? m_Metadata.Filepath.stem().string() : m_Data.Name;
        ++m_Revision;
        m_Ready = true;

        // THE SOURCE RIG IS PART OF WHAT WAS LOADED, so it is part of the line that says what was loaded.
        // Without it the log cannot tell a retarget waiting for a rig that is not in the project from one
        // whose rig is simply cold — and that is the only observation a person makes on a headless run.
        LOG_INFO( "[Animation] Retarget '{}' loaded: source rig '{}', {} chain(s), {} rename(s), "
                  "{}+{} retarget-pose offset(s).",
                  m_DisplayName, m_Data.SourceSkeleton, m_Data.Chains.size(),
                  m_Data.BoneRenames.size(), m_Data.SourceRetargetPose.BoneOffsets.size(),
                  m_Data.TargetRetargetPose.BoneOffsets.size() );
        return BOOLSUCCESS;
    }

    void RetargetAsset::ResolveDependencies( AssetManager& manager )
    {
        m_SourceSkeleton.Handle = Common::AssetHandle::Null();
        m_SourceSkeleton.Cached.reset();

        if ( m_Data.SourceSkeleton.empty() )
        {
            return;
        }

        // RELATIVE TO THE COOKED MESHES ROOT, JOINED HERE AND NOWHERE ELSE — `CloudTypeAsset`'s shape, and
        // against the root a `.skeleton` actually lives under: `AssetPreloader` scans skeletons from
        // `MESH_PATH_COOKED` and from nowhere else, so a rig indexed anywhere else does not exist.
        const Common::Filepath full =
             ( Common::Constants::Path::MESH_PATH_COOKED / m_Data.SourceSkeleton ).lexically_normal();

        auto skeleton = manager.FindByPath<SkeletonAsset>( full );
        if ( !skeleton )
        {
            // NOT a silent fall-through to "no retarget": the file names a rig, the rig is not there, and
            // the character that comes out will be the un-retargeted one wearing this retarget's name.
            LOG_WARN( "RetargetAsset '{}': source rig '{}' ({}) is not a skeleton this project has "
                      "scanned. Characters naming this retarget play their clips on their own rig.",
                      m_Metadata.Filepath.string(), m_Data.SourceSkeleton, full.string() );
            return;
        }

        // THE RIG'S BONES MUST BE RESIDENT BEFORE THIS COUNTS AS RESOLVED. The only thing anyone does with
        // this dependency is copy the bones into a `RetargetSource`, which needs them present; and
        // eviction releases a source rig whenever no scene names it, so "registered but cold" is the
        // ordinary state here rather than an edge case. `SkeletonAsset::GetSignature`'s own comment
        // records what the equivalent omission cost on the mesh's side: 410 "dependency invalid" lines in
        // twelve seconds and no character drawn.
        if ( const auto loaded = skeleton->EnsureLoaded( manager ); !loaded )
        {
            LOG_ERROR( "RetargetAsset '{}': source rig '{}' could not be read: {}",
                       m_Metadata.Filepath.string(), full.string(), loaded.GetError() );
            return;
        }

        m_SourceSkeleton.Handle = skeleton->GetMetadata().Handle;
        m_SourceSkeleton.Cached = skeleton;
    }

    Common::BoolResultStr RetargetAsset::Unload()
    {
        m_Data        = Serialization::RetargetAssetData{};
        m_DisplayName = m_Metadata.Filepath.stem().string();
        m_SourceSkeleton.Handle = Common::AssetHandle::Null();
        m_SourceSkeleton.Cached.reset();
        m_Ready = false;
        // m_Revision is NOT reset, for UIThemeAsset's reason: it is monotonic per instance, and rewinding
        // it would make the next load look like no change at all to a consumer that compares revisions.
        return BOOLSUCCESS;
    }

    Common::BoolResultStr RetargetAsset::Save( const Common::Filepath&                 filepath,
                                               const Serialization::RetargetAssetData& data )
    {
        std::error_code ec;
        if ( filepath.has_parent_path() )
        {
            std::filesystem::create_directories( filepath.parent_path(), ec );
        }

        // SaveRetargetFile validates before it writes and goes through the atomic write primitive, so a
        // rejected retarget never reaches disk and a failed write cannot cost the author the file they had.
        if ( const auto ok = Serialization::SaveRetargetFile( filepath, data ); !ok )
        {
            return ok;
        }

        LOG_INFO( "[Animation] Retarget written: '{}', source rig '{}', {} chain(s), {} rename(s).",
                  filepath.string(), data.SourceSkeleton, data.Chains.size(),
                  data.BoneRenames.size() );
        return BOOLSUCCESS;
    }
} // namespace Desert::Assets
