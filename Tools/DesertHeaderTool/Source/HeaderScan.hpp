#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace Desert::HeaderTool
{
    std::string StripComments( const std::string& source );

    struct ScannedFile
    {
        std::filesystem::path Path;
        std::string           Text;
        bool                  Checked = false;
        std::string           IncludePath;
    };

    struct Diagnostic
    {
        std::filesystem::path Path;
        int                   Line = 0;
        std::string           Message;
    };

    std::string FormatDiagnostic( const Diagnostic& diagnostic );

    struct RoutedEventName
    {
        std::string EventClass;
        std::string HandlerSuffix;
    };

    struct SubsystemDeclaration
    {
        std::string           QualifiedName;
        std::string           Owner;
        std::filesystem::path Header;
        std::string           Include;
    };

    struct HeaderModel
    {
        std::vector<RoutedEventName>      Events;
        std::vector<SubsystemDeclaration> Subsystems;
        std::vector<Diagnostic>           Errors;
    };

    HeaderModel ScanHeaders( const std::vector<ScannedFile>& files );
} // namespace Desert::HeaderTool
