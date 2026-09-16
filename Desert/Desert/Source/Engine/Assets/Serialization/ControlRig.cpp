#include <Engine/Assets/Serialization/ControlRig.hpp>

#include <Engine/Animation/Rig/ControlRigStage.hpp>
#include <Engine/Animation/Skeleton.hpp>
#include <Common/Core/Serialization/GlmReflection.hpp>

#include <Common/Utilities/FileSystem.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <unordered_map>
#include <unordered_set>

#include <rflcpp/rfl/json.hpp>

namespace Desert::Assets::Serialization
{
    namespace
    {
        // The ONE table mapping the file's spelling to the enum. A second copy of it — one for reading and
        // one for writing — is how a format ends up able to write a word it cannot read.
        struct SpaceKindRow
        {
            std::string_view            Text;
            Animation::ControlSpaceKind Kind;
        };

        constexpr std::array<SpaceKindRow, 3> kSpaceKinds = { {
             { "Component", Animation::ControlSpaceKind::Component },
             { "Bone", Animation::ControlSpaceKind::Bone },
             { "Control", Animation::ControlSpaceKind::Control },
        } };

        [[nodiscard]] std::optional<Animation::ControlSpaceKind> KindFromText( const std::string& text )
        {
            for ( const auto& row : kSpaceKinds )
            {
                if ( row.Text == text )
                {
                    return row.Kind;
                }
            }
            return std::nullopt;
        }

        [[nodiscard]] std::string TextFromKind( Animation::ControlSpaceKind kind )
        {
            for ( const auto& row : kSpaceKinds )
            {
                if ( row.Kind == kind )
                {
                    return std::string( row.Text );
                }
            }
            // Unreachable while the table covers the enum, which the static_assert below pins.
            return std::string( kSpaceKinds[0].Text );
        }

        static_assert( kSpaceKinds.size() == 3,
                       "every ControlSpaceKind must have exactly one spelling in the file; a kind with no "
                       "row is a rig this format can hold in memory and cannot write" );

        /// @param zeroScaleReason why a zero scale component is fatal FOR THIS FIELD. A parameter and not a
        /// sentence baked in here, because the two answers are genuinely different — a zero in an offset
        /// or a pose is a parent space that cannot be inverted, a zero in a shape transform is a control
        /// drawn flat — and one message covering both would be wrong about one of them. "A comment is not
        /// the code" applies to a refusal's text too: it is the only thing the rigger gets.
        [[nodiscard]] Common::BoolResultStr FiniteTransform( const std::string& control, const std::string& which,
                                                             const RigTransformData& t,
                                                             const std::string&      zeroScaleReason )
        {
            // A non-finite transform is a control whose global is NaN: it draws nothing, hit-tests to
            // nothing, and drives its bone to a matrix that poisons every skinning weight it touches. The
            // control and the field are named because "the rig is invalid" is a morning spent bisecting.
            for ( int c = 0; c < 3; ++c )
            {
                if ( !std::isfinite( t.Translation[c] ) || !std::isfinite( t.Scale[c] ) )
                {
                    return Common::MakeFormattedError<bool>( "control '{}' has a non-finite {} on component {}",
                                                             control, which, c );
                }
            }
            for ( int c = 0; c < 4; ++c )
            {
                if ( !std::isfinite( t.Rotation[c] ) )
                {
                    return Common::MakeFormattedError<bool>(
                         "control '{}' has a non-finite {} rotation on component {}", control, which, c );
                }
            }
            if ( t.Scale.x == 0.0f || t.Scale.y == 0.0f || t.Scale.z == 0.0f )
            {
                return Common::MakeFormattedError<bool>( "control '{}' has a zero component in its {} scale "
                                                         "({}, {}, {}); {}",
                                                         control, which, t.Scale.x, t.Scale.y, t.Scale.z,
                                                         zeroScaleReason );
            }
            return Common::MakeSuccess( true );
        }

        [[nodiscard]] Animation::BoneTransform ToBoneTransform( const RigTransformData& t )
        {
            Animation::BoneTransform out;
            out.Translation = t.Translation;
            out.Rotation    = t.Rotation;
            out.Scale       = t.Scale;
            return out;
        }

