param(
    [string]$RootPath = (Join-Path $PSScriptRoot ".."),
    [string]$GeoBuildDir = "",
    [string]$LibGeoBuildDir = "",
    [string]$OutputDir = "",
    [switch]$Dynamic
)

$ErrorActionPreference = "Stop"

function Resolve-NormalPath([string]$PathValue) {
    return (Resolve-Path $PathValue).Path
}

function Resolve-ExistingPath([string[]]$Candidates) {
    foreach ($candidate in $Candidates) {
        if ([string]::IsNullOrWhiteSpace($candidate)) {
            continue
        }
        if (Test-Path $candidate) {
            return (Resolve-Path $candidate).Path
        }
    }
    return $Candidates[0]
}

function Ensure-Directory([string]$PathValue) {
    if (!(Test-Path $PathValue)) {
        New-Item -ItemType Directory -Path $PathValue | Out-Null
    }
}

function Copy-IfExists([string]$SourcePath, [string]$DestinationPath) {
    if (Test-Path $SourcePath) {
        Copy-Item -Path $SourcePath -Destination $DestinationPath -Force
    }
}

function New-BinaryAlias([string]$TargetPath, [string]$AliasPath) {
    if (!(Test-Path $TargetPath)) {
        return
    }
    if (Test-Path $AliasPath) {
        Remove-Item -Path $AliasPath -Force
    }
    try {
        New-Item -ItemType HardLink -Path $AliasPath -Target (Resolve-Path $TargetPath).Path | Out-Null
    } catch {
        Write-Warning "Hard-link alias failed for $AliasPath; falling back to a byte copy."
        Copy-Item -Path $TargetPath -Destination $AliasPath -Force
    }
}

function Copy-TreeIfExists([string]$SourcePath, [string]$DestinationPath) {
    if (Test-Path $SourcePath) {
        Copy-Item -Path $SourcePath -Destination $DestinationPath -Recurse -Force
    }
}

function Copy-FirstExistingFile([string[]]$Candidates, [string]$DestinationDir, [string]$Label) {
    foreach ($candidate in $Candidates) {
        if ([string]::IsNullOrWhiteSpace($candidate)) {
            continue
        }
        if (Test-Path $candidate) {
            $resolved = (Resolve-Path $candidate).Path
            Copy-Item -Path $resolved -Destination $DestinationDir -Force
            Write-Host "[portable-windows] bundled ${Label}: $resolved"
            return $true
        }
    }
    Write-Warning "[portable-windows] optional $Label was not found; backend will require vendor runtime from installed GPU driver/runtime."
    return $false
}

function Copy-ExistingFilesByPattern([string[]]$Directories, [string]$Filter, [string]$DestinationDir, [string]$Label) {
    $copied = 0
    $seen = @{}
    foreach ($dir in $Directories) {
        if ([string]::IsNullOrWhiteSpace($dir) -or !(Test-Path $dir)) {
            continue
        }
        Get-ChildItem -Path $dir -Filter $Filter -File -ErrorAction SilentlyContinue | ForEach-Object {
            $key = $_.Name.ToLowerInvariant()
            if (!$seen.ContainsKey($key)) {
                Copy-Item -Path $_.FullName -Destination $DestinationDir -Force
                $seen[$key] = $true
                $copied++
                Write-Host "[portable-windows] bundled ${Label}: $($_.FullName)"
            }
        }
    }
    if ($copied -eq 0) {
        Write-Warning "[portable-windows] $Label files were not found in the inspected runtime directories."
    }
    return $copied
}

function Copy-FirstDirectoryFilesByPattern([string[]]$Directories, [string]$Filter, [string]$DestinationDir, [string]$Label) {
    foreach ($dir in $Directories) {
        if ([string]::IsNullOrWhiteSpace($dir) -or !(Test-Path $dir)) {
            continue
        }
        $matches = @(Get-ChildItem -Path $dir -Filter $Filter -File -ErrorAction SilentlyContinue)
        if ($matches.Count -eq 0) {
            continue
        }
        foreach ($match in $matches) {
            Copy-Item -Path $match.FullName -Destination $DestinationDir -Force
            Write-Host "[portable-windows] bundled ${Label}: $($match.FullName)"
        }
        return $true
    }
    Write-Warning "[portable-windows] optional $Label files were not found; backend will require vendor runtime from installed GPU driver/runtime."
    return $false
}

