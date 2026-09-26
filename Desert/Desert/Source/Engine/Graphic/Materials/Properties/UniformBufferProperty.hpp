#pragma once

#include <Engine/Graphic/Materials/Properties/MaterialProperty.hpp>

#include "FieldProperty.hpp"
#include <Engine/ShaderResources/BufferFillKind.hpp>
#include <Engine/ShaderResources/UniformBuffer.hpp>

namespace Desert::Graphic
{
    // A uniform buffer that KNOWS how it is filled. See ShaderResources::BufferFillKind.hpp for why
    // that matters: the two routes (whole-block SetRawData vs field-by-field UpdateFields) disagree
    // about whether FieldProperty's local data means anything, and mixing them writes uninitialised
    // heap over live camera matrices. The buffer claims a route on its first write and refuses the
    // other one by name afterwards.
    class UniformBufferProperty : public MaterialProperty
    {
    public:
        UniformBufferProperty( const std::shared_ptr<ShaderResources::UniformBuffer>& buffer ) : m_Buffer( buffer )
        {
            m_FieldProperties.assign( buffer->GetFields().begin(), buffer->GetFields().end() );

            for ( size_t i = 0; i < m_FieldProperties.size(); ++i )
            {
                m_FieldIndexMap[m_FieldProperties[i].GetFieldInfo().Name] = i;
            }
        }

        // The backend is asked EVERY time: whether this view's set needs a write is the set's own record
        // (DescriptorCopyRecord: the copy it points at and the version it applied), not a flag here.
        void Apply( MaterialBackend* backend ) override
        {
            backend->ApplyUniformBuffer( this );
        }

        void UpdateFields()
        {
            if ( !ClaimFill( ShaderResources::FillKind::Fields, "UpdateFields" ) )
                return;

            // THE ANSWER IS READ NOW, AND THE OLD SHAPE IS WHY. This was
            // `[[maybe_unused]] const auto mapPtr = m_Buffer->MapMemory();` — a pointer taken and thrown
            // away. When the mapping had failed, every SetData below silently wrote nothing and the
            // fields were then marked CLEAN, so the material reported an upload that never happened and
            // the buffer kept last frame's contents with no line anywhere saying so.
            const auto mapped = m_Buffer->EnsureMapped();
            if ( !mapped.IsSuccess() )
            {
                LOG_ERROR( "[UB] '{}': UpdateFields has nowhere to write, so the fields stay dirty -- {}",
                           m_Buffer->GetName(), mapped.GetError() );
                return;
            }

            // THE ANSWER FROM THE WRITE ITSELF IS NOW READ, AND IT DECIDES WHETHER THE FIELD IS CLEAN.
            // This is where the refusal Г13 gave SetData terminates, on purpose and with an argument:
            // above this line there is a render-graph pass with no channel of its own, and the thing a
            // caller would DO with the failure is exactly what happens here — the field stays DIRTY, so
            // the write is attempted again next frame instead of being reported as delivered. Marking a
            // field clean after a write that refused is the same false claim EnsureMapped closed one
            // paragraph above; this closes the other half of it.
            // Only fields NEWER than what the active view's copy for this frame has applied. A copy that did
            // not exist a moment ago (EnsureMapped just made and seeded it) has applied nothing and takes every
            // written field once — the bytes its seed already holds, so that write is idempotent.
            const uint64_t applied    = m_Buffer->ActiveAppliedVersion();
            uint64_t       newest     = applied;
            bool           allWritten = true;
            std::string    firstFailure;
            for ( auto& field : m_FieldProperties )
            {
                if ( field.GetVersion() <= applied )
                    continue;

                const auto wrote = m_Buffer->SetData( field.GetLocalData().Data, field.GetFieldInfo().Size,
                                                      field.GetFieldInfo().Offset );
                if ( !wrote.IsSuccess() )
                {
                    allWritten = false;
                    if ( firstFailure.empty() )
                        firstFailure = field.GetFieldInfo().Name + ": " + wrote.GetError();
                    continue;
                }
                newest = field.GetVersion() > newest ? field.GetVersion() : newest;
            }

            // The copy's version is NOT advanced past a refused write, so the next frame retries every field
            // this copy is still behind on (the delivered ones again, harmlessly).
            if ( !allWritten )
            {
                LOG_ERROR( "[UB] '{}': one or more fields were not uploaded and stay pending for this view -- {}",
                           m_Buffer->GetName(), firstFailure );
                return;
            }

            m_Buffer->NoteActiveApplied( newest );
            NoteWritten();
        }

        // True while the ACTIVE view's copy for this frame is behind some field's latest write. Each
        // (view x frame) copy catches up on its own, however late it first records.
        //
        // A whole-filled buffer answers NO regardless of what its field counters say. Those counters
        // start dirty for every field of every buffer (FieldProperty's constructor), so before this
        // line a generic "flush every UB with dirty fields" loop -- and there were four of them --
        // picked up CameraUB and ShadowUB on the opening frames and pushed uninitialised heap into
        // them. That is the same loop DataDrivenMaterial::m_ParamBuffers used to be a hand-maintained
        // exception list for; the exception is now a consequence of what the buffer is.
        bool HasDirtyFields() const
        {
            if ( m_Fill == ShaderResources::FillKind::Whole )
                return false;

            const uint64_t applied = m_Buffer->ActiveAppliedVersion();
            for ( const auto& field : m_FieldProperties )
            {
                if ( field.GetVersion() > applied )
                    return true;
            }
            return false;
        }

        /// Which route this buffer has claimed. Unclaimed until its first write.
        ShaderResources::FillKind GetFillKind() const
        {
            return m_Fill;
        }

