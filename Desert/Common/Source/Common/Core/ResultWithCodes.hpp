#pragma once

#include <optional>
#include <variant>
#include <string>
#include <vector>
#include <initializer_list>

#include <spdlog/fmt/fmt.h>

// ReportFailedUnwrap lives with the other result type: both unwrap the same way and must report the
// same way, or a reader would have to know which of the two they are holding to know whether a
// failed unwrap says anything.
#include <Common/Core/ResultStr.hpp>

#include <algorithm>
#include <utility>

namespace Common
{
    template <typename T, typename ErrorCodeType = int>
    class ResultWithCodes;

    template <typename T, typename ErrorCodeType = int>
    ResultWithCodes<T, ErrorCodeType> MakeErrorWithCodes( std::initializer_list<ErrorCodeType> errorCodes,
                                                          const std::string&                   message );

    template <typename T, typename ErrorCodeType = int, typename... Args>
    ResultWithCodes<T, ErrorCodeType> MakeFormattedErrorWithCodes( std::initializer_list<ErrorCodeType> errorCodes,
                                                                   fmt::format_string<Args...>          format,
                                                                   Args&&... args );
    template <typename T = bool, typename ErrorCodeType = int>
    auto MakeSuccessWithCodes( const T& value );

    // Alias for common use case
    template <typename ErrorCodeType = int>
    using BoolResultWithCodes = ResultWithCodes<bool, ErrorCodeType>;

    template <typename T, typename ErrorCodeType>
    class ResultWithCodes
    {
    public:
        ResultWithCodes() = default;

    public:
        class Error
        {
        public:
            explicit Error( std::initializer_list<ErrorCodeType> errorCodes, const std::string& errorMessage )
                 : m_ErrorMessage( errorMessage ), m_ErrorCodes( errorCodes )
            {
            }

            explicit Error( const std::string& errorMessage ) : m_ErrorMessage( errorMessage )
            {
            }

            // The format string is checked by the COMPILER (see Common/Core/Logger.hpp for the full why).
            // This constructor builds the message of a FAILURE, so a mismatched brace count here threw
            // `fmt::format_error` at the one moment the code was already handling something going wrong -
            // and nothing catches it. A genuinely runtime format must say so with `fmt::runtime(...)`.
            //
            // The `const std::string&` overload above still wins for a single non-literal argument, so a
            // message that merely CONTAINS braces and formats nothing is unaffected, exactly as before.
            template <typename... Args>
            explicit Error( std::initializer_list<ErrorCodeType> errorCodes, fmt::format_string<Args...> format,
                            Args&&... args )
                 : m_ErrorMessage( fmt::format( format, std::forward<Args>( args )... ) ),
                   m_ErrorCodes( errorCodes )
            {
            }

            // The format string is checked by the COMPILER (see Common/Core/Logger.hpp for the full why).
            // This constructor builds the message of a FAILURE, so a mismatched brace count here threw
            // `fmt::format_error` at the one moment the code was already handling something going wrong -
            // and nothing catches it. A genuinely runtime format must say so with `fmt::runtime(...)`.
            //
            // The `const std::string&` overload above still wins for a single non-literal argument, so a
            // message that merely CONTAINS braces and formats nothing is unaffected, exactly as before.
            template <typename... Args>
            explicit Error( fmt::format_string<Args...> format, Args&&... args )
                 : m_ErrorMessage( fmt::format( format, std::forward<Args>( args )... ) )
            {
            }

            const std::string& GetMessage() const
            {
                return m_ErrorMessage;
            }

            const std::vector<ErrorCodeType>& GetErrorCodes() const
            {
                return m_ErrorCodes;
            }

            bool HasErrorCode( ErrorCodeType code ) const
            {
                return std::find( m_ErrorCodes.begin(), m_ErrorCodes.end(), code ) != m_ErrorCodes.end();
            }

            bool HasAnyErrorCode( std::initializer_list<ErrorCodeType> codes ) const
            {
                for ( const auto& code : codes )
                {
                    if ( HasErrorCode( code ) )
                    {
                        return true;
                    }
                }
                return false;
            }

            bool HasAllErrorCodes( std::initializer_list<ErrorCodeType> codes ) const
            {
                for ( const auto& code : codes )
                {
                    if ( !HasErrorCode( code ) )
                    {
                        return false;
                    }
                }
                return true;
            }

        private:
            std::string                m_ErrorMessage;
            std::vector<ErrorCodeType> m_ErrorCodes;
        };

        bool IsSuccess() const
        {
            return m_IsSuccess;
        }

        // Same contract as ResultStr::GetValue — see the long WHY there. Two differences are specific
        // to this type and both were defects rather than choices:
        //
        // THE NON-CONST OVERLOAD IS GONE. It returned `static T default_value{}` by MUTABLE reference
        // on the failure path: one process-wide object per T, handed to every failed unwrap anywhere
        // in the binary, writable by any of them and read by all the others, with no synchronisation.
        // A caller that wrote through it (which the signature invited) silently changed what every
        // other failed unwrap of that T would see afterwards, across threads. Nothing in the engine
        // needed to mutate a result's value in place, so the overload is removed rather than fixed —
        // a const answer cannot grow this defect back.
        //
        // THE RVALUE OVERLOAD IS DELETED, so unwrapping a temporary does not compile.
        const T& GetValue() const&
        {
            if ( !m_IsSuccess )
            {
                ReportFailedUnwrap( GetError() );
                static const T default_value{};
                return default_value;
            }
            return std::get<T>( m_Outcome );
        }