function Add-IfDirectory([System.Collections.Generic.List[string]]$List, [string]$PathValue) {
    if (![string]::IsNullOrWhiteSpace($PathValue) -and (Test-Path $PathValue)) {
        $resolved = (Resolve-Path $PathValue).Path
        if (!$List.Contains($resolved)) {
            $List.Add($resolved) | Out-Null
        }
    }
}

function Get-CMakeCacheValue([string]$CachePath, [string]$Key) {
    if (!(Test-Path $CachePath)) {
        return ""
    }
    $line = Select-String -Path $CachePath -Pattern "^$([Regex]::Escape($Key)):[^=]*=(.*)$" | Select-Object -First 1
    if ($line) {
        return $line.Matches[0].Groups[1].Value
    }
    return ""
}

function Find-CMakeCacheDirectory([string]$StartPath) {
    if ([string]::IsNullOrWhiteSpace($StartPath) -or !(Test-Path $StartPath)) {
        return ""
    }

    $current = Get-Item $StartPath
    if (!$current.PSIsContainer) {
        $current = $current.Directory
    }
    for ($depth = 0; $depth -lt 4 -and $null -ne $current; $depth++) {
        if (Test-Path (Join-Path $current.FullName "CMakeCache.txt")) {
            return $current.FullName
        }
        $current = $current.Parent
    }
    return ""
}

function Add-OpenSslRuntimeDirectoryFromLibrary(
    [System.Collections.Generic.List[string]]$List,
    [string]$LibraryPath
) {
    if ([string]::IsNullOrWhiteSpace($LibraryPath) -or
        $LibraryPath -in @("optimized", "debug", "general")) {
        return
    }
    $libraryDir = Split-Path $LibraryPath -Parent
    if ([string]::IsNullOrWhiteSpace($libraryDir)) {
        return
    }
    Add-IfDirectory $List (Join-Path (Split-Path $libraryDir -Parent) "bin")
}

function Get-HipRuntimeDirectories([string]$BuildDir, [string]$ProjectRoot) {
    $dirs = [System.Collections.Generic.List[string]]::new()

    $cacheHipLib = Get-CMakeCacheValue (Join-Path $BuildDir "CMakeCache.txt") "GEOCOIN_HIP_LIBRARY"
    if ($cacheHipLib) {
        Add-IfDirectory $dirs (Join-Path (Split-Path (Split-Path $cacheHipLib -Parent) -Parent) "bin")
    }

    $toolchainRoot = Join-Path $ProjectRoot "out\toolchains"
    if (Test-Path $toolchainRoot) {
        Get-ChildItem $toolchainRoot -Directory -Filter "rocm-*-win-extracted" -ErrorAction SilentlyContinue |
            Sort-Object Name -Descending |
            ForEach-Object {
                Get-ChildItem $_.FullName -Directory -Recurse -ErrorAction SilentlyContinue |
                    Where-Object { $_.FullName -match '\\AMD\\ROCm\\[0-9][^\\]*\\bin$' } |
                    Sort-Object FullName -Descending |
                    ForEach-Object { Add-IfDirectory $dirs $_.FullName }
            }
    }

    Add-IfDirectory $dirs $(if ($env:HIP_PATH) { Join-Path $env:HIP_PATH "bin" } else { "" })
    Add-IfDirectory $dirs $(if ($env:ROCM_PATH) { Join-Path $env:ROCM_PATH "bin" } else { "" })

    $programRocm = "C:\Program Files\AMD\ROCm"
    if (Test-Path $programRocm) {
        Get-ChildItem $programRocm -Directory -ErrorAction SilentlyContinue |
            Sort-Object Name -Descending |
            ForEach-Object { Add-IfDirectory $dirs (Join-Path $_.FullName "bin") }
        Add-IfDirectory $dirs (Join-Path $programRocm "bin")
    }

    return $dirs.ToArray()
}

