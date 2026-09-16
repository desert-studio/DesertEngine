#include "ControlHierarchy.hpp"

#include <Engine/Animation/Skeleton.hpp>

#include <algorithm>
#include <cmath>

namespace Desert::Animation
{
    namespace
    {
        [[nodiscard]] bool Finite( const glm::vec3& v )
        {
            return std::isfinite( v.x ) && std::isfinite( v.y ) && std::isfinite( v.z );
        }

        [[nodiscard]] bool Finite( const BoneTransform& t )
        {
            return Finite( t.Translation ) && Finite( t.Scale ) && std::isfinite( t.Rotation.x ) &&
                   std::isfinite( t.Rotation.y ) && std::isfinite( t.Rotation.z ) && std::isfinite( t.Rotation.w );
        }
    } // namespace

    glm::mat4 BlendSpaces( const std::vector<BoneTransform>& spaces, const std::vector<float>& weights )
    {
        if ( spaces.empty() || spaces.size() != weights.size() )
        {
            return glm::mat4( 1.0F );
        }

        float total = 0.0F;
        for ( const float weight : weights )
        {
            total += weight;
        }
        if ( !( std::abs( total ) > 1e-6F ) )
        {
            return glm::mat4( 1.0F );
        }

        glm::vec3 translation( 0.0F );
        glm::vec3 scale( 0.0F );
        glm::quat rotation( 0.0F, 0.0F, 0.0F, 0.0F );
        glm::quat reference( 1.0F, 0.0F, 0.0F, 0.0F );
        bool      haveReference = false;

        for ( size_t i = 0; i < spaces.size(); ++i )
        {
            const float          normalised = weights[i] / total;
            const BoneTransform& t          = spaces[i];

            translation += t.Translation * normalised;
            scale += t.Scale * normalised;

            // SIGN ALIGNMENT AGAINST THE FIRST CONTRIBUTOR. q and -q are the same rotation and opposite
            // vectors: adding them unaligned cancels towards zero, and the blend of two nearly equal
            // spaces would come out as an arbitrary rotation rather than as either of them.
            glm::quat q = t.Rotation;
            if ( !haveReference )
            {
                reference     = q;
                haveReference = true;
            }
            else if ( glm::dot( reference, q ) < 0.0F )
            {
                q = -q;
            }
            rotation += q * normalised;
        }

        if ( glm::dot( rotation, rotation ) < 1e-12F )
        {
            return glm::mat4( 1.0F );
        }

        BoneTransform blended;
        blended.Translation = translation;
        blended.Scale       = scale;
        blended.Rotation    = glm::normalize( rotation );
        return blended.ToMatrix();
    }

    Common::ResultStr<uint32_t> ControlHierarchy::Add( ControlElement element )
    {
        if ( element.Name.empty() )
        {
            return Common::MakeFormattedError<uint32_t>(
                 "a control with no name cannot be selected, keyed or refused by name" );
        }
        if ( Find( element.Name ) != INVALID )
        {
            return Common::MakeFormattedError<uint32_t>(
                 "a control named '{}' is already in this rig; two controls under one name are one thing "
                 "when keyed and two when selected",
                 element.Name );
        }
        if ( !Finite( element.Offset ) || !Finite( element.Pose ) )
        {
            return Common::MakeFormattedError<uint32_t>( "control '{}' has a non-finite offset or pose",
                                                         element.Name );
        }

        const auto self = static_cast<uint32_t>( m_Controls.size() );
        for ( const ControlSpace& space : element.Parents )
        {
            if ( space.Kind == ControlSpaceKind::Control )
            {
                // A PARENT MUST ALREADY EXIST, which is what makes a cycle inexpressible rather than
                // detected: `Add` only ever sees indices below its own, so the adjacency is acyclic by
                // construction and there is no traversal that can loop. A rig format that allows forward
                // references would need the search; this one does not, and refusing here says so.
                if ( space.Index >= self )
                {
                    return Common::MakeFormattedError<uint32_t>(
                         "control '{}' parents to control {}, which does not exist yet — a control's parents "
                         "are added before it, so a cycle cannot be written down",
                         element.Name, space.Index );
                }
            }
            if ( !std::isfinite( space.Weight ) )
            {
                return Common::MakeFormattedError<uint32_t>( "control '{}' has a non-finite space weight",
                                                             element.Name );
            }
        }

        m_Controls.push_back( std::move( element ) );
        m_Dependents.emplace_back();
        m_Global.emplace_back( 1.0F );
        m_Resolved.push_back( 0 );

        for ( const ControlSpace& space : m_Controls.back().Parents )
        {
            if ( space.Kind == ControlSpaceKind::Control )
            {
                m_Dependents[space.Index].push_back( self );
            }
        }
        return Common::MakeSuccess( self );
    }

