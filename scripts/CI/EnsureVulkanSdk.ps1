# EnsureVulkanSdk.ps1 — make C:\VulkanSDK\<Version> a complete install, then export it to the job.
#
#   pwsh -File scripts/CI/EnsureVulkanSdk.ps1 -Version 1.3.296.0
#
# ONE PATH FOR EVERY WINDOWS JOB THAT NEEDS THE SDK (the build job and the test shards). Each job restores
# the C:\VulkanSDK cache first; this script then judges the TREE, not the cache result: "Cache restored"
# says an archive was unpacked, not that the archive holds what the job needs. Whatever part is missing
# is installed here, by the same commands on every job.
#
# A COMPLETE TREE HAS THREE PARTS:
#   Include\vulkan\vulkan.h     — the headers (the core install)
#   Lib\shadercd.lib            — the Debug halves of the shader libs: com.lunarg.vulkan.debug, an OPTIONAL
#                                 component a bare `install --default-answer` leaves out
#   Runtime\x64\vulkan-1.dll    — the Vulkan LOADER. Every executable that links the engine imports
#                                 vulkan-1.dll (vulkan-1.lib is an import library), and Windows refuses to
#                                 START such a process when the DLL is not found — exit -1073741515,
#                                 STATUS_DLL_NOT_FOUND, before a single test runs, even for a suite that never
#                                 touches Vulkan. The SDK installer puts the loader into System32 as a side
#                                 effect, which the C:\VulkanSDK cache does not carry, so a job that restored
#                                 the cache had no loader. It is taken from LunarG's runtime-components archive
#                                 into the tree, so the cache carries it, and the tree's copy goes on PATH.
#                                 A GPU-less runner has the loader and no driver: a suite that needs a device
#                                 is planned out by TestShards.py (build/TestNeedsVulkanDevice.txt).
# The cache key in ci.yml names this set; change the set -> change the key's suffix.
param([Parameter(Mandatory = $true)][string]$Version)

$ErrorActionPreference = "Stop"
$sdk = "C:\VulkanSDK\$Version"
$loaderDir = "$sdk\Runtime\x64"

function Test-SdkCore { (Test-Path "$sdk\Include\vulkan\vulkan.h") -and (Test-Path "$sdk\Lib\shadercd.lib") }

if (-not (Test-SdkCore)) {
    Write-Host "Vulkan SDK $Version is absent or incomplete at $sdk — installing"
    $url = "https://sdk.lunarg.com/sdk/download/$Version/windows/VulkanSDK-$Version-Installer.exe"
    Write-Host "Downloading $url"
    Invoke-WebRequest -Uri $url -OutFile "$env:RUNNER_TEMP\vulkan.exe" -MaximumRetryCount 3 -RetryIntervalSec 15
    # The installer downloads its components from sdk.lunarg.com itself, and that download drops
    # ("archiveDownloadError ... Connection") often enough to have failed three gate runs on 2026-09-27.
    # A cache is visible only to its own branch and the default branch, so every new integration branch
    # reinstalls. Retry the whole install from an empty root each time: a half-written root from a failed
    # attempt and an incomplete tree from a restore are cleared alike.
    $ok = $false
    for ($attempt = 1; $attempt -le 3 -and -not $ok; $attempt++) {
        if (Test-Path $sdk) { Remove-Item -Recurse -Force $sdk }
        & "$env:RUNNER_TEMP\vulkan.exe" --root $sdk --accept-licenses `
            --default-answer --confirm-command install com.lunarg.vulkan.debug
        if ($LASTEXITCODE -eq 0) { $ok = $true; break }
        Write-Host "Vulkan SDK install attempt $attempt failed ($LASTEXITCODE)"
        Start-Sleep -Seconds 30
    }
    if (-not $ok) { throw "Vulkan SDK install failed after 3 attempts" }
    if (-not (Test-SdkCore)) { throw "Vulkan SDK $Version installed, but $sdk still lacks vulkan.h or shadercd.lib" }
}

if (-not (Test-Path "$loaderDir\vulkan-1.dll")) {
    $url = "https://sdk.lunarg.com/sdk/download/$Version/windows/VulkanRT-$Version-Components.zip"
    Write-Host "Vulkan loader absent at $loaderDir — downloading $url"
    $zip = "$env:RUNNER_TEMP\vulkan-rt.zip"
    Invoke-WebRequest -Uri $url -OutFile $zip -MaximumRetryCount 3 -RetryIntervalSec 15
    $unpacked = "$env:RUNNER_TEMP\vulkan-rt"
    if (Test-Path $unpacked) { Remove-Item -Recurse -Force $unpacked }
    Expand-Archive -Path $zip -DestinationPath $unpacked
    $dll = Get-ChildItem $unpacked -Recurse -File -Filter vulkan-1.dll | Where-Object { $_.Directory.Name -eq "x64" }
    if (@($dll).Count -ne 1) { throw "VulkanRT-$Version-Components.zip: expected one x64\vulkan-1.dll, found $(@($dll).Count)" }
    New-Item -ItemType Directory -Force -Path $loaderDir | Out-Null
    Copy-Item $dll.FullName "$loaderDir\vulkan-1.dll"
}

"VULKAN_SDK=$sdk" | Out-File -FilePath $env:GITHUB_ENV -Append -Encoding utf8
$loaderDir | Out-File -FilePath $env:GITHUB_PATH -Append -Encoding utf8
Write-Host "VULKAN_SDK=$sdk; loader $loaderDir\vulkan-1.dll on PATH"
