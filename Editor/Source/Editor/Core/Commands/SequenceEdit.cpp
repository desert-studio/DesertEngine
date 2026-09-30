#include "SequenceEdit.hpp"

#include <Common/Core/Logger.hpp>

#include <Engine/Animation/Animator.hpp>
#include <Engine/ECS/Components.hpp>

#include <algorithm>
#include <utility>
#include <variant>

namespace Desert::Editor
{
    namespace Timeline = Animation::Timeline;

    namespace
    {
        template <typename T>
        [[nodiscard]] bool SameList( const std::vector<T>& a, const std::vector<T>& b )
        {
            return std::ranges::equal( a, b, []( const T& x, const T& y ) { return SameStoredValue( x, y ); } );
        }

        [[nodiscard]] bool SameGuid( const Common::Content::AssetGuid& a, const Common::Content::AssetGuid& b )
        {
            const auto& [aHi, aLo] = a;
            const auto& [bHi, bLo] = b;
            return aHi == bHi && aLo == bLo;
        }

        [[nodiscard]] bool SameGuid( const Timeline::BindingGuid& a, const Timeline::BindingGuid& b )
        {
            const auto& [aValue] = a;
            const auto& [bValue] = b;
            return SameGuid( aValue, bValue );
        }

        /// Two values of one `std::variant`: same alternative, and that alternative's stored value equal.
        template <typename... Ts>
        [[nodiscard]] bool SameAlternative( const std::variant<Ts...>& x, const std::variant<Ts...>& y )
        {
            if ( x.index() != y.index() )
            {
                return false;
            }
            return std::visit(
                 [&y]( const auto& value ) -> bool
                 {
                     using T = std::decay_t<decltype( value )>;
                     return SameStoredValue( value, std::get<T>( y ) );
                 },
                 x );
        }

        /// A copy of @p sequence without its tracks — what the header half of an entry stores.
        [[nodiscard]] Timeline::Sequence HeaderOf( const Timeline::Sequence& sequence )
        {
            Timeline::Sequence header;
            header.Host        = sequence.Host;
            header.TickRate    = sequence.TickRate;
            header.DisplayRate = sequence.DisplayRate;
            header.Start       = sequence.Start;
            header.End         = sequence.End;
            header.Bindings    = sequence.Bindings;
            return header;
        }
    } // namespace

    // ── The census ────────────────────────────────────────────────────────────────────────────────────

    bool SameStoredValue( const Animation::ScalarKey& a, const Animation::ScalarKey& b )
    {
        const auto& [aTick, aValue, aArrive, aLeave, aArriveW, aLeaveW, aInterp, aMode] = a;
        const auto& [bTick, bValue, bArrive, bLeave, bArriveW, bLeaveW, bInterp, bMode] = b;
        return aTick == bTick && aValue == bValue && aArrive == bArrive && aLeave == bLeave &&
               aArriveW == bArriveW && aLeaveW == bLeaveW && aInterp == bInterp && aMode == bMode;
    }

    bool SameStoredValue( const Timeline::FloatChannel& a, const Timeline::FloatChannel& b )
    {
        const auto& [aKeys, aDefault] = a;
        const auto& [bKeys, bDefault] = b;
        return aDefault == bDefault && SameList( aKeys, bKeys );
    }

    bool SameStoredValue( const Timeline::VectorChannel& a, const Timeline::VectorChannel& b )
    {
        const auto& [aX, aY, aZ] = a;
        const auto& [bX, bY, bZ] = b;
        return SameStoredValue( aX, bX ) && SameStoredValue( aY, bY ) && SameStoredValue( aZ, bZ );
    }

    bool SameStoredValue( const Timeline::RotationChannel& a, const Timeline::RotationChannel& b )
    {
        const auto& [aX, aY, aZ, aW] = a;
        const auto& [bX, bY, bZ, bW] = b;
        return SameStoredValue( aX, bX ) && SameStoredValue( aY, bY ) && SameStoredValue( aZ, bZ ) &&
               SameStoredValue( aW, bW );
    }

    bool SameStoredValue( const Timeline::TransformChannel& a, const Timeline::TransformChannel& b )
    {
        const auto& [aT, aR, aS] = a;
        const auto& [bT, bR, bS] = b;
        return SameStoredValue( aT, bT ) && SameStoredValue( aR, bR ) && SameStoredValue( aS, bS );
    }