function Invoke-NativeChecked([string]$FilePath, [string[]]$Arguments) {
    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$FilePath failed with exit code $LASTEXITCODE"
    }
}

function Resolve-BinaryDirectory([string]$BaseDir, [string]$FileName) {
    $candidates = @(
        $BaseDir,
        (Join-Path $BaseDir "Release"),
        (Join-Path $BaseDir "bin"),
        (Join-Path $BaseDir "Release\bin")
    )

    foreach ($candidate in $candidates) {
        if (Test-Path (Join-Path $candidate $FileName)) {
            return (Resolve-Path $candidate).Path
        }
    }

    $match = Get-ChildItem -Path $BaseDir -Filter $FileName -Recurse -File -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($match) {
        return $match.Directory.FullName
    }

    return $BaseDir
}

$root = Resolve-NormalPath $RootPath
$staticRelease = -not $Dynamic

if ($staticRelease) {
    if ([string]::IsNullOrWhiteSpace($env:VCPKG_ROOT)) {
        foreach ($candidate in @("H:\PROJECTS\VCPKG", "C:\vcpkg")) {
            if (Test-Path (Join-Path $candidate "scripts\buildsystems\vcpkg.cmake")) {
                $env:VCPKG_ROOT = (Resolve-Path $candidate).Path
                break
            }
        }
    }
    if (-not [string]::IsNullOrWhiteSpace($env:VCPKG_ROOT) -and $env:VCPKG_ROOT -match '\+') {
        throw "Static Qt builds require VCPKG_ROOT without '+'. Use a path such as H:\PROJECTS\VCPKG or C:\vcpkg."
    }
}

if ([string]::IsNullOrWhiteSpace($GeoBuildDir)) {
    $GeoBuildDir = if ($staticRelease) {
        Resolve-ExistingPath @(
            (Join-Path $root "out\build\windows-clang-static-release-gui-gpu"),
            (Join-Path $root "out\build\windows-clang-static-release-gui")
        )
    } else {
        Resolve-ExistingPath @(
            (Join-Path $root "out\build\windows-clang-release-gui"),
            (Join-Path $root "out\build\windows-release-gui")
        )
    }
}

if ([string]::IsNullOrWhiteSpace($LibGeoBuildDir)) {
    $LibGeoBuildDir = if ($staticRelease) {
        Resolve-ExistingPath @(
            (Join-Path $root "libgeo\out\build\windows-clang-static-release")
        )
    } else {
        Resolve-ExistingPath @(
            (Join-Path $root "libgeo\out\build\windows-clang-release"),
            (Join-Path $root "libgeo\out\build\windows-release")
        )
    }
}

if ([string]::IsNullOrWhiteSpace($OutputDir)) {
    $OutputDir = if ($staticRelease) {
        Join-Path $root "out\portable\GeoCoin-windows-x64-static"
    } else {
        Join-Path $root "out\portable\GeoCoin-windows-x64"
    }
}

$PortableRoot = Split-Path $OutputDir -Parent
$ArchivePath = if ($staticRelease) {
    Join-Path $PortableRoot "GeoCoin-windows-x64-static.zip"
} else {
    Join-Path $PortableRoot "GeoCoin-windows-x64-portable.zip"
}

if (Test-Path $GeoBuildDir) {
    $GeoBuildDir = Resolve-BinaryDirectory $GeoBuildDir "geocoin-core.exe"
}
if (Test-Path $LibGeoBuildDir) {
    $LibGeoBuildDir = Resolve-BinaryDirectory $LibGeoBuildDir "geo.lib"
}

if (!(Test-Path (Join-Path $GeoBuildDir "geocoin-core.exe"))) {
    $preset = if ($staticRelease) { "windows-clang-static-release-gui-gpu" } else { "windows-clang-release-gui" }
    $buildDir = if ($staticRelease) {
        Join-Path $root "out\build\windows-clang-static-release-gui-gpu"
    } else {
        Join-Path $root "out\build\windows-clang-release-gui"
    }
    Write-Host "[portable-windows] GUI build not found, building preset $preset..."
    Invoke-NativeChecked "cmake" @("--preset", $preset, "-S", $root)
    Invoke-NativeChecked "cmake" @("--build", $buildDir, "--config", "Release", "--parallel")
    $GeoBuildDir = Resolve-BinaryDirectory $buildDir "geocoin-core.exe"
}

