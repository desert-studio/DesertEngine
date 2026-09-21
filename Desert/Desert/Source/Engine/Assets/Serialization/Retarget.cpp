#include <Engine/Assets/Serialization/Retarget.hpp>

#include <Engine/Animation/Skeleton.hpp>

#include <Common/Core/Serialization/GlmReflection.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <cmath>
#include <filesystem>
#include <string>
#include <unordered_set>

#include <rflcpp/rfl/json.hpp>

namespace Desert::Assets::Serialization
{
    namespace
    {
        using Animation::Retarget::RetargetChain;
        using Animation::Retarget::RetargetPose;
        using Animation::Retarget::RetargetSetup;

        [[nodiscard]] bool IsFinite( const glm::vec3& v )
        {
            return std::isfinite( v.x ) && std::isfinite( v.y ) && std::isfinite( v.z );
        }

        [[nodiscard]] bool IsFinite( const glm::quat& q )
        {
            return std::isfinite( q.x ) && std::isfinite( q.y ) && std::isfinite( q.z ) && std::isfinite( q.w );
        }

        /**
         * @brief Refuses one rig's retarget pose, naming the row.
         *
         * @p side is "source" or "target" and is in every message: the two poses have identical shapes and
         * a message that does not say which one is wrong sends the author to the wrong half of the file.
         *
         * A ZERO-LENGTH ROTATION IS REFUSED RATHER THAN NORMALISED. `RetargetPose::Apply` post-multiplies
         * the delta onto the bind pose, and glm's `normalize` on a zero quaternion is a division by zero
         * that produces NaNs which then travel through every descendant of that bone. Repairing it here
         * would be the "quietly substituted" answer the contract forbids: the author asked for a rotation
         * and would get one they did not write.
         */
        [[nodiscard]] Common::BoolResultStr ValidateRetargetPoseData( const RetargetPoseData& pose,
                                                                      const char*             side )
        {
            if ( !IsFinite( pose.PelvisOffset ) )
            {
                return Common::MakeFormattedError<bool>( "the {} retarget pose's pelvis offset is not finite",
                                                         side );
            }

            std::unordered_set<std::string> seen;
            seen.reserve( pose.BoneOffsets.size() );

            for ( size_t i = 0; i < pose.BoneOffsets.size(); ++i )
            {
                const auto& row = pose.BoneOffsets[i];
                if ( row.Bone.empty() )
                {
                    return Common::MakeFormattedError<bool>(
                         "the {} retarget pose's offset {} names no bone", side, i );
                }
                if ( !IsFinite( row.Rotation ) )
                {
                    return Common::MakeFormattedError<bool>(
                         "the {} retarget pose's offset for bone '{}' is not finite", side, row.Bone );
                }
                if ( glm::length( row.Rotation ) <= 1e-6F )
                {
                    return Common::MakeFormattedError<bool>(
                         "the {} retarget pose's offset for bone '{}' has length {} and cannot be a "
                         "rotation",
                         side, row.Bone, glm::length( row.Rotation ) );
                }
                if ( !seen.insert( row.Bone ).second )
                {
                    return Common::MakeFormattedError<bool>(
                         "the {} retarget pose offsets bone '{}' twice, and which one wins has no answer "
                         "invented here",
                         side, row.Bone );
                }
            }
            return BOOLSUCCESS;
        }

        void FillRetargetPose( const RetargetPoseData& data, RetargetPose& out )
        {
            for ( const auto& row : data.BoneOffsets )
            {
                out.SetBoneRotationOffset( row.Bone, glm::normalize( row.Rotation ) );
            }
            out.SetPelvisTranslationOffset( data.PelvisOffset );
        }

        [[nodiscard]] RetargetPoseData PoseDataFrom( const RetargetPose& pose )
        {
            RetargetPoseData out;
            out.BoneOffsets.reserve( pose.GetBoneRotationOffsets().size() );
            for ( const auto& [bone, rotation] : pose.GetBoneRotationOffsets() )
            {
                out.BoneOffsets.push_back( RetargetBoneOffsetData{ bone, rotation } );
            }
            out.PelvisOffset = pose.GetPelvisTranslationOffset();
            return out;
        }
    } // namespace