    bool SameStoredValue( const Timeline::BoolChannel& a, const Timeline::BoolChannel& b )
    {
        const auto& [aBits] = a;
        const auto& [bBits] = b;
        return SameStoredValue( aBits, bBits );
    }

    bool SameStoredValue( const Timeline::EventKey& a, const Timeline::EventKey& b )
    {
        const auto& [aTick, aDuration, aName, aRow] = a;
        const auto& [bTick, bDuration, bName, bRow] = b;
        return aTick == bTick && aDuration == bDuration && aName == bName && aRow == bRow;
    }

    bool SameStoredValue( const Timeline::EventChannel& a, const Timeline::EventChannel& b )
    {
        const auto& [aKeys] = a;
        const auto& [bKeys] = b;
        return SameList( aKeys, bKeys );
    }

    bool SameStoredValue( const Timeline::Channel& a, const Timeline::Channel& b )
    {
        return SameAlternative( a, b );
    }

    bool SameStoredValue( const Timeline::AnimationSectionContent& a, const Timeline::AnimationSectionContent& b )
    {
        const auto& [aClip, aOffset, aRate, aLoop] = a;
        const auto& [bClip, bOffset, bRate, bLoop] = b;
        return SameGuid( aClip, bClip ) && aOffset == bOffset && aRate == bRate && aLoop == bLoop;
    }

    bool SameStoredValue( const Timeline::CameraCutSectionContent& a, const Timeline::CameraCutSectionContent& b )
    {
        const auto& [aCamera] = a;
        const auto& [bCamera] = b;
        return SameGuid( aCamera, bCamera );
    }

    bool SameStoredValue( const Timeline::Section& a, const Timeline::Section& b )
    {
        const auto& [aStart, aEnd, aBlend, aWeight, aRow, aName, aContent] = a;
        const auto& [bStart, bEnd, bBlend, bWeight, bRow, bName, bContent] = b;
        if ( !( aStart == bStart && aEnd == bEnd && aBlend == bBlend && aRow == bRow && aName == bName &&
                SameList( aWeight, bWeight ) ) )
        {
            return false;
        }
        // The alternative's index is part of the value; a channel's own alternative is the next level.
        return SameAlternative( aContent, bContent );
    }

    bool SameStoredValue( const Timeline::Binding& a, const Timeline::Binding& b )
    {
        const auto& [aGuid, aKind, aLocator, aLabel, aParent] = a;
        const auto& [bGuid, bKind, bLocator, bLabel, bParent] = b;
        return SameGuid( aGuid, bGuid ) && aKind == bKind && aLocator == bLocator && aLabel == bLabel &&
               SameGuid( aParent, bParent );
    }

    bool SameStoredValue( const Timeline::Track& a, const Timeline::Track& b )
    {
        const auto& [aBinding, aProperty, aKind, aSections, aMuted] = a;
        const auto& [bBinding, bProperty, bKind, bSections, bMuted] = b;
        return SameGuid( aBinding, bBinding ) && aProperty == bProperty && aKind == bKind && aMuted == bMuted &&
               SameList( aSections, bSections );
    }

    bool SameStoredHeader( const Timeline::Sequence& a, const Timeline::Sequence& b )
    {
        // `Tracks` is compared per track by the caller and `Revision` is not stored: both named, both skipped.
        const auto& [aHost, aTick, aDisplay, aStart, aEnd, aBindings, aTracks, aRevision] = a;
        const auto& [bHost, bTick, bDisplay, bStart, bEnd, bBindings, bTracks, bRevision] = b;
        return aHost == bHost && aTick == bTick && aDisplay == bDisplay && aStart == bStart && aEnd == bEnd &&
               SameList( aBindings, bBindings );
    }

    bool SameStoredValue( const Timeline::Sequence& a, const Timeline::Sequence& b )
    {
        return SameStoredHeader( a, b ) && SameList( a.Tracks, b.Tracks );
    }

    bool SameStoredValue( const Animation::BoneTransform& a, const Animation::BoneTransform& b )
    {
        const auto& [aT, aR, aS] = a;
        const auto& [bT, bR, bS] = b;
        return aT == bT && aR == bR && aS == bS;
    }