if (!(Test-Path (Join-Path $LibGeoBuildDir "geo.lib"))) {
    $preset = if ($staticRelease) { "windows-clang-static-release" } else { "windows-clang-release" }
    $buildDir = if ($staticRelease) {
        Join-Path $root "libgeo\out\build\windows-clang-static-release"
    } else {
        Join-Path $root "libgeo\out\build\windows-clang-release"
    }
    Write-Host "[portable-windows] libgeo build not found, building preset $preset..."
    Invoke-NativeChecked "cmake" @("--preset", $preset, "-S", (Join-Path $root "libgeo"))
    Invoke-NativeChecked "cmake" @("--build", $buildDir, "--config", "Release", "--parallel")
    $LibGeoBuildDir = Resolve-BinaryDirectory $buildDir "geo.lib"
}

if (!(Test-Path (Join-Path $GeoBuildDir "geocoin-core.exe"))) {
    throw "geocoin-core.exe was not found under $GeoBuildDir"
}
if (!(Test-Path (Join-Path $LibGeoBuildDir "geo.lib"))) {
    throw "geo.lib was not found under $LibGeoBuildDir"
}

if (Test-Path $OutputDir) {
    Remove-Item -Path $OutputDir -Recurse -Force
}
Ensure-Directory $OutputDir

$binDir = Join-Path $OutputDir "bin"
$libDir = Join-Path $OutputDir "lib"
$includeDir = Join-Path $OutputDir "include\libgeo"
$configDir = Join-Path $OutputDir "config"

Ensure-Directory $binDir
Ensure-Directory $libDir
Ensure-Directory $includeDir
Ensure-Directory $configDir

$binaries = @(
    "geocoin-core.exe",
    "geocoin-node.exe",
    "genesis.exe",
    "geocoin-rpc-miner.exe",
    "geocoin-gpu-miner.exe"
)

foreach ($binary in $binaries) {
    Copy-IfExists (Join-Path $GeoBuildDir $binary) $binDir
}

New-BinaryAlias (Join-Path $binDir "geocoin-node.exe") (Join-Path $binDir "degree_node.exe")
New-BinaryAlias (Join-Path $binDir "genesis.exe") (Join-Path $binDir "mine_genesis.exe")
New-BinaryAlias (Join-Path $binDir "genesis.exe") (Join-Path $binDir "geocoin-miner.exe")

if ($staticRelease) {
    $openClCandidates = @(
        (Join-Path $GeoBuildDir "OpenCL.dll"),
        (Join-Path $root "vendor\opencl\bin\OpenCL.dll"),
        (Join-Path $root "third_party\opencl\bin\OpenCL.dll"),
        (Join-Path $root "external\opencl\bin\OpenCL.dll"),
        $(if ($env:GEOCOIN_OPENCL_LOADER_DLL) { $env:GEOCOIN_OPENCL_LOADER_DLL } else { "" }),
        $(if ($env:OpenCL_ROOT) { Join-Path $env:OpenCL_ROOT "bin\OpenCL.dll" } else { "" }),
        $(if ($env:VCPKG_ROOT) { Join-Path $env:VCPKG_ROOT "installed\x64-windows\bin\OpenCL.dll" } else { "" }),
        $(if ($env:VCPKG_ROOT) { Join-Path $env:VCPKG_ROOT "installed\x64-windows-static\bin\OpenCL.dll" } else { "" }),
        "C:\Windows\System32\OpenCL.dll"
    )
    $null = Copy-FirstExistingFile $openClCandidates $binDir "OpenCL ICD loader"

    $hipRuntimeDirs = Get-HipRuntimeDirectories $GeoBuildDir $root
    $null = Copy-FirstDirectoryFilesByPattern $hipRuntimeDirs "amdhip64_*.dll" $binDir "AMD HIP runtime"
    $null = Copy-FirstDirectoryFilesByPattern $hipRuntimeDirs "amd_comgr_*.dll" $binDir "AMD HIP compiler runtime"
}

