#pragma once

#include <string_view>
#include <atomic>
#include <cstdint>
#include <cstring>

namespace Desert::Graphic
{
    enum class PropertyKind
    {
        Value,      // POD scalar/vector — can be written to a FieldProperty via SetRawBytes
        Texture2D,  // void* image pointer — written to a Texture2DProperty
    };

    // A `PropertyTypeTag` enum and a `PropertyEditorMeta` struct STOOD HERE, together with the three
    // pure virtuals that served them (GetTypeTag, GetEditorMeta, SetEditorMeta) and a `m_Meta` member on
    // both property templates. Г12 deleted all of it, and the reason is worth more than the deletion:
    //
    // this was not an unbuilt capability. It was a SECOND DESIGN for editor hints the engine already
    // has and uses. `PropertyEditorMeta` carried displayName / category / minVal / maxVal — and the
    // live mechanism is the reflection macro, `PROPERTY( DisplayName( ... ), Category( ... ),
    // Range( lo, hi ) )`, authored beside the field it describes and read across the editor. Colour is
    // decided by the parameter's TYPE at the draw site, not by an `isColor` flag. Category grouping in
    // the material editor comes from the shader's own Properties block, which is where the parameter is
    // declared.
    //
    // So keeping it was not "holding a feature in reserve", it was maintaining a competing source of
    // truth for a value that already has one (contract §2, one source of truth per value) — and a dead
    // one, which is the worst kind: it reads as the intended mechanism to anyone who finds it first.
    // Nothing is filed as a follow-up, because there is nothing to build: the capability exists.

    class IProperty
    {
    public:
        virtual ~IProperty() = default;

        virtual std::string_view GetName()       const = 0;
        virtual std::string_view GetShaderName() const = 0;
        virtual bool             IsDirty()       const = 0;
        virtual void             MarkClean()           = 0;
        virtual void             Reset()               = 0;

        virtual PropertyKind GetKind() const                = 0;
        virtual size_t       GetByteSize() const            = 0;
        virtual void         CopyValueTo( void* out ) const = 0;

        // CPU-BIND-PREPARE (MaterialExecutor::CheckRecordable in O(1)): the owner's counter, bumped by every
        // write that leaves the property dirty. False when this kind counts no writes, so the owner checks every
        // property instead (Material::HasPendingRegisteredProperties).
        [[nodiscard]] virtual bool AttachWriteSink( std::atomic<uint64_t>* /*sink*/ ) noexcept
        {
            return false;
        }
    };

    // Base interface for objects that own typed properties (i.e. Material and its subclasses).
    // Kept separate from IProperty so TProperty.hpp has no dependency on Material headers.
    class IPropertyOwner
    {
    public:
        virtual void RegisterProperty( IProperty* prop ) = 0;

    protected:
        ~IPropertyOwner() = default;
    };

    // Strongly-typed material property. Set() is no-op when value hasn't changed.
    template <typename T>
    class TProperty final : public IProperty
    {
    public:
        TProperty( std::string_view name, std::string_view shaderName, T defaultValue )
             : m_Name( name )
             , m_ShaderName( shaderName )
             , m_Value( defaultValue )
             , m_Default( defaultValue )
        {
        }

        std::string_view GetName()       const override { return m_Name; }
        std::string_view GetShaderName() const override { return m_ShaderName; }
        bool             IsDirty()       const override { return m_Dirty; }
        void             MarkClean()           override { m_Dirty = false; }
        bool             AttachWriteSink( std::atomic<uint64_t>* sink ) noexcept override
        {
            m_WriteSink = sink;
            return true;
        }
        PropertyKind     GetKind()       const override { return PropertyKind::Value; }
        size_t           GetByteSize()   const override { return sizeof( T ); }

        void CopyValueTo( void* out ) const override
        {
            memcpy( out, &m_Value, sizeof( T ) );
        }

        void Reset() override
        {
            if ( m_Value != m_Default )
            {
                m_Value = m_Default;
                NoteDirty();
            }
        }

        const T& Get()        const { return m_Value; }
        const T& GetDefault() const { return m_Default; }

        void Set( const T& value )
        {
            if ( m_Value != value )
            {
                m_Value = value;
                NoteDirty();
            }
        }

        void ForceSet( const T& value )
        {
            m_Value = value;
            NoteDirty();
        }

    private:
        void NoteDirty() noexcept
        {
            m_Dirty = true;
            if ( m_WriteSink != nullptr )
                m_WriteSink->fetch_add( 1, std::memory_order_relaxed );
        }

        std::string_view    m_Name;
        std::string_view    m_ShaderName;
        T                   m_Value;
        T                   m_Default;
        // Start dirty so the default value is uploaded to the GPU on the first frame even when
        // Set() is called with the same value (equality check would skip it).
        bool                m_Dirty = true;
        // The owning Material's write counter (AttachWriteSink); it outlives its member properties.
        std::atomic<uint64_t>* m_WriteSink = nullptr;
    };

    // `TTextureProperty` STOOD HERE AND NOTHING IN THE TREE EVER MADE ONE. Its only producer was the
    // MTEXTURE_PROPERTY macro below, and that macro had zero uses over the whole repository — so the
    // class existed to hold a `void* m_Value` that no material ever set and no pass ever read. The
    // pointer-ownership census (Desert/Tests/Engine/PointerOwnership) found it while asking who owned
    // the image behind that `void*`, and the answer was "nobody, because there is no image": an
    // un-owned raw pointer whose lifetime question could not be asked, let alone answered. A8.
    //
    // The LIVE texture path is unaffected and is not this: a texture reaches a material through
    // `MaterialInstance::SetTexture` -> the `void*` alternative of MaterialPropertyValue ->
    // `Texture2DProperty::SetImage`, which snapshots the descriptor. That path has its own row in the
    // census.

} // namespace Desert::Graphic

// ---------------------------------------------------------------------------
// Declares a typed property member, auto-registers it with the owning Material,
// and generates Get##VarName / Set##VarName accessors.
//
// Usage (inside a Material subclass body):
//   MPROPERTY( float, Metallic, "u_Metallic", 0.0f )
// ---------------------------------------------------------------------------
// clang-format off
#define MPROPERTY( Type, VarName, ShaderName, Default )                                    \
    Desert::Graphic::TProperty<Type> VarName { #VarName, ShaderName, Default };            \
    struct VarName##_PropertyRegistrar                                                      \
    {                                                                                       \
        VarName##_PropertyRegistrar( Desert::Graphic::IPropertyOwner* owner,               \
                                     Desert::Graphic::IProperty*       prop )              \
        {                                                                                   \
            owner->RegisterProperty( prop );                                               \
        }                                                                                   \
    } VarName##_reg { this, &VarName };                                                    \
    Type Get##VarName() const { return VarName.Get(); }                                    \
    void Set##VarName( const Type& val ) { VarName.Set( val ); }

// clang-format on