    Common::BoolResultStr ValidateRetargetData( const RetargetAssetData& data )
    {
        // THE SOURCE RIG IS THE ONE FACT THIS FILE CANNOT DO WITHOUT. A retarget naming no source rig is
        // a retarget that can only ever be the identity — a feature that runs, reports success and does
        // nothing, which is the shape this whole format exists to end.
        if ( data.SourceSkeleton.empty() )
        {
            return Common::MakeFormattedError<bool>(
                 "retarget '{}' names no source rig. A retarget is a statement about two rigs and the "
                 "source one is not the entity's; it has to be named here",
                 data.Name );
        }

        // RELATIVE, AND IT IS REFUSED HERE RATHER THAN NORMALISED AT THE JOIN. An absolute path carries
        // one developer's home directory into a file that ships with the project; an escaping one names
        // content the cook did not produce. Both load perfectly on the machine that wrote them, which is
        // exactly why the refusal belongs in the format rather than in whoever happens to open it.
        {
            const std::filesystem::path rig( data.SourceSkeleton );
            if ( rig.is_absolute() || data.SourceSkeleton.starts_with( ".." ) )
            {
                return Common::MakeFormattedError<bool>(
                     "retarget '{}': source rig '{}' must be relative to the cooked meshes root and must "
                     "not escape it",
                     data.Name, data.SourceSkeleton );
            }
        }

        if ( data.SourcePelvisBone.empty() || data.TargetPelvisBone.empty() )
        {
            return Common::MakeFormattedError<bool>(
                 "retarget '{}': both pelvis bones must be named (source '{}', target '{}'). The pelvis is "
                 "stage 1 of the pipeline and its height is what the whole motion is scaled by",
                 data.Name, data.SourcePelvisBone, data.TargetPelvisBone );
        }

        if ( auto ok = ValidateRetargetPoseData( data.SourceRetargetPose, "source" ); !ok )
        {
            return Common::MakeFormattedError<bool>( "retarget '{}': {}", data.Name, ok.GetError() );
        }
        if ( auto ok = ValidateRetargetPoseData( data.TargetRetargetPose, "target" ); !ok )
        {
            return Common::MakeFormattedError<bool>( "retarget '{}': {}", data.Name, ok.GetError() );
        }

        std::unordered_set<std::string> chainNames;
        chainNames.reserve( data.Chains.size() );
        for ( size_t i = 0; i < data.Chains.size(); ++i )
        {
            const auto& chain = data.Chains[i];
            if ( chain.Name.empty() )
            {
                return Common::MakeFormattedError<bool>( "retarget '{}': chain {} has no name", data.Name, i );
            }
            if ( chain.SourceStartBone.empty() || chain.SourceEndBone.empty() ||
                 chain.TargetStartBone.empty() || chain.TargetEndBone.empty() )
            {
                return Common::MakeFormattedError<bool>(
                     "retarget '{}': chain '{}' leaves a bone unnamed (source '{}'..'{}', target "
                     "'{}'..'{}'). A chain is a run on EACH rig and half of one is not a run",
                     data.Name, chain.Name, chain.SourceStartBone, chain.SourceEndBone,
                     chain.TargetStartBone, chain.TargetEndBone );
            }
            if ( !chainNames.insert( chain.Name ).second )
            {
                return Common::MakeFormattedError<bool>(
                     "retarget '{}': two chains are called '{}', and a refusal naming that chain could "
                     "then mean either of them",
                     data.Name, chain.Name );
            }
        }

        std::unordered_set<std::string> renamed;
        renamed.reserve( data.BoneRenames.size() );
        for ( size_t i = 0; i < data.BoneRenames.size(); ++i )
        {
            const auto& row = data.BoneRenames[i];
            if ( row.TargetBone.empty() || row.SourceBone.empty() )
            {
                return Common::MakeFormattedError<bool>(
                     "retarget '{}': rename {} leaves a side unnamed (target '{}' -> source '{}')",
                     data.Name, i, row.TargetBone, row.SourceBone );
            }
            if ( !renamed.insert( row.TargetBone ).second )
            {
                return Common::MakeFormattedError<bool>(
                     "retarget '{}': target bone '{}' is renamed twice, and which source bone drives it "
                     "has no answer invented here",
                     data.Name, row.TargetBone );
            }
        }

        return BOOLSUCCESS;
    }

    Common::ResultStr<RetargetAssetData> ParseRetarget( const std::string& text )
    {
        if ( text.empty() )
        {
            return Common::MakeFormattedError<RetargetAssetData>( "the file is empty" );
        }

        // THE VERSION IS READ FIRST, ON ITS OWN, as an untyped tree — see the header. A struct imposes the
        // rest of the schema on a document whose whole problem may be that it does not match the schema.
        if ( const auto tree = rfl::json::read<rfl::Generic>( text ); tree )
        {
            if ( const auto fields = tree.value().to_object(); fields )
            {
                if ( const auto stated = fields.value().get( "FormatVersion" ); stated.has_value() )
                {
                    const auto number = stated.value().to_int();
                    if ( number.has_value() && number.value() != kRetargetVersion )
                    {
                        return Common::MakeFormattedError<RetargetAssetData>(
                             "retarget format version {} was written by a different build; this one reads "
                             "version {}",
                             number.value(), kRetargetVersion );
                    }
                }
            }
        }

        const auto parsed = rfl::json::read<RetargetAssetData>( text );
        if ( !parsed )
        {
            return Common::MakeFormattedError<RetargetAssetData>( "{}", parsed.error().what() );
        }

        RetargetAssetData data = parsed.value();

        const int32_t version = data.FormatVersion.value_or( kRetargetVersion );
        if ( version != kRetargetVersion )
        {
            return Common::MakeFormattedError<RetargetAssetData>(
                 "retarget format version {} was written by a different build; this one reads version {}",
                 version, kRetargetVersion );
        }

        if ( auto valid = ValidateRetargetData( data ); !valid )
        {
            return Common::MakeFormattedError<RetargetAssetData>( "{}", valid.GetError() );
        }

        data.FormatVersion = kRetargetVersion;
        return Common::MakeSuccess( std::move( data ) );
    }

