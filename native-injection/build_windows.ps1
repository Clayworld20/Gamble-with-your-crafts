# ============================================================================
#  build_windows.ps1 — сборка нативной части под Visual Studio из командной
#  строки. Скрипт делает ровно то, что делал бы человек: настраивает CMake под
#  VS 2022 x64, собирает Release и прогоняет самотест.
#
#  Запуск:  powershell -ExecutionPolicy Bypass -File native-injection\build_windows.ps1
#  Ключи:   -BuildDir <путь>   каталог сборки (по умолчанию native-injection\build)
#           -JavaHome <путь>   путь к JDK (для gwyf_native_bridge.dll)
#           -NoTests           не собирать самотест и синтетический хост
# ============================================================================
param(
    [string]$BuildDir = "$PSScriptRoot\build",
    [string]$JavaHome = "",
    [switch]$NoTests
)

$ErrorActionPreference = "Stop"

Write-Host "=== gwyfbridge: сборка нативной части ===" -ForegroundColor Cyan
Write-Host "Каталог сборки: $BuildDir"

$cmake = Get-Command cmake -ErrorAction SilentlyContinue
if (-not $cmake) {
    Write-Error "CMake не найден в PATH. Установите CMake или используйте Visual Studio (File → Open → Folder)."
}

$arguments = @("-S", "$PSScriptRoot", "-B", $BuildDir, "-G", "Visual Studio 17 2022", "-A", "x64")

if ($NoTests) {
    $arguments += "-DGWYF_BUILD_TESTS=OFF"
}

if ($JavaHome -ne "") {
    $arguments += "-DJAVA_HOME=$JavaHome"
}

Write-Host "`n[1/3] Настройка проекта..." -ForegroundColor Yellow
& cmake @arguments

Write-Host "`n[2/3] Сборка Release..." -ForegroundColor Yellow
& cmake --build $BuildDir --config Release --parallel

Write-Host "`n[3/3] Самотест..." -ForegroundColor Yellow
if ($NoTests) {
    Write-Host "Самотест отключён ключом -NoTests." -ForegroundColor DarkYellow
} else {
    Push-Location $BuildDir
    try {
        & ctest -C Release --output-on-failure
    } finally {
        Pop-Location
    }
}

Write-Host "`n=== Готово ===" -ForegroundColor Green
Write-Host "Артефакты находятся в $BuildDir\Release:"
foreach ($artifact in @("GWYF_HookEngine.dll",
                        "gwyf_native_bridge.dll",
                        "gwyfbridge.exe",
                        "gwyfbridge_selftest.exe",
                        "fake_mono_host.dll")) {
    $path = Join-Path $BuildDir "Release\$artifact"
    if (Test-Path $path) {
        Write-Host ("  [есть]   {0}" -f $artifact)
    } else {
        Write-Host ("  [пропущен] {0}" -f $artifact) -ForegroundColor DarkYellow
    }
}