    bool SameStoredValue( const Animation::LocalPose& a, const Animation::LocalPose& b )
    {
        if ( a.Size() != b.Size() )
        {
            return false;
        }
        for ( size_t i = 0; i < a.Size(); ++i )
        {
            if ( !SameStoredValue( a[i], b[i] ) )
            {
                return false;
            }
        }
        return true;
    }

    // ── Owners ────────────────────────────────────────────────────────────────────────────────────────

    SequenceOwner OwnerOf( Animation::AnimationClip* clip )
    {
        SequenceOwner owner;
        owner.Identity = clip;
        owner.Resolve  = [clip]() -> Timeline::Sequence* { return clip != nullptr ? &clip->Sequence : nullptr; };
        owner.Volatile = true;
        owner.Name     = clip != nullptr ? clip->AnimationName : std::string{};
        return owner;
    }

    SequenceOwner OwnerOf( ECS::UIAnimData* animation )
    {
        SequenceOwner owner;
        owner.Identity = animation;
        owner.Resolve  = [animation]() -> Timeline::Sequence*
        { return animation != nullptr ? &animation->Sequence : nullptr; };
        owner.AfterRestore = [animation]()
        {
            if ( animation != nullptr )
            {
                animation->Playback.reset();
            }
        };
        owner.Volatile = true;
        owner.Name     = "UI animation";
        return owner;
    }

    // ── SequenceEditCommand ───────────────────────────────────────────────────────────────────────────

    SequenceEditCommand::SequenceEditCommand( SequenceOwner owner, std::optional<HeaderEdit> header,
                                              std::vector<TrackDelta> tracks, size_t trackCountBefore,
                                              size_t trackCountAfter, std::optional<PoseEdit> pose )
         : m_Owner( std::move( owner ) ), m_Header( std::move( header ) ), m_Tracks( std::move( tracks ) ),
           m_TrackCountBefore( trackCountBefore ), m_TrackCountAfter( trackCountAfter ),
           m_Pose( std::move( pose ) )
    {
    }

    bool SequenceEditCommand::Undo()
    {
        return Apply( true );
    }

    bool SequenceEditCommand::Redo()
    {
        return Apply( false );
    }

    std::string SequenceEditCommand::GetLabel() const
    {
        if ( m_Tracks.empty() && !m_Header.has_value() )
        {
            return "Pose bone";
        }
        return m_Owner.Name.empty() ? std::string( "Edit sequence" ) : std::format( "Edit {}", m_Owner.Name );
    }

    bool SequenceEditCommand::Apply( const bool undo )
    {
        Timeline::Sequence* sequence = m_Owner.Resolve ? m_Owner.Resolve() : nullptr;
        if ( sequence == nullptr )
        {
            LOG_ERROR( "[SequenceUndo] '{}' no longer resolves; the entry is discarded", m_Owner.Name );
            return false;
        }

        // Check the pose half BEFORE writing anything: an entry that can restore only one half must restore
        // neither, or the skeleton on screen and the clip disagree.
        std::optional<Animation::LocalPose> pose;
        if ( m_Pose.has_value() )
        {
            if ( m_Pose->Animator == nullptr )
            {
                return false;
            }
            const size_t wanted = undo ? m_Pose->SizeBefore : m_Pose->SizeAfter;
            pose                = m_Pose->Animator->GetAuthoringPose();
            if ( pose->Size() != wanted )
            {
                LOG_ERROR( "[SequenceUndo] discarding an entry recorded against {} bone(s); the rig now has {}",
                           wanted, pose->Size() );
                return false;
            }
            for ( const BoneDelta& delta : m_Pose->Bones )
            {
                if ( delta.Bone < pose->Size() )
                {
                    ( *pose )[delta.Bone] = undo ? delta.Before : delta.After;
                }
            }
        }

        const uint32_t revision = sequence->Revision;
        if ( m_Header.has_value() )
        {
            std::vector<Timeline::Track> tracks = std::move( sequence->Tracks );
            *sequence                           = undo ? m_Header->Before : m_Header->After;
            sequence->Tracks                    = std::move( tracks );
        }
        sequence->Tracks.resize( undo ? m_TrackCountBefore : m_TrackCountAfter );
        for ( const TrackDelta& delta : m_Tracks )
        {
            const bool has = undo ? delta.HasBefore : delta.HasAfter;
            if ( has && delta.Index < sequence->Tracks.size() )
            {
                sequence->Tracks[delta.Index] = undo ? delta.Before : delta.After;
            }
        }
        // A restored binding list or track set is a structural edit to whoever cached against this copy.
        sequence->Revision = revision + 1;

        if ( pose.has_value() )
        {
            if ( const auto installed = m_Pose->Animator->SetAuthoringPose( *pose ); !installed.IsSuccess() )
            {
                LOG_ERROR( "[SequenceUndo] {}", installed.GetError() );
                return false;
            }
            m_Pose->Animator->ApplyLocalPose();
        }
        if ( m_Owner.AfterRestore )
        {
            m_Owner.AfterRestore();
        }
        return true;
    }