        /// Write one of THIS buffer's fields and claim the field-by-field route in the same step. The
        /// sanctioned way to reach a FieldProperty from outside: a bare `field->SetRawBytes(...)` writes
        /// data the buffer never learns about, which leaves the route unclaimed and lets a later
        /// whole-block write be accepted over the top of it.
        /// Returns false (writing nothing) if the buffer is whole-filled or @p field is not one of ours.
        bool WriteField( FieldProperty* field, const void* data, size_t size )
        {
            if ( !field || !data )
                return false;

            // Membership, not trust: the caller found this pair through Material::FindFieldInAnyUB, and
            // a mismatched pair would claim one buffer's route while writing another buffer's bytes.
            const FieldProperty* first = m_FieldProperties.data();
            if ( field < first || field >= first + m_FieldProperties.size() )
            {
                LOG_ERROR( "[UB] '{}' was asked to write field '{}', which belongs to another buffer",
                           m_Buffer->GetName(), field->GetFieldInfo().Name );
                return false;
            }

            if ( !ClaimFill( ShaderResources::FillKind::Fields, "a field write" ) )
                return false;

            return field->SetRawBytes( data, size );
        }

        void SetRawData( const std::byte* data, size_t size )
        {
            DESERT_VERIFY( data != nullptr, "UniformBufferProperty::SetRawData: data is null" );

            if ( !ClaimFill( ShaderResources::FillKind::Whole, "SetRawData" ) )
                return;

            const size_t bufferSize = m_Buffer->GetSize();
            if ( size > bufferSize ) // the sizes identify WHICH UB overflowed (VERIFY drops its args)
                LOG_ERROR( "[UB] SetRawData overflow: writing {} bytes into a {}-byte buffer ({} field(s), "
                           "first '{}')",
                           size, bufferSize, m_FieldProperties.size(),
                           m_FieldProperties.empty() ? "?" : m_FieldProperties[0].GetFieldInfo().Name );
            DESERT_VERIFY( size <= bufferSize,
                           "UniformBufferProperty::SetRawData: data size ({}) exceeds buffer size ({})", size,
                           bufferSize );

            // Read, not discarded — see UpdateFields above for the defect the discarded form hid.
            const auto mapped = m_Buffer->EnsureMapped();
            if ( !mapped.IsSuccess() )
            {
                LOG_ERROR( "[UB] '{}': SetRawData has nowhere to write -- {}", m_Buffer->GetName(),
                           mapped.GetError() );
                return;
            }

            // Read, and terminated here with the same argument as UpdateFields above. There is no
            // per-field dirty flag on this route to leave standing, and none is needed: a whole-block
            // buffer is rewritten from its owner's C++ struct every frame, so a refused write is retried
            // by construction on the next one. What was missing was the LINE SAYING SO — before this,
            // the frame simply rendered from whatever the buffer last held.
            const auto wrote = m_Buffer->SetData( reinterpret_cast<const void*>( data ), size, 0 );
            if ( !wrote.IsSuccess() )
                LOG_ERROR( "[UB] '{}': SetRawData delivered nothing, the shader will read the previous "
                           "frame's block -- {}",
                           m_Buffer->GetName(), wrote.GetError() );

            NoteWritten();
        }

        const auto& GetUniform() const
        {
            return m_Buffer;
        }

        FieldProperty* GetField( const std::string& name )
        {
            auto it = m_FieldIndexMap.find( name );
            if ( it != m_FieldIndexMap.end() )
            {
                return &m_FieldProperties[it->second];
            }
            return nullptr;
        }

        const FieldProperty* GetField( const std::string& name ) const
        {
            auto it = m_FieldIndexMap.find( name );
            if ( it != m_FieldIndexMap.end() )
            {
                return &m_FieldProperties[it->second];
            }
            return nullptr;
        }

    private:
        // Claim @p requested, or refuse it by name. @p operation is the wording of the caller as it
        // should read in the log ("SetRawData", "UpdateFields", "a field write").
        //
        // Loud rather than silent, and loud rather than fatal: this is a wrong picture, not a crash
        // (Docs/Clouds/DEV_CONTRACT.md 1.4), and the whole reason the defect cost a rendering session
        // is that the corruption had no symptom other than the frame. A message naming the buffer and
        // both routes turns it into a grep.
        bool ClaimFill( ShaderResources::FillKind requested, const char* operation )
        {
            const auto claim = ShaderResources::ClaimFill( m_Fill, requested );
            // Assigned on both paths on purpose: ClaimFill answers with the state the buffer must be
            // left in, and a refusal answers with the state unchanged. Assigning only on success would
            // make that half of its contract unreachable from here, and an unreachable guarantee is one
            // nothing keeps true.
            m_Fill = claim.Next;
            if ( !claim.Accepted )
            {
                LOG_ERROR( "[UB] '{}' is filled {}; refusing {} ({}). One buffer cannot be filled both "
                           "ways -- the field shadow copies of a whole-filled buffer are uninitialised "
                           "memory, and flushing them erases whatever the renderer wrote.",
                           m_Buffer->GetName(), ShaderResources::FillKindName( m_Fill ), operation,
                           ShaderResources::FillKindName( requested ) );
                return false;
            }
            return true;
        }

    private:
        std::vector<FieldProperty>                      m_FieldProperties;
        std::unordered_map<std::string, size_t>         m_FieldIndexMap;
        std::shared_ptr<ShaderResources::UniformBuffer> m_Buffer;
        // Claimed by the first write; see ShaderResources::BufferFillKind.hpp.
        ShaderResources::FillKind m_Fill = ShaderResources::FillKind::Unclaimed;
    };
} // namespace Desert::Graphic