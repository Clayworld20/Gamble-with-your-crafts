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

$extra = @()
if ($NoTests) {
    $extra += "-DGWYF_BUILD_TESTS=OFF"
}
if ($JavaHome -ne "") {
    $extra += "-DJAVA_HOME=$JavaHome"
}

Write-Host "`n[1/3] Настройка проекта..." -ForegroundColor Yellow

# Генератор подбираем по факту: Visual Studio 2022, иначе Visual Studio 2026
# (VS 18), иначе — тот, что CMake выберет сам. Жёсткая привязка к 17 2022
# ломается на машинах только с новым VS («could not find any instance»).
$configured = $false
foreach ($generator in @("Visual Studio 17 2022", "Visual Studio 18 2026")) {
    & cmake -S "$PSScriptRoot" -B $BuildDir -G $generator -A x64 @extra
    if ($LASTEXITCODE -eq 0) {
        $configured = $true
        break
    }
    Write-Host "Генератор '$generator' недоступен — пробую следующий." -ForegroundColor DarkYellow
}

if (-not $configured) {
    Write-Host "Явные генераторы не подошли — отдаю выбор CMake (генератор по умолчанию)." -ForegroundColor DarkYellow
    & cmake -S "$PSScriptRoot" -B $BuildDir @extra
    if ($LASTEXITCODE -ne 0) {
        Write-Error "CMake не смог настроить проект. Проверьте, что установлены CMake и Visual Studio с рабочей нагрузкой «Разработка классических приложений на C++»."
    }
}

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