if ($Dynamic) {
    Get-ChildItem -Path $GeoBuildDir -File -Filter *.dll | ForEach-Object {
        Copy-Item -Path $_.FullName -Destination $binDir -Force
    }
    foreach ($dirName in @("platforms", "imageformats", "iconengines", "styles")) {
        Copy-TreeIfExists (Join-Path $GeoBuildDir $dirName) $binDir
    }
    Copy-IfExists (Join-Path $LibGeoBuildDir "geo.dll") $binDir

    # CMake stages detected runtime DLLs next to the executables. Also inspect
    # the configured OpenSSL/vcpkg roots so a portable bundle cannot silently
    # omit OpenSSL 3.x (or legacy 1.1) when a toolchain does not stage it.
    $opensslRuntimeDirs = [System.Collections.Generic.List[string]]::new()
    Add-IfDirectory $opensslRuntimeDirs $GeoBuildDir

    $cmakeCacheDir = Find-CMakeCacheDirectory $GeoBuildDir
    $isDebugBuild = (Split-Path $GeoBuildDir -Leaf) -ieq "Debug"
    if (![string]::IsNullOrWhiteSpace($cmakeCacheDir)) {
        $cachePath = Join-Path $cmakeCacheDir "CMakeCache.txt"
        $cmakeBuildType = Get-CMakeCacheValue $cachePath "CMAKE_BUILD_TYPE"
        if ($cmakeBuildType -ieq "Debug") {
            $isDebugBuild = $true
        }
        $vcpkgInstalledDir = Get-CMakeCacheValue $cachePath "VCPKG_INSTALLED_DIR"
        if ([string]::IsNullOrWhiteSpace($vcpkgInstalledDir)) {
            $vcpkgInstalledDir = Get-CMakeCacheValue $cachePath "_VCPKG_INSTALLED_DIR"
        }
        $vcpkgTriplet = Get-CMakeCacheValue $cachePath "VCPKG_TARGET_TRIPLET"
        if (![string]::IsNullOrWhiteSpace($vcpkgInstalledDir) -and
            ![string]::IsNullOrWhiteSpace($vcpkgTriplet)) {
            if ($isDebugBuild) {
                Add-IfDirectory $opensslRuntimeDirs (Join-Path $vcpkgInstalledDir "$vcpkgTriplet\debug\bin")
            }
            Add-IfDirectory $opensslRuntimeDirs (Join-Path $vcpkgInstalledDir "$vcpkgTriplet\bin")
        }

        $opensslRoot = Get-CMakeCacheValue $cachePath "OPENSSL_ROOT_DIR"
        if (![string]::IsNullOrWhiteSpace($opensslRoot)) {
            if ($isDebugBuild) {
                Add-IfDirectory $opensslRuntimeDirs (Join-Path $opensslRoot "debug\bin")
            }
            Add-IfDirectory $opensslRuntimeDirs (Join-Path $opensslRoot "bin")
        }

        $libraryVariables = if ($isDebugBuild) {
            @(
                "OPENSSL_CRYPTO_LIBRARY_DEBUG",
                "OPENSSL_SSL_LIBRARY_DEBUG",
                "OPENSSL_CRYPTO_LIBRARY",
                "OPENSSL_SSL_LIBRARY"
            )
        } else {
            @(
                "OPENSSL_CRYPTO_LIBRARY_RELEASE",
                "OPENSSL_SSL_LIBRARY_RELEASE",
                "OPENSSL_CRYPTO_LIBRARY",
                "OPENSSL_SSL_LIBRARY"
            )
        }
        foreach ($libraryVariable in $libraryVariables) {
            $librarySpec = Get-CMakeCacheValue $cachePath $libraryVariable
            $libraryConfiguration = "general"
            foreach ($libraryPath in ($librarySpec -split ';')) {
                if ($libraryPath -in @("optimized", "debug", "general")) {
                    $libraryConfiguration = $libraryPath
                    continue
                }
                $matchesBuild = $libraryConfiguration -eq "general" -or
                    ($isDebugBuild -and $libraryConfiguration -eq "debug") -or
                    (!$isDebugBuild -and $libraryConfiguration -eq "optimized")
                if ($matchesBuild) {
                    Add-OpenSslRuntimeDirectoryFromLibrary $opensslRuntimeDirs $libraryPath
                }
            }
        }
    }

    if (![string]::IsNullOrWhiteSpace($env:VCPKG_ROOT)) {
        if ($isDebugBuild) {
            Add-IfDirectory $opensslRuntimeDirs (Join-Path $env:VCPKG_ROOT "installed\x64-windows\debug\bin")
        }
        Add-IfDirectory $opensslRuntimeDirs (Join-Path $env:VCPKG_ROOT "installed\x64-windows\bin")
    }

    $opensslRuntimeDirArray = $opensslRuntimeDirs.ToArray()
    $cryptoDllCount = Copy-ExistingFilesByPattern `
        $opensslRuntimeDirArray "libcrypto*.dll" $binDir "OpenSSL crypto runtime"
    $sslDllCount = Copy-ExistingFilesByPattern `
        $opensslRuntimeDirArray "libssl*.dll" $binDir "OpenSSL SSL runtime"

    if ($cryptoDllCount -eq 0 -or $sslDllCount -eq 0) {
        throw "Dynamic Windows bundle is missing required OpenSSL runtime DLLs"
    }
}