        [[nodiscard]] RigTransformData FromBoneTransform( const Animation::BoneTransform& t )
        {
            RigTransformData out;
            out.Translation = t.Translation;
            out.Rotation    = t.Rotation;
            out.Scale       = t.Scale;
            return out;
        }

        /// Refuses a parent chain that closes on itself, walking only the `Control` slots — the other two
        /// kinds terminate by construction. Iterative rather than recursive: a hand-edited file is exactly
        /// where an unbounded depth can come from, and a stack overflow is not a refusal.
        [[nodiscard]] Common::BoolResultStr RefuseCycles( const ControlRigData&                          data,
                                                          const std::unordered_map<std::string, size_t>& byName )
        {
            // 0 = unvisited, 1 = on the current path, 2 = proven acyclic. The classic colouring, because a
            // plain visited-set answers "have I been here" and not "am I inside my own subtree".
            std::vector<uint8_t> state( data.Controls.size(), 0 );
            std::vector<size_t>  stack;
            std::vector<size_t>  cursor;

            for ( size_t root = 0; root < data.Controls.size(); ++root )
            {
                if ( state[root] != 0 )
                {
                    continue;
                }
                stack.clear();
                cursor.clear();
                stack.push_back( root );
                cursor.push_back( 0 );
                state[root] = 1;

                while ( !stack.empty() )
                {
                    const size_t node  = stack.back();
                    size_t&      slot  = cursor.back();
                    const auto&  slots = data.Controls[node].Parents;

                    if ( slot >= slots.size() )
                    {
                        state[node] = 2;
                        stack.pop_back();
                        cursor.pop_back();
                        continue;
                    }

                    const ControlSpaceData& space = slots[slot];
                    ++slot;
                    if ( space.Kind != "Control" )
                    {
                        continue;
                    }

                    const auto found = byName.find( space.Target );
                    if ( found == byName.end() )
                    {
                        return Common::MakeFormattedError<bool>(
                             "control '{}' is parented to '{}', which this rig does not define",
                             data.Controls[node].Name, space.Target );
                    }

                    const size_t parent = found->second;
                    if ( state[parent] == 1 )
                    {
                        return Common::MakeFormattedError<bool>(
                             "control '{}' is parented to '{}', which closes a cycle; a control cannot be "
                             "its own ancestor because its global would have no fixed point",
                             data.Controls[node].Name, space.Target );
                    }
                    if ( state[parent] == 0 )
                    {
                        state[parent] = 1;
                        stack.push_back( parent );
                        cursor.push_back( 0 );
                    }
                }
            }
            return Common::MakeSuccess( true );
        }

        /// Controls in an order where every `Control` parent precedes its child. `ControlHierarchy::Add`
        /// refuses an unknown parent, so a file written child-first is legal content that would be refused
        /// for the wrong reason; ordering it here is what makes the file's own order not matter.
        [[nodiscard]] std::vector<size_t> ResolveOrder( const ControlRigData&                          data,
                                                        const std::unordered_map<std::string, size_t>& byName )
        {
            std::vector<size_t>  order;
            std::vector<uint8_t> emitted( data.Controls.size(), 0 );
            order.reserve( data.Controls.size() );

            std::vector<size_t> stack;
            std::vector<size_t> cursor;
            for ( size_t root = 0; root < data.Controls.size(); ++root )
            {
                if ( emitted[root] != 0 )
                {
                    continue;
                }
                stack.clear();
                cursor.clear();
                stack.push_back( root );
                cursor.push_back( 0 );

                while ( !stack.empty() )
                {
                    const size_t node  = stack.back();
                    size_t&      slot  = cursor.back();
                    const auto&  slots = data.Controls[node].Parents;

                    if ( slot >= slots.size() )
                    {
                        if ( emitted[node] == 0 )
                        {
                            emitted[node] = 1;
                            order.push_back( node );
                        }
                        stack.pop_back();
                        cursor.pop_back();
                        continue;
                    }

                    const ControlSpaceData& space = slots[slot];
                    ++slot;
                    if ( space.Kind != "Control" )
                    {
                        continue;
                    }
                    // Validate has already established the name resolves and the graph is acyclic, which
                    // is what makes this walk terminate without a depth guard of its own.
                    const size_t parent = byName.at( space.Target );
                    if ( emitted[parent] == 0 )
                    {
                        stack.push_back( parent );
                        cursor.push_back( 0 );
                    }
                }
            }
            return order;
        }
    } // namespace

