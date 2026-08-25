param(
    [string]$CubeIdeRoot = "C:\ST\STM32CubeIDE_2.1.1\STM32CubeIDE"
)

$ErrorActionPreference = "Stop"

$ProjectRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$BuildDir = Join-Path $ProjectRoot "STM32CubeIDE\Debug"

$Make = Get-ChildItem -Path (Join-Path $CubeIdeRoot "plugins") -Recurse -Filter make.exe |
    Select-Object -First 1 -ExpandProperty FullName
$ToolBin = Get-ChildItem -Path (Join-Path $CubeIdeRoot "plugins") -Recurse -Filter arm-none-eabi-gcc.exe |
    Select-Object -First 1 -ExpandProperty DirectoryName

if (-not $Make) {
    throw "make.exe not found under $CubeIdeRoot"
}
if (-not $ToolBin) {
    throw "arm-none-eabi toolchain not found under $CubeIdeRoot"
}

$env:PATH = "$ToolBin;$(Split-Path $Make);$env:PATH"

$GeneratedMakefiles = Get-ChildItem -Path $BuildDir -Recurse -Filter *.mk
$O0Flags = $GeneratedMakefiles | Select-String -SimpleMatch " -O0 "
$O2Flags = $GeneratedMakefiles | Select-String -SimpleMatch " -O2 "

if ($O0Flags) {
    throw "Generated CubeIDE makefiles still use -O0. Regenerate the Debug build files from the -O2 .cproject configuration before benchmarking."
}
if (-not $O2Flags) {
    throw "No -O2 compiler flag found in the generated CubeIDE makefiles."
}

Push-Location $BuildDir
try {
    & $Make -j1 all
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }

    $Elf = Get-ChildItem -Path . -Filter *.elf | Select-Object -First 1 -ExpandProperty FullName
    if (-not $Elf) {
        throw "No ELF file found in $BuildDir"
    }

    $Nm = Join-Path $ToolBin "arm-none-eabi-nm.exe"
    & $Nm $Elf |
        Select-String "Reset_Handler|openvlc_boot_step|openvlc_stm32_tx_init"
}
finally {
    Pop-Location
}