Copy-IfExists (Join-Path $LibGeoBuildDir "geo.lib") $libDir
Copy-IfExists (Join-Path $LibGeoBuildDir "geo_static.lib") $libDir
Copy-IfExists (Join-Path $root "libgeo\include\libgeo\libgeo.h") $includeDir

Get-ChildItem -Path (Join-Path $root "config") -File -Filter *.json | ForEach-Object {
    Copy-Item -Path $_.FullName -Destination $configDir -Force
}

Copy-TreeIfExists (Join-Path $root "docs") $OutputDir
Copy-TreeIfExists (Join-Path $root "deploy") $OutputDir
Copy-IfExists (Join-Path $root "README.md") $OutputDir
$geographicLicense = Join-Path $root "geographiclib\LICENSE.txt"
if (!(Test-Path $geographicLicense)) { throw "Pinned GeographicLib license is missing" }
$licensesDir = Join-Path $OutputDir "licenses"
Ensure-Directory $licensesDir
Copy-Item -Path $geographicLicense -Destination (Join-Path $licensesDir "GeographicLib.txt") -Force
Copy-Item -Path (Join-Path $root "third_party\boost-geometry.LICENSE.txt") -Destination (Join-Path $licensesDir "Boost.txt") -Force
Copy-IfExists (Join-Path $root "WHATS_NEW.txt") $OutputDir
Copy-IfExists (Join-Path $root "out\release-manifest.json") $OutputDir
Copy-IfExists (Join-Path $root "out\release-sbom.json") $OutputDir

Set-Content -Path (Join-Path $binDir "qt.conf") -Encoding ASCII -Value @"
[Paths]
Prefix=.
"@

Set-Content -Path (Join-Path $OutputDir "degree_node.cmd") -Encoding ASCII -Value @"
@echo off
setlocal
if not exist "%~dp0bin\degree_node.exe" (
  echo degree_node.exe is not included in this bundle. Use geocoin-node.cmd or rebuild with BUILD_NODE_CLI=ON.
  exit /b 127
)
"%~dp0bin\degree_node.exe" %*
"@

Set-Content -Path (Join-Path $OutputDir "geocoin-node.cmd") -Encoding ASCII -Value @"
@echo off
setlocal
if not exist "%~dp0bin\geocoin-node.exe" (
  echo geocoin-node.exe is not included in this bundle. Rebuild with BUILD_NODE_CLI=ON.
  exit /b 127
)
"%~dp0bin\geocoin-node.exe" %*
"@

Set-Content -Path (Join-Path $OutputDir "README.txt") -Encoding UTF8 -Value @"
GeoCoin Windows static release bundle