    // ── SequenceEditTransaction ───────────────────────────────────────────────────────────────────────

    Common::BoolResultStr SequenceEditTransaction::Begin( SequenceOwner owner, Animation::Animator* animator )
    {
        if ( m_Open )
        {
            return Common::MakeFormattedError<bool>(
                 "a sequence edit on '{}' is already open; transactions do not nest, because there is no "
                 "answer to which end commits the undo entry",
                 m_Owner.Name );
        }
        const Timeline::Sequence* sequence = owner.Resolve ? owner.Resolve() : nullptr;
        if ( sequence == nullptr )
        {
            return Common::MakeFormattedError<bool>( "a sequence edit needs an owner whose sequence exists ('{}' "
                                                     "resolves to nothing)",
                                                     owner.Name );
        }

        m_Before   = *sequence;
        m_Owner    = std::move( owner );
        m_Animator = animator;
        m_Open     = true;
        m_Driver   = Driver::Explicit;
        // An explicit transaction is opened BEFORE the edit it brackets, so the live buffer is the true
        // before. The edge-driven one overrides it with the baseline (Observe).
        m_PoseBefore = animator != nullptr ? animator->GetAuthoringPose() : Animation::LocalPose{};
        return Common::MakeSuccess( true );
    }

    void SequenceEditTransaction::Cancel()
    {
        m_Open       = false;
        m_Owner      = SequenceOwner{};
        m_Animator   = nullptr;
        m_Before     = Timeline::Sequence{};
        m_PoseBefore = Animation::LocalPose{};
    }

    Common::ResultStr<uint32_t> SequenceEditTransaction::End()
    {
        if ( !m_Open )
        {
            return Common::MakeFormattedError<uint32_t>(
                 "no sequence edit is open; an undo entry closed by an end that never began would hold whatever "
                 "the last interaction left behind" );
        }
        const Timeline::Sequence* after = m_Owner.Resolve ? m_Owner.Resolve() : nullptr;
        if ( after == nullptr )
        {
            const std::string name = m_Owner.Name;
            Cancel();
            return Common::MakeFormattedError<uint32_t>(
                 "the sequence of '{}' went away while its edit was open; nothing was recorded", name );
        }

        std::optional<SequenceEditCommand::HeaderEdit> header;
        if ( !SameStoredHeader( m_Before, *after ) )
        {
            header = SequenceEditCommand::HeaderEdit{ HeaderOf( m_Before ), HeaderOf( *after ) };
        }

        std::vector<SequenceEditCommand::TrackDelta> tracks;
        const size_t maxTracks = std::max( m_Before.Tracks.size(), after->Tracks.size() );
        for ( size_t i = 0; i < maxTracks; ++i )
        {
            const bool hasBefore = i < m_Before.Tracks.size();
            const bool hasAfter  = i < after->Tracks.size();
            if ( hasBefore && hasAfter && SameStoredValue( m_Before.Tracks[i], after->Tracks[i] ) )
            {
                continue;
            }
            SequenceEditCommand::TrackDelta delta;
            delta.Index     = i;
            delta.HasBefore = hasBefore;
            delta.HasAfter  = hasAfter;
            if ( hasBefore )
            {
                delta.Before = std::move( m_Before.Tracks[i] );
            }
            if ( hasAfter )
            {
                delta.After = after->Tracks[i];
            }
            tracks.push_back( std::move( delta ) );
        }
        const size_t trackCountBefore = m_Before.Tracks.size();
        const size_t trackCountAfter  = after->Tracks.size();

        std::optional<SequenceEditCommand::PoseEdit> pose;
        bool                                         poseChanged = false;
        if ( m_Animator != nullptr )
        {
            const Animation::LocalPose&   poseAfter = m_Animator->GetAuthoringPose();
            SequenceEditCommand::PoseEdit edit;
            edit.Animator       = m_Animator;
            edit.SizeBefore     = m_PoseBefore.Size();
            edit.SizeAfter      = poseAfter.Size();
            const size_t common = std::min( edit.SizeBefore, edit.SizeAfter );
            for ( size_t i = 0; i < common; ++i )
            {
                if ( !SameStoredValue( m_PoseBefore[i], poseAfter[i] ) )
                {
                    edit.Bones.push_back( { static_cast<uint32_t>( i ), m_PoseBefore[i], poseAfter[i] } );
                }
            }
            poseChanged = !edit.Bones.empty() || edit.SizeBefore != edit.SizeAfter;
            pose        = std::move( edit );
        }

        SequenceOwner owner = std::move( m_Owner );
        Cancel();

        if ( !header.has_value() && tracks.empty() && trackCountBefore == trackCountAfter && !poseChanged )
        {
            // AN INTERACTION THAT CHANGED NOTHING IS NOT AN UNDO STEP: Ctrl+Z spending a press on nothing
            // reads as undo being broken, and a gizmo click that misses would produce one every time.
            return Common::MakeSuccess( 0U );
        }

        CommandHistory::Get().PushCommand(
             std::make_unique<SequenceEditCommand>( std::move( owner ), std::move( header ), std::move( tracks ),
                                                    trackCountBefore, trackCountAfter, std::move( pose ) ) );
        return Common::MakeSuccess( 1U );
    }