    Common::BoolResultStr ValidateControlRigData( const ControlRigData& data )
    {
        if ( data.Controls.empty() )
        {
            return Common::MakeFormattedError<bool>( "rig '{}' defines no controls", data.Name );
        }

        std::unordered_map<std::string, size_t> byName;
        byName.reserve( data.Controls.size() );

        for ( size_t i = 0; i < data.Controls.size(); ++i )
        {
            const ControlElementData& control = data.Controls[i];
            if ( control.Name.empty() )
            {
                return Common::MakeFormattedError<bool>(
                     "control {} has an empty name; the name is the only key a drive, a space and a "
                     "Sequencer track bind on",
                     i );
            }
            if ( !byName.emplace( control.Name, i ).second )
            {
                return Common::MakeFormattedError<bool>(
                     "two controls are named '{}'; an animator would have two things to select that are one "
                     "thing when keyed",
                     control.Name );
            }

            constexpr const char* kSpaceReason = "the parent space it builds cannot be inverted, so a drag "
                                                 "on it or on any child would have no answer";
            if ( auto ok = FiniteTransform( control.Name, "offset", control.Offset, kSpaceReason ); !ok )
            {
                return ok;
            }
            if ( auto ok = FiniteTransform( control.Name, "pose", control.Pose, kSpaceReason ); !ok )
            {
                return ok;
            }
            if ( control.ShapeTransform.has_value() )
            {
                // REFUSED AND NOT CLAMPED. A flattened shape draws as a line or a point: invisible at any
                // zoom and unhittable, which is the identical symptom to a typo'd shape name that this
                // format already refuses by name. A knob that can reach that state silently would be the
                // "a knob that hides a defect instead of fixing it" the contract forbids.
                if ( auto ok = FiniteTransform( control.Name, "shape transform", *control.ShapeTransform,
                                                "a shape scaled to zero on an axis draws flat, which is a "
                                                "control the animator can neither see nor grab" );
                     !ok )
                {
                    return ok;
                }
            }
        }

        for ( const ControlElementData& control : data.Controls )
        {
            if ( control.Parents.empty() )
            {
                return Common::MakeFormattedError<bool>(
                     "control '{}' declares no parent space; a control with no space is not 'in world "
                     "space', it is a control whose parent transform is undefined. Declare a Component slot",
                     control.Name );
            }

            float weightSum = 0.0f;
            for ( const ControlSpaceData& space : control.Parents )
            {
                const auto kind = KindFromText( space.Kind );
                if ( !kind.has_value() )
                {
                    return Common::MakeFormattedError<bool>(
                         "control '{}' has a parent space of kind '{}', which is not one of Component, Bone "
                         "or Control",
                         control.Name, space.Kind );
                }
                if ( !std::isfinite( space.Weight ) || space.Weight < 0.0f )
                {
                    return Common::MakeFormattedError<bool>(
                         "control '{}' has a parent space with weight {}; a weight is a non-negative finite "
                         "number",
                         control.Name, space.Weight );
                }
                weightSum += space.Weight;

                if ( *kind == Animation::ControlSpaceKind::Component )
                {
                    if ( !space.Target.empty() )
                    {
                        return Common::MakeFormattedError<bool>(
                             "control '{}' has a Component space naming '{}'; the mesh's own space has no "
                             "target, and a slot that says both is two statements about one fact",
                             control.Name, space.Target );
                    }
                }
                else if ( space.Target.empty() )
                {
                    return Common::MakeFormattedError<bool>(
                         "control '{}' has a {} space that names nothing to follow", control.Name, space.Kind );
                }
                else if ( *kind == Animation::ControlSpaceKind::Control )
                {
                    if ( space.Target == control.Name )
                    {
                        return Common::MakeFormattedError<bool>( "control '{}' is parented to itself",
                                                                 control.Name );
                    }
                    if ( byName.find( space.Target ) == byName.end() )
                    {
                        return Common::MakeFormattedError<bool>(
                             "control '{}' is parented to '{}', which this rig does not define", control.Name,
                             space.Target );
                    }
                }
            }

            if ( weightSum <= 0.0f )
            {
                return Common::MakeFormattedError<bool>(
                     "control '{}' has parent spaces whose weights are all zero; the blend would be "
                     "normalised by zero and the answer would be invented",
                     control.Name );
            }
        }

        if ( auto ok = RefuseCycles( data, byName ); !ok )
        {
            return ok;
        }

        if ( data.Drives.empty() )
        {
            // The load-bearing refusal. See the header: a rig that drives no bones is a pipeline stage that
            // cannot change the pose, and such a stage passes every assertion a working one passes.
            return Common::MakeFormattedError<bool>(
                 "rig '{}' drives no bones; it would be a pipeline stage that cannot change the pose, which "
                 "is indistinguishable from a rig that works",
                 data.Name );
        }

        std::unordered_set<std::string> drivenBones;
        drivenBones.reserve( data.Drives.size() );
        for ( size_t i = 0; i < data.Drives.size(); ++i )
        {
            const ControlDriveData& drive = data.Drives[i];
            if ( drive.Control.empty() || drive.Bone.empty() )
            {
                return Common::MakeFormattedError<bool>(
                     "drive {} names control '{}' and bone '{}'; both halves are required", i, drive.Control,
                     drive.Bone );
            }
            if ( byName.find( drive.Control ) == byName.end() )
            {
                return Common::MakeFormattedError<bool>(
                     "drive {} names control '{}', which this rig does not define", i, drive.Control );
            }
            if ( !drivenBones.emplace( drive.Bone ).second )
            {
                return Common::MakeFormattedError<bool>(
                     "two drives write bone '{}'; which control wins has no answer that is not invented here",
                     drive.Bone );
            }
        }

        return Common::MakeSuccess( true );
    }

