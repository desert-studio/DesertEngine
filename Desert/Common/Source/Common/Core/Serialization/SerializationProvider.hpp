#pragma once

#include <any>
#include <string>

namespace Common::Serialization
{
    class SerializationProvider
    {
    public:
        SerializationProvider()          = default;
        virtual ~SerializationProvider() = default;

        SerializationProvider( const std::string& nodeName, const std::string& paramName, const std::any& value )
        {
        }

        std::pair<std::string, std::any> GetParametrNameAndValue() const
        {
            return std::make_pair( m_ParamName, m_Value );
        }

        std::string GetNodeName() const
        {
            return m_NodeName;
        }

    private:
        std::string m_NodeName;
        std::string m_ParamName;
        std::any    m_Value;
    };

} // namespace Common::Serialization

//clang-format off
#define SERIALIZABLE_CLASS_MAKE                                                                                   \
     : public Common::Serialization::SerializationProvider
#define SERIALIZABLE_CLASS_INIT( NodeName, ParamaName, Value )                                                    \
    , Common::Serialization::SerializationProvider( #NodeName, #ParamaName, Value )

//clang-format on