    uint32_t ControlHierarchy::Find( const std::string& name ) const
    {
        for ( size_t i = 0; i < m_Controls.size(); ++i )
        {
            if ( m_Controls[i].Name == name )
            {
                return static_cast<uint32_t>( i );
            }
        }
        return INVALID;
    }

    const std::vector<uint32_t>& ControlHierarchy::Dependents( uint32_t control ) const
    {
        static const std::vector<uint32_t> none;
        return control < m_Dependents.size() ? m_Dependents[control] : none;
    }

    bool ControlHierarchy::Resolved( uint32_t control ) const
    {
        return control < m_Resolved.size() && m_Resolved[control] != 0;
    }

    void ControlHierarchy::Dirty( uint32_t control )
    {
        if ( control >= m_Resolved.size() )
        {
            return;
        }
        // PROPAGATED, NOT BLANKET (report 01 §(c)1), AND ITERATIVE. A stack rather than a recursive call:
        // `ComponentPose::Get` walks its parent chain the same way and says why — a member scratch buffer
        // means a per-frame re-evaluation allocates nothing, and the depth of a rig is data rather than
        // something this code gets to assume about the stack it is running on.
        //
        // No visited set is needed, and that is a property of the structure rather than an optimisation: a
        // control's parents are added before it, so every dependent index is strictly greater than the
        // control it depends on, and the walk cannot come back to where it started.
        m_Scratch.clear();
        m_Scratch.push_back( control );
        while ( !m_Scratch.empty() )
        {
            const uint32_t current = m_Scratch.back();
            m_Scratch.pop_back();
            m_Resolved[current] = 0;
            for ( const uint32_t dependent : m_Dependents[current] )
            {
                m_Scratch.push_back( dependent );
            }
        }
    }