    std::string WriteRetarget( const RetargetAssetData& data )
    {
        RetargetAssetData out = data;
        out.FormatVersion     = kRetargetVersion;
        return rfl::json::write( out, YYJSON_WRITE_PRETTY );
    }

    Common::ResultStr<RetargetAssetData> LoadRetargetFile( const std::filesystem::path& path )
    {
        auto text = Common::Utils::FileSystem::ReadFileContent( path );
        if ( !text )
        {
            return Common::MakeFormattedError<RetargetAssetData>( "cannot read retarget '{}': {}",
                                                                  path.string(), text.GetError() );
        }

        auto parsed = ParseRetarget( text.GetValue() );
        if ( !parsed )
        {
            return Common::MakeFormattedError<RetargetAssetData>( "retarget '{}': {}", path.string(),
                                                                  parsed.GetError() );
        }
        return parsed;
    }

    Common::BoolResultStr SaveRetargetFile( const std::filesystem::path& path, const RetargetAssetData& data )
    {
        if ( auto valid = ValidateRetargetData( data ); !valid )
        {
            return Common::MakeFormattedError<bool>( "refusing to write retarget '{}': {}", path.string(),
                                                     valid.GetError() );
        }
        return Common::Utils::FileSystem::WriteContentToFileAtomic( path, WriteRetarget( data ) );
    }

    Common::ResultStr<RetargetSetup> BuildRetargetSetup( const RetargetAssetData& data )
    {
        if ( auto valid = ValidateRetargetData( data ); !valid )
        {
            return Common::MakeFormattedError<RetargetSetup>( "{}", valid.GetError() );
        }

        RetargetSetup setup;
        setup.SourcePelvisBone = data.SourcePelvisBone;
        setup.TargetPelvisBone = data.TargetPelvisBone;

        FillRetargetPose( data.SourceRetargetPose, setup.SourceRetargetPose );
        FillRetargetPose( data.TargetRetargetPose, setup.TargetRetargetPose );

        setup.Chains.reserve( data.Chains.size() );
        for ( const auto& chain : data.Chains )
        {
            setup.Chains.push_back( RetargetChain{ chain.Name, chain.SourceStartBone, chain.SourceEndBone,
                                                   chain.TargetStartBone, chain.TargetEndBone,
                                                   chain.DriveWithIK } );
        }

        for ( const auto& row : data.BoneRenames )
        {
            setup.BoneRenames.emplace( row.TargetBone, row.SourceBone );
        }

        return Common::MakeSuccess( std::move( setup ) );
    }

    RetargetAssetData BuildDataFromRetargetSetup( const std::string& name, const std::string& sourceSkeleton,
                                                  const RetargetSetup& setup )
    {
        RetargetAssetData out;
        out.FormatVersion      = kRetargetVersion;
        out.Name               = name;
        out.SourceSkeleton     = sourceSkeleton;
        out.SourcePelvisBone        = setup.SourcePelvisBone;
        out.TargetPelvisBone        = setup.TargetPelvisBone;
        out.SourceRetargetPose      = PoseDataFrom( setup.SourceRetargetPose );
        out.TargetRetargetPose      = PoseDataFrom( setup.TargetRetargetPose );

        out.Chains.reserve( setup.Chains.size() );
        for ( const auto& chain : setup.Chains )
        {
            out.Chains.push_back( RetargetChainData{ chain.Name, chain.SourceStartBone, chain.SourceEndBone,
                                                     chain.TargetStartBone, chain.TargetEndBone,
                                                     chain.DriveWithIK } );
        }

        out.BoneRenames.reserve( setup.BoneRenames.size() );
        for ( const auto& [targetBone, sourceBone] : setup.BoneRenames )
        {
            out.BoneRenames.push_back( RetargetBoneRenameData{ targetBone, sourceBone } );
        }
        return out;
    }
} // namespace Desert::Assets::Serialization