Что запускать:
- bin\geocoin-core.exe
  Обычный GUI кошелёк. Это Windows GUI subsystem: консольное окно не должно открываться.
  GUI запускает локальную full node, хранит wallet.dat и подключается к P2P-нодам.

- geocoin-node.cmd или bin\geocoin-node.exe
  Консольная full node для сервера или локальной диагностики.
  Тут консоль нормальна: это не UI.

- degree_node.cmd или bin\degree_node.exe
  Старый совместимый alias той же роли full node.

- bin\geocoin-rpc-miner.exe
  Только тестовый helper для майнинга через RPC getblocktemplate/submitblock.
  Использует отдельные miner RPC credentials, не admin/wallet credentials.
  Для обычного GUI-кошелька он не нужен.

- bin\geocoin-gpu-miner.exe
  Отдельный GPU miner для CUDA/HIP/OpenCL. GUI запускает его сам как дочерний процесс.
  При ручном solo mining использует отдельные miner RPC credentials.
  Вручную он нужен только для solo mining через RPC внешней или локальной ноды.

- genesis.exe, mine_genesis.exe, geocoin-miner.exe
  Диагностические/служебные инструменты, не запускать для обычной работы кошелька.

Статичность Windows release:
- Qt/OpenSSL/Curl/RocksDB/GeographicLib/zlib не кладутся рядом DLL-файлами.
- В этой папке не должно быть проектных *.dll.
- Системные DLL Windows остаются системными и не копируются в bundle.
- GPU:
  * CUDA требует установленный NVIDIA driver. NVIDIA SDK ставить не нужно.
  * OpenCL loader OpenCL.dll кладётся в bin, если найден при упаковке; vendor OpenCL ICD берётся из установленного GPU driver.
  * AMD HIP runtime DLL кладутся в bin, если найдены при упаковке; AMD driver всё равно должен быть установлен.

Быстрая проверка GUI:
bin\geocoin-core.exe -platform offscreen --smoke-test -no-auto-connect

Полный GUI e2e:
bin\geocoin-core.exe --gui-e2e-test --data-dir <temp-dir> -addnode=node1.geocoin.network:3030 -addnode=node2.geocoin.network:3030 -addnode=node3.geocoin.network:3030
"@

if ($staticRelease) {
    $dlls = Get-ChildItem -Path $OutputDir -Filter *.dll -Recurse -File -ErrorAction SilentlyContinue
    $allowedGpuRuntimePatterns = @(
        '^OpenCL\.dll$',
        '^amdhip64_.*\.dll$',
        '^amd_comgr_.*\.dll$'
    )
    $unexpectedDlls = @()
    foreach ($dll in $dlls) {
        $allowed = $false
        foreach ($pattern in $allowedGpuRuntimePatterns) {
            if ($dll.Name -match $pattern) {
                $allowed = $true
                break
            }
        }
        if (!$allowed) {
            $unexpectedDlls += $dll
        }
    }
    if ($unexpectedDlls.Count -gt 0) {
        $list = ($unexpectedDlls | ForEach-Object { $_.FullName }) -join [Environment]::NewLine
        throw "Static Windows release must not contain project/Qt/OpenSSL/Curl/RocksDB DLL files. Found:$([Environment]::NewLine)$list"
    }

    $checker = Join-Path $root "scripts\check_windows_static_imports.ps1"
    if (Test-Path $checker) {
        & powershell -ExecutionPolicy Bypass -File $checker -BundleDir $OutputDir
        if ($LASTEXITCODE -ne 0) {
            throw "Static import check failed"
        }
    }
}

Ensure-Directory $PortableRoot
if (Test-Path $ArchivePath) {
    Remove-Item -Path $ArchivePath -Force
}
Compress-Archive -Path (Join-Path $OutputDir "*") -DestinationPath $ArchivePath -CompressionLevel Optimal

Write-Host "[portable-windows] mode: $(if ($staticRelease) { 'static' } else { 'dynamic' })"
Write-Host "[portable-windows] bundle directory: $OutputDir"
Write-Host "[portable-windows] archive: $ArchivePath"
