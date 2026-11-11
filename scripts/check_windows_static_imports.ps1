param(
    [Parameter(Mandatory = $true)]
    [string]$BundleDir
)

$ErrorActionPreference = "Stop"

if (!(Test-Path $BundleDir)) {
    throw "Bundle directory not found: $BundleDir"
}

$forbiddenPatterns = @(
    '^Qt5.*\.dll$',
    '^libcrypto.*\.dll$',
    '^libssl.*\.dll$',
    '^libcurl.*\.dll$',
    '^GeographicLib.*\.dll$',
    '^zlib.*\.dll$',
    '^zstd.*\.dll$',
    '^bz2.*\.dll$',
    '^libpng.*\.dll$',
    '^pcre2.*\.dll$',
    '^double-conversion.*\.dll$',
    '^glib.*\.dll$',
    '^harfbuzz.*\.dll$',
    '^freetype.*\.dll$',
    '^brotli.*\.dll$',
    '^vcruntime.*\.dll$',
    '^msvcp.*\.dll$'
)

function Get-ImportsWithDumpbin([string]$ExePath) {
    $dumpbin = (Get-Command dumpbin.exe -ErrorAction SilentlyContinue).Source
    if (!$dumpbin) {
        return $null
    }
    $text = & $dumpbin /DEPENDENTS $ExePath 2>$null
    if ($LASTEXITCODE -ne 0) {
        return $null
    }
    return $text | Where-Object { $_ -match '^\s*[^ ]+\.dll\s*$' } | ForEach-Object { $_.Trim() }
}

function Get-ImportsWithLlvmObjdump([string]$ExePath) {
    $objdump = (Get-Command llvm-objdump.exe -ErrorAction SilentlyContinue).Source
    if (!$objdump) {
        $objdump = (Get-Command llvm-readobj.exe -ErrorAction SilentlyContinue).Source
    }
    if (!$objdump) {
        return $null
    }
    $text = if ((Split-Path $objdump -Leaf) -eq "llvm-readobj.exe") {
        & $objdump --coff-imports $ExePath 2>$null
    } else {
        & $objdump -p $ExePath 2>$null
    }
    if ($LASTEXITCODE -ne 0) {
        return $null
    }
    return $text | Select-String -Pattern '[A-Za-z0-9_.+-]+\.dll' -AllMatches |
        ForEach-Object { $_.Matches.Value } | Sort-Object -Unique
}

$executables = Get-ChildItem -Path $BundleDir -Recurse -File |
    Where-Object { $_.Extension -in @('.exe', '.dll') }
$allForbidden = New-Object System.Collections.Generic.List[string]

foreach ($exe in $executables) {
    $imports = Get-ImportsWithDumpbin $exe.FullName
    if ($null -eq $imports) {
        $imports = Get-ImportsWithLlvmObjdump $exe.FullName
    }
    if ($null -eq $imports) {
        Write-Warning "Cannot inspect PE imports for $($exe.FullName): dumpbin/llvm tools not found"
        continue
    }

    foreach ($import in $imports) {
        foreach ($pattern in $forbiddenPatterns) {
            if ($import -match $pattern) {
                $allForbidden.Add("$($exe.Name) imports $import")
            }
        }
    }
}

if ($allForbidden.Count -gt 0) {
    $allForbidden | ForEach-Object { Write-Error $_ }
    exit 1
}

Write-Host "[static-imports] OK: no forbidden project/runtime DLL imports found in $BundleDir"

exit 0