        const T& GetValue() const&& = delete;

        std::string GetError() const
        {
            if ( m_IsSuccess )
            {
                return s_NoError;
            }
            return std::get<Error>( m_Outcome ).GetMessage();
        }

        const std::vector<ErrorCodeType>& GetErrorCodes() const
        {
            if ( m_IsSuccess )
            {
                static std::vector<ErrorCodeType> empty_codes;
                return empty_codes;
            }
            return std::get<Error>( m_Outcome ).GetErrorCodes();
        }

        bool HasErrorCode( ErrorCodeType code ) const
        {
            if ( m_IsSuccess )
            {
                return false;
            }
            return std::get<Error>( m_Outcome ).HasErrorCode( code );
        }

        bool HasAnyErrorCode( std::initializer_list<ErrorCodeType> codes ) const
        {
            if ( m_IsSuccess )
            {
                return false;
            }
            return std::get<Error>( m_Outcome ).HasAnyErrorCode( codes );
        }

        bool HasAllErrorCodes( std::initializer_list<ErrorCodeType> codes ) const
        {
            if ( m_IsSuccess )
            {
                return false;
            }
            return std::get<Error>( m_Outcome ).HasAllErrorCodes( codes );
        }

        explicit operator bool() const
        {
            return m_IsSuccess;
        }

        // For compatibility with existing code
        const ErrorCodeType GetPrimaryErrorCode() const
        {
            if ( m_IsSuccess || GetErrorCodes().empty() )
            {
                return ErrorCodeType{};
            }
            return GetErrorCodes()[0];
        }

    private:
        explicit ResultWithCodes( const Error& error ) : m_Outcome( error ), m_IsSuccess( false )
        {
        }

        explicit ResultWithCodes( const T& value ) : m_Outcome( value ), m_IsSuccess( true )
        {
        }

        static inline std::string s_NoError = "Cannot get error message, result is a success";
        std::variant<T, Error>    m_Outcome;
        bool                      m_IsSuccess = false;

    private:
        // Befriend the whole factory templates (not specific specializations):
        // MSVC accepted the `Make...<U, E>` spelling, but standard C++ (clang/gcc)
        // rejects it as a function template partial specialization.
        template <typename U, typename E>
        friend ResultWithCodes<U, E> MakeErrorWithCodes( std::initializer_list<E> errorCodes,
                                                         const std::string&       message );

        template <typename U, typename E>
        friend auto MakeSuccessWithCodes( const U& value );

        template <typename U, typename E, typename... Args>
        friend ResultWithCodes<U, E> MakeFormattedErrorWithCodes( std::initializer_list<E>    errorCodes,
                                                                  fmt::format_string<Args...> format,
                                                                  Args&&... args );
    };

    template <typename T, typename ErrorCodeType>
    ResultWithCodes<T, ErrorCodeType> MakeErrorWithCodes( std::initializer_list<ErrorCodeType> errorCodes,
                                                          const std::string&                   message )
    {
        return ResultWithCodes<T, ErrorCodeType>(
             typename ResultWithCodes<T, ErrorCodeType>::Error( errorCodes, message ) );
    }

    // Like MakeFormattedError in ResultStr.hpp: the format has to stay a format string across this
    // boundary, or the check it carries is thrown away here on behalf of every caller.
    template <typename T, typename ErrorCodeType, typename... Args>
    ResultWithCodes<T, ErrorCodeType> MakeFormattedErrorWithCodes( std::initializer_list<ErrorCodeType> errorCodes,
                                                                   fmt::format_string<Args...>          format,
                                                                   Args&&... args )
    {
        return ResultWithCodes<T, ErrorCodeType>( typename ResultWithCodes<T, ErrorCodeType>::Error(
             errorCodes, format, std::forward<Args>( args )... ) );
    }

    template <typename T, typename ErrorCodeType>
    auto MakeSuccessWithCodes( const T& value )
    {
        return ResultWithCodes<T, ErrorCodeType>( value );
    }

    template <typename T, typename ErrorCodeType = int>
    ResultWithCodes<T, ErrorCodeType> MakeErrorWithSingleCode( ErrorCodeType      errorCode,
                                                               const std::string& message )
    {
        return MakeErrorWithCodes<T, ErrorCodeType>( { errorCode }, message );
    }

    template <typename T, typename ErrorCodeType = int, typename... Args>
    ResultWithCodes<T, ErrorCodeType>
    MakeFormattedErrorWithSingleCode( ErrorCodeType errorCode, fmt::format_string<Args...> format, Args&&... args )
    {
        return MakeFormattedErrorWithCodes<T, ErrorCodeType>( { errorCode }, format,
                                                              std::forward<Args>( args )... );
    }

} // namespace Common