    Common::BoolResultStr ControlHierarchy::SetPose( uint32_t control, const BoneTransform& pose )
    {
        if ( control >= m_Controls.size() )
        {
            return Common::MakeFormattedError<bool>( "no control {} in a rig of {}", control, m_Controls.size() );
        }
        if ( !Finite( pose ) )
        {
            return Common::MakeFormattedError<bool>( "control '{}': a non-finite pose", m_Controls[control].Name );
        }
        m_Controls[control].Pose = pose;
        Dirty( control );
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr ControlHierarchy::SetOffset( uint32_t control, const BoneTransform& offset )
    {
        if ( control >= m_Controls.size() )
        {
            return Common::MakeFormattedError<bool>( "no control {} in a rig of {}", control, m_Controls.size() );
        }
        if ( !Finite( offset ) )
        {
            return Common::MakeFormattedError<bool>( "control '{}': a non-finite offset",
                                                     m_Controls[control].Name );
        }
        m_Controls[control].Offset = offset;
        Dirty( control );
        return Common::MakeSuccess( true );
    }

    glm::mat4 ControlHierarchy::ParentSpace( uint32_t control )
    {
        const ControlElement& element = m_Controls[control];

        std::vector<glm::mat4> spaces;
        std::vector<float>     weights;
        spaces.reserve( element.Parents.size() );
        weights.reserve( element.Parents.size() );

        for ( const ControlSpace& space : element.Parents )
        {
            if ( !( std::abs( space.Weight ) > 0.0F ) )
            {
                continue; // an inert slot contributes nothing and must not drag the blend towards identity
            }
            switch ( space.Kind )
            {
                case ControlSpaceKind::Component:
                    spaces.emplace_back( 1.0F );
                    break;
                case ControlSpaceKind::Bone:
                    // FROM THE SNAPSHOT `Evaluate` TOOK, not from a stored pose — see the header.
                    spaces.push_back( space.Index < m_BoneSpace.size() ? m_BoneSpace[space.Index]
                                                                       : glm::mat4( 1.0F ) );
                    break;
                case ControlSpaceKind::Control:
                    // ALREADY RESOLVED BY THE CALLER, by construction: the only caller resolves the needed
                    // set in ascending index order and a parent's index is always smaller. Reading the
                    // cache here rather than calling back into the resolver is what takes the two out of a
                    // recursive call chain — `misc-no-recursion` was right that one was there.
                    spaces.push_back( m_Global[space.Index] );
                    break;
            }
            weights.push_back( space.Weight );
        }

        if ( spaces.empty() )
        {
            return glm::mat4( 1.0F );
        }
        if ( spaces.size() == 1 )
        {
            // ONE SPACE IS NOT A BLEND. Going through the blend would decompose and recompose it, and a
            // TRS round trip is not the identity on a matrix carrying shear — so a single-parent control
            // would drift away from the bone it is supposed to be exactly on. It is also the only path
            // that can carry a mirrored space through unharmed, which is the honest thing to do with one.
            return spaces.front();
        }

        // DECOMPOSED HERE, WHERE THE CONTROL HAS A NAME. This loop used to live inside the blend, which
        // could only answer a refusal with `continue` — and a skipped space still counted in the weight
        // total, so the blend came back quietly scaled towards nothing. A census found it (GpuWriteCensus,
        // "no refusal is answered with a bare continue") and it was mine.
        std::vector<BoneTransform> transforms;
        transforms.reserve( spaces.size() );
        for ( size_t i = 0; i < spaces.size(); ++i )
        {
            auto decomposed = BoneTransform::FromMatrix( spaces[i] );
            if ( !decomposed.IsSuccess() )
            {
                if ( m_StructureError.empty() )
                {
                    m_StructureError = "control '" + element.Name + "': parent space " + std::to_string( i ) +
                                       " cannot be blended — " + decomposed.GetError();
                }
                return glm::mat4( 1.0F );
            }
            transforms.push_back( decomposed.GetValue() );
        }
        return BlendSpaces( transforms, weights );
    }

    glm::mat4 ControlHierarchy::GetGlobalTransform( uint32_t control )
    {
        if ( control >= m_Controls.size() )
        {
            return glm::mat4( 1.0F );
        }
        if ( m_Resolved[control] != 0 )
        {
            return m_Global[control];
        }

        // RESOLVE WHAT THIS ONE NEEDS, PARENTS FIRST, WITHOUT RECURSING. The needed set is collected by
        // walking control-parents upwards; because a control's parents are added before it, every index
        // collected is strictly smaller than the one that asked for it, so resolving the set in ASCENDING
        // order resolves every parent before its child. That ordering IS the algorithm — there is no
        // dependency sort here because `Add` already did it.
        m_Needed.clear();
        m_Scratch.clear();
        m_Scratch.push_back( control );
        while ( !m_Scratch.empty() )
        {
            const uint32_t current = m_Scratch.back();
            m_Scratch.pop_back();
            if ( m_Resolved[current] != 0 )
            {
                continue;
            }
            m_Needed.push_back( current );
            for ( const ControlSpace& space : m_Controls[current].Parents )
            {
                if ( space.Kind == ControlSpaceKind::Control && std::abs( space.Weight ) > 0.0F )
                {
                    m_Scratch.push_back( space.Index );
                }
            }
        }

        std::sort( m_Needed.begin(), m_Needed.end() );
        m_Needed.erase( std::unique( m_Needed.begin(), m_Needed.end() ), m_Needed.end() );
        for ( const uint32_t current : m_Needed )
        {
            const ControlElement& element = m_Controls[current];
            m_Global[current]   = ParentSpace( current ) * element.Offset.ToMatrix() * element.Pose.ToMatrix();
            m_Resolved[current] = 1;
        }
        return m_Global[control];
    }

    Common::BoolResultStr ControlHierarchy::SetGlobalTransform( uint32_t control, const glm::mat4& global )
    {
        if ( control >= m_Controls.size() )
        {
            return Common::MakeFormattedError<bool>( "no control {} in a rig of {}", control, m_Controls.size() );
        }

        // RESOLVE FIRST, THEN MEASURE AGAINST WHAT WAS RESOLVED. A write READS: the back-solve below runs
        // through this control's parent space, and a parent nobody has read yet this evaluation holds
        // whatever its cache held last frame. Asking for this control's own global is what walks its
        // ancestors — the one call that makes `ParentSpace`'s cache reads true for the line after it.
        (void)GetGlobalTransform( control );

        // THE ONLY INVERSION OF THE OFFSET IN THIS FILE (report 01 §(c)3 counts the write paths in UE and
        // finds several). Nothing global is stored: the answer lands in `Pose`, and the next read rebuilds
        // the same global from it.
        const glm::mat4 parent = ParentSpace( control );
        const glm::mat4 local  = glm::inverse( parent * m_Controls[control].Offset.ToMatrix() ) * global;

        const auto decomposed = BoneTransform::FromMatrix( local );
        if ( !decomposed.IsSuccess() )
        {
            return Common::MakeFormattedError<bool>(
                 "control '{}': the transform asked for does not decompose into translation, rotation and "
                 "scale in this control's space ({})",
                 m_Controls[control].Name, decomposed.GetError() );
        }
        return SetPose( control, decomposed.GetValue() );
    }

    Common::BoolResultStr ControlHierarchy::SetSpaceWeights( uint32_t control, const std::vector<float>& weights,
                                                             bool keepWorld )
    {
        if ( control >= m_Controls.size() )
        {
            return Common::MakeFormattedError<bool>( "no control {} in a rig of {}", control, m_Controls.size() );
        }
        ControlElement& element = m_Controls[control];
        if ( weights.size() != element.Parents.size() )
        {
            return Common::MakeFormattedError<bool>(
                 "control '{}' has {} space(s) and {} weight(s) were given; a weight list is one number per "
                 "declared space, and adding a space is a rig edit rather than an animation one",
                 element.Name, element.Parents.size(), weights.size() );
        }

        float total = 0.0F;
        for ( const float weight : weights )
        {
            if ( !std::isfinite( weight ) )
            {
                return Common::MakeFormattedError<bool>( "control '{}': a non-finite space weight", element.Name );
            }
            total += std::abs( weight );
        }
        if ( !( total > 1e-6F ) )
        {
            return Common::MakeFormattedError<bool>(
                 "control '{}': every space weight is zero. A control with no space is not 'in world space' "
                 "— its parent transform is undefined, and identity would be a silent wrong answer. Declare "
                 "a Component space and weight that.",
                 element.Name );
        }

        // WHERE IT IS NOW, read BEFORE the weights change — the whole of "switch space without a twitch".
        const glm::mat4 before = keepWorld ? GetGlobalTransform( control ) : glm::mat4( 1.0F );

        for ( size_t i = 0; i < weights.size(); ++i )
        {
            element.Parents[i].Weight = weights[i];
        }
        Dirty( control );

        if ( !keepWorld )
        {
            return Common::MakeSuccess( true );
        }
        return SetGlobalTransform( control, before );
    }

    void ControlHierarchy::Evaluate( const Skeleton& skeleton, ComponentPose& pose )
    {
        std::fill( m_Resolved.begin(), m_Resolved.end(), static_cast<uint8_t>( 0 ) );

        // DOES THIS RIG FIT THIS SKELETON? The first moment both halves are present, and the only one: a
        // control names its bone by INDEX, so the same rig over a shorter skeleton resolves those spaces to
        // identity and parks every control at the origin — a silent wrong answer with no bad value in it.
        m_StructureError.clear();
        const size_t bones = skeleton.GetBones().size();
        m_BoneSpace.assign( bones, glm::mat4( 1.0F ) );

        // ONE PASS, TWO JOBS, AND THE ONLY PLACE THE POSE IS TOUCHED. It asks "does this rig fit this
        // skeleton?" and takes the bone spaces the rig actually names — so nothing is kept afterwards but
        // matrices this object owns, and the class has no pointer whose lifetime it would owe the
        // ownership register an argument for.
        //
        // Only the bones the rig NAMES are converted, which is what keeps `ComponentPose`'s laziness
        // meaningful: a hundred-bone rig with four bone-parented controls converts four chains.
        for ( const ControlElement& element : m_Controls )
        {
            for ( const ControlSpace& space : element.Parents )
            {
                if ( space.Kind != ControlSpaceKind::Bone )
                {
                    continue;
                }
                if ( space.Index >= bones )
                {
                    // A CONTROL NAMES ITS BONE BY INDEX, so the same rig over a shorter skeleton would
                    // resolve that space to identity and park the control at the origin — a wrong answer
                    // with no bad value in it, which is the only kind that has to be SAID.
                    m_StructureError = "control '" + element.Name + "' is parented to bone " +
                                       std::to_string( space.Index ) + ", and this skeleton has " +
                                       std::to_string( bones ) +
                                       " — the rig was built against a different skeleton";
                    return;
                }
                m_BoneSpace[space.Index] = pose.Get( space.Index );
            }
        }
    }
} // namespace Desert::Animation