    Common::ResultStr<uint32_t> SequenceEditTransaction::Observe( const SequenceOwner& owner,
                                                                  Animation::Animator* animator, const bool held )
    {
        const bool rose = held && !m_Held;
        const bool fell = !held && m_Held;
        m_Held          = held;

        uint32_t pushed = 0;
        if ( fell && m_Open && m_Driver == Driver::Edge )
        {
            const auto ended = End();
            if ( !ended.IsSuccess() )
            {
                return Common::MakeError<uint32_t>( ended.GetError() );
            }
            pushed += ended.GetValue();
        }

        if ( rose && !m_Open && owner.Resolve && owner.Resolve() != nullptr )
        {
            if ( const auto began = Begin( owner, animator ); !began.IsSuccess() )
            {
                return Common::MakeError<uint32_t>( began.GetError() );
            }
            m_Driver = Driver::Edge; // `Begin` marks it Explicit; this is the one caller that owns the edge
            if ( animator != nullptr && m_BaselineValid &&
                 m_Baseline.Size() == animator->GetAuthoringPose().Size() )
            {
                m_PoseBefore = m_Baseline;
            }
        }

        if ( !m_Open )
        {
            m_BaselineValid = animator != nullptr;
            if ( animator != nullptr )
            {
                m_Baseline = animator->GetAuthoringPose();
            }
        }
        return Common::MakeSuccess( pushed );
    }

    // ── ScopedSequenceEdit ────────────────────────────────────────────────────────────────────────────

    ScopedSequenceEdit::ScopedSequenceEdit( SequenceEditTransaction& transaction, SequenceOwner owner,
                                            Animation::Animator* animator )
         : m_Transaction( transaction )
    {
        const auto began = transaction.Begin( std::move( owner ), animator );
        m_Opened         = began.IsSuccess();
        if ( !m_Opened )
        {
            // Not silent: the edit inside the scope happens anyway, and the author deserves to know why it
            // cannot be undone.
            LOG_ERROR( "[SequenceUndo] this edit will not be undoable: {}", began.GetError() );
        }
    }

    ScopedSequenceEdit::~ScopedSequenceEdit()
    {
        if ( !m_Opened )
        {
            return;
        }
        if ( const auto ended = m_Transaction.End(); !ended.IsSuccess() )
        {
            LOG_ERROR( "[SequenceUndo] {}", ended.GetError() );
        }
    }

    size_t DropPoseRecordsFor( const Animation::Animator* animator )
    {
        if ( animator == nullptr )
        {
            return 0;
        }
        return CommandHistory::Get().DropIf(
             [animator]( const ICommand& command )
             {
                 const auto* edit = dynamic_cast<const SequenceEditCommand*>( &command );
                 return edit != nullptr && edit->PosedAnimator() == animator;
             } );
    }
} // namespace Desert::Editor
