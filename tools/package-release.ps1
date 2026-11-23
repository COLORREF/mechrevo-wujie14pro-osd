param(
    [string]$Wix = 'wix',
    [string[]]$ConfigureArguments = @()
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root 'build/package-release'
$bin = Join-Path $build 'bin'
$dist = Join-Path $root 'dist'
$msi = Join-Path $build 'YaoOsd.msi'
$product = Join-Path $root 'packaging/Product.wxs'
$localization = Join-Path $root 'packaging/zh-cn.wxl'
$wixCommand = (Get-Command $Wix -CommandType Application).Source
$cache = Join-Path $build 'CMakeCache.txt'
if ($ConfigureArguments.Count -eq 0 -and -not (Test-Path -LiteralPath $cache)) {
    $ConfigureArguments = @('-A', 'x64')
}

cmake -S $root -B $build -D CMAKE_BUILD_TYPE=Release `
    -D CMAKE_TRY_COMPILE_CONFIGURATION=Release -D "OSD_SETUP_MSI=$msi" `
    -D "CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE=$bin" `
    -D "CMAKE_LIBRARY_OUTPUT_DIRECTORY_RELEASE=$bin" @ConfigureArguments
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }

cmake --build $build --config Release --target osd_hotkey osd_setup_check
if ($LASTEXITCODE -ne 0) { throw 'Release build failed.' }

$app = Join-Path $bin 'osd_hotkey.exe'
$check = Join-Path $bin 'osd_setup_check.dll'
$versionLine = Select-String -LiteralPath $cache -Pattern '^CMAKE_PROJECT_VERSION:STATIC=(.+)$'
if (-not $versionLine) { throw 'CMake did not report a product version.' }
$version = $versionLine.Matches[0].Groups[1].Value

& $wixCommand build $product $localization -ext WixToolset.UI.wixext -arch x64 -culture zh-cn `
    -d "AppExe=$app" -d "FolderCheckDll=$check" -d "ProductVersion=$version" `
    -intermediatefolder (Join-Path $build 'wix') -pdbtype none -out $msi
if ($LASTEXITCODE -ne 0) { throw 'MSI build failed.' }

cmake --build $build --config Release --target osd_setup
if ($LASTEXITCODE -ne 0) { throw 'EXE installer build failed.' }

New-Item -ItemType Directory -Path $dist -Force | Out-Null
$prefix = "YaoOsd-$version-win-x64"
Copy-Item -LiteralPath $app -Destination (Join-Path $dist "$prefix-portable.exe") -Force
Copy-Item -LiteralPath $msi -Destination (Join-Path $dist "$prefix.msi") -Force
Copy-Item -LiteralPath (Join-Path $bin 'osd_setup.exe') -Destination (Join-Path $dist "$prefix-setup.exe") -Force