    Common::ResultStr<ControlRigData> ParseControlRig( const std::string& text )
    {
        if ( text.empty() )
        {
            return Common::MakeFormattedError<ControlRigData>( "the file is empty" );
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
                    if ( number.has_value() && number.value() != kControlRigVersion )
                    {
                        return Common::MakeFormattedError<ControlRigData>(
                             "control rig format version {} was written by a different build; this one reads "
                             "version {}",
                             number.value(), kControlRigVersion );
                    }
                }
            }
        }

        const auto parsed = rfl::json::read<ControlRigData>( text );
        if ( !parsed )
        {
            return Common::MakeFormattedError<ControlRigData>( "{}", parsed.error().what() );
        }

        ControlRigData data = parsed.value();

        const int32_t version = data.FormatVersion.value_or( kControlRigVersion );
        if ( version != kControlRigVersion )
        {
            return Common::MakeFormattedError<ControlRigData>(
                 "control rig format version {} was written by a different build; this one reads version {}",
                 version, kControlRigVersion );
        }

        if ( auto valid = ValidateControlRigData( data ); !valid )
        {
            return Common::MakeFormattedError<ControlRigData>( "{}", valid.GetError() );
        }

        data.FormatVersion = kControlRigVersion;
        return Common::MakeSuccess( std::move( data ) );
    }

    std::string WriteControlRig( const ControlRigData& data )
    {
        ControlRigData out = data;
        out.FormatVersion  = kControlRigVersion;
        return rfl::json::write( out, YYJSON_WRITE_PRETTY );
    }

    Common::ResultStr<ControlRigData> LoadControlRigFile( const std::filesystem::path& path )
    {
        auto text = Common::Utils::FileSystem::ReadFileContent( path );
        if ( !text )
        {
            return Common::MakeFormattedError<ControlRigData>( "cannot read control rig '{}': {}", path.string(),
                                                               text.GetError() );
        }

        auto parsed = ParseControlRig( text.GetValue() );
        if ( !parsed )
        {
            return Common::MakeFormattedError<ControlRigData>( "control rig '{}': {}", path.string(),
                                                               parsed.GetError() );
        }
        return parsed;
    }

    Common::BoolResultStr SaveControlRigFile( const std::filesystem::path& path, const ControlRigData& data )
    {
        if ( auto valid = ValidateControlRigData( data ); !valid )
        {
            return Common::MakeFormattedError<bool>( "refusing to write control rig '{}': {}", path.string(),
                                                     valid.GetError() );
        }
        return Common::Utils::FileSystem::WriteContentToFileAtomic( path, WriteControlRig( data ) );
    }

    Common::BoolResultStr BuildControlRig( const ControlRigData& data, const Animation::Skeleton& skeleton,
                                           Animation::ControlRigStage& out )
    {
        if ( auto valid = ValidateControlRigData( data ); !valid )
        {
            return valid;
        }

        std::unordered_map<std::string, size_t> byName;
        byName.reserve( data.Controls.size() );
        for ( size_t i = 0; i < data.Controls.size(); ++i )
        {
            byName.emplace( data.Controls[i].Name, i );
        }

        Animation::ControlHierarchy& hierarchy = out.GetHierarchy();

        // Where the file's names become this skeleton's indices, and the ONLY place they do.
        std::unordered_map<std::string, uint32_t> controlIndex;
        controlIndex.reserve( data.Controls.size() );

        for ( const size_t source : ResolveOrder( data, byName ) )
        {
            const ControlElementData& file = data.Controls[source];

            Animation::ControlElement element;
            element.Name      = file.Name;
            element.ShapeName = file.ShapeName;
            // ABSENT IS IDENTITY, spelled once, here. `RigTransformData{}` is the identity TRS (scale one,
            // unit quaternion), which is exactly the composition BuildFrame performed before this field
            // existed — that equivalence is what lets `kControlRigVersion` stay at 1.
            element.ShapeTransform = ToBoneTransform( file.ShapeTransform.value_or( RigTransformData{} ) );
            element.Offset         = ToBoneTransform( file.Offset );
            element.Pose           = ToBoneTransform( file.Pose );
            element.Parents.reserve( file.Parents.size() );

            for ( const ControlSpaceData& space : file.Parents )
            {
                Animation::ControlSpace slot;
                // Validate has established the spelling is one of the three.
                slot.Kind   = *KindFromText( space.Kind );
                slot.Weight = space.Weight;

                if ( slot.Kind == Animation::ControlSpaceKind::Bone )
                {
                    const auto bone = skeleton.FindBoneIndex( space.Target );
                    if ( !bone.has_value() )
                    {
                        // REFUSED, NOT RESOLVED TO IDENTITY. See the header: the silent version of this is
                        // every control at the origin with nothing said.
                        return Common::MakeFormattedError<bool>(
                             "rig '{}': control '{}' follows bone '{}', which this skeleton (signature {}) "
                             "does not have",
                             data.Name, file.Name, space.Target, skeleton.GetSignature() );
                    }
                    slot.Index = *bone;
                }
                else if ( slot.Kind == Animation::ControlSpaceKind::Control )
                {
                    const auto parent = controlIndex.find( space.Target );
                    if ( parent == controlIndex.end() )
                    {
                        return Common::MakeFormattedError<bool>(
                             "rig '{}': control '{}' follows control '{}', which the resolve order did not "
                             "place before it",
                             data.Name, file.Name, space.Target );
                    }
                    slot.Index = parent->second;
                }

                element.Parents.push_back( slot );
            }

            auto added = hierarchy.Add( std::move( element ) );
            if ( !added )
            {
                return Common::MakeFormattedError<bool>( "rig '{}': {}", data.Name, added.GetError() );
            }
            controlIndex.emplace( file.Name, added.GetValue() );
        }

        std::vector<Animation::ControlBoneDrive> drives;
        drives.reserve( data.Drives.size() );
        for ( const ControlDriveData& drive : data.Drives )
        {
            const auto bone = skeleton.FindBoneIndex( drive.Bone );
            if ( !bone.has_value() )
            {
                return Common::MakeFormattedError<bool>(
                     "rig '{}': control '{}' drives bone '{}', which this skeleton (signature {}) does not "
                     "have",
                     data.Name, drive.Control, drive.Bone, skeleton.GetSignature() );
            }

            Animation::ControlBoneDrive resolved;
            // Validate has established the control name is defined, and every defined control was added.
            resolved.Control = controlIndex.at( drive.Control );
            resolved.Bone    = *bone;
            drives.push_back( resolved );
        }

        if ( auto ok = out.SetDrives( skeleton, std::move( drives ) ); !ok )
        {
            return Common::MakeFormattedError<bool>( "rig '{}': {}", data.Name, ok.GetError() );
        }

        return Common::MakeSuccess( true );
    }

    Common::ResultStr<ControlRigData> BuildDataFromControlRig( const std::string&                name,
                                                               const Animation::ControlRigStage& rig,
                                                               const Animation::Skeleton&        skeleton )
    {
        const Animation::ControlHierarchy& hierarchy = rig.GetHierarchy();

        ControlRigData data;
        data.FormatVersion = kControlRigVersion;
        data.Name          = name;
        data.Controls.reserve( hierarchy.Size() );

        const auto boneName = [&skeleton]( uint32_t index ) -> Common::ResultStr<std::string>
        {
            if ( index >= skeleton.GetBones().size() )
            {
                return Common::MakeFormattedError<std::string>(
                     "bone index {} is outside this skeleton's {} bones", index, skeleton.GetBones().size() );
            }
            return Common::MakeSuccess( skeleton.GetBones()[index].Name );
        };

        for ( uint32_t i = 0; i < static_cast<uint32_t>( hierarchy.Size() ); ++i )
        {
            const Animation::ControlElement& control = hierarchy.Get( i );

            ControlElementData file;
            file.Name      = control.Name;
            file.ShapeName = control.ShapeName;
            file.Offset    = FromBoneTransform( control.Offset );
            file.Pose      = FromBoneTransform( control.Pose );

            // THE CANONICAL SPELLING OF "no shape transform" IS THE ABSENT FIELD, chosen once and here.
            // Both spellings are legal input and build the same control, so the writer picks one: a rig
            // whose controls are all default does not grow a block of ones per control, and a generation-1
            // file round-trips through this function unchanged instead of gaining fields it never had.
            if ( const RigTransformData shape = FromBoneTransform( control.ShapeTransform );
                 shape != RigTransformData{} )
            {
                file.ShapeTransform = shape;
            }
            file.Parents.reserve( control.Parents.size() );

            for ( const Animation::ControlSpace& slot : control.Parents )
            {
                ControlSpaceData space;
                space.Kind   = TextFromKind( slot.Kind );
                space.Weight = slot.Weight;

                if ( slot.Kind == Animation::ControlSpaceKind::Bone )
                {
                    auto named = boneName( slot.Index );
                    if ( !named )
                    {
                        return Common::MakeFormattedError<ControlRigData>( "control '{}': {}", control.Name,
                                                                           named.GetError() );
                    }
                    space.Target = named.GetValue();
                }
                else if ( slot.Kind == Animation::ControlSpaceKind::Control )
                {
                    if ( slot.Index >= hierarchy.Size() )
                    {
                        return Common::MakeFormattedError<ControlRigData>(
                             "control '{}' follows control index {}, which this rig does not have", control.Name,
                             slot.Index );
                    }
                    space.Target = hierarchy.Get( slot.Index ).Name;
                }

                file.Parents.push_back( std::move( space ) );
            }

            data.Controls.push_back( std::move( file ) );
        }

        data.Drives.reserve( rig.GetDrives().size() );
        for ( const Animation::ControlBoneDrive& drive : rig.GetDrives() )
        {
            if ( drive.Control >= hierarchy.Size() )
            {
                return Common::MakeFormattedError<ControlRigData>(
                     "a drive names control index {}, which this rig does not have", drive.Control );
            }
            auto named = boneName( drive.Bone );
            if ( !named )
            {
                return Common::MakeFormattedError<ControlRigData>( "a drive: {}", named.GetError() );
            }

            ControlDriveData file;
            file.Control = hierarchy.Get( drive.Control ).Name;
            file.Bone    = named.GetValue();
            data.Drives.push_back( std::move( file ) );
        }

        if ( auto valid = ValidateControlRigData( data ); !valid )
        {
            return Common::MakeFormattedError<ControlRigData>( "{}", valid.GetError() );
        }

        return Common::MakeSuccess( std::move( data ) );
    }
} // namespace Desert::Assets::Serialization
