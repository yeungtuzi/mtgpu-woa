# 在 guest（Windows 11 ARM64）内用 MSYS2 CLANGARM64 从源码构建 upstream Sunshine。
# 与 Foundation Sunshine 不同，upstream 官方支持 CLANGARM64（ARM64），
# 且其 Windows 编码器表包含 mediafoundation（FFmpeg h264_mf）。
#
# 用法（guest 管理员 PowerShell）：
#   powershell -NoProfile -ExecutionPolicy Bypass -File C:\mtgpu-woa\build-sunshine-upstream.ps1
#
# 产物：C:\mtgpu-woa\src\sunshine\build\sunshine.exe
# 安装：把 build 目录内容复制到 C:\mtgpu-woa\sunshine（与官方安装目录分开，便于回滚）
$ErrorActionPreference = 'Stop'
$Msys = 'C:\msys64\msys2_shell.cmd'
$Root = 'C:\mtgpu-woa'
$Src  = Join-Path $Root 'src\sunshine'
$Log  = Join-Path $Root 'sunshine-upstream-build.log'

function Log($m) {
    $line = "[{0}] {1}" -f (Get-Date -Format 'HH:mm:ss'), $m
    Write-Host $line
    Add-Content -Path $Log -Value $line
}

if (-not (Test-Path $Msys)) { throw "MSYS2 not found: $Msys" }

Log "== dependencies (CLANGARM64) =="
& $Msys -clangarm64 -no-start -defterm -c @'
pacman -S --needed --noconfirm git mingw-w64-clang-aarch64-boost mingw-w64-clang-aarch64-cmake mingw-w64-clang-aarch64-cppwinrt mingw-w64-clang-aarch64-curl-winssl mingw-w64-clang-aarch64-miniupnpc mingw-w64-clang-aarch64-onevpl mingw-w64-clang-aarch64-openssl mingw-w64-clang-aarch64-opus mingw-w64-clang-aarch64-toolchain mingw-w64-clang-aarch64-qt6-static
'@ 2>&1 | Tee-Object -FilePath $Log -Append | Out-Null

Log "== source =="
New-Item -ItemType Directory -Force -Path (Join-Path $Root 'src') | Out-Null
if (-not (Test-Path (Join-Path $Src '.git'))) {
    & $Msys -clangarm64 -no-start -defterm -c @"
git clone --depth 1 --recurse-submodules --shallow-submodules https://github.com/LizardByte/Sunshine.git /c/mtgpu-woa/src/sunshine
"@ 2>&1 | Tee-Object -FilePath $Log -Append | Out-Null
} else {
    & $Msys -clangarm64 -no-start -defterm -c 'cd /c/mtgpu-woa/src/sunshine && git submodule update --init --recursive --depth 1' 2>&1 |
        Tee-Object -FilePath $Log -Append | Out-Null
}

Log "== cmake =="
& $Msys -clangarm64 -no-start -defterm -c @'
cd /c/mtgpu-woa/src/sunshine
cmake -B build -G Ninja -S . \
  -DSUNSHINE_ENABLE_TRAY=OFF -DSUNSHINE_REQUIRE_TRAY=OFF \
  -DCUDA_FAIL_ON_MISSING=OFF -DBUILD_TESTS=OFF -DBUILD_DOCS=OFF
'@ 2>&1 | Tee-Object -FilePath $Log -Append | Out-Null

Log "== ninja =="
& $Msys -clangarm64 -no-start -defterm -c 'cd /c/mtgpu-woa/src/sunshine && ninja -C build' 2>&1 |
    Tee-Object -FilePath $Log -Append | Out-Null

$exe = Join-Path $Src 'build\sunshine.exe'
if (Test-Path $exe) {
    Log "built: $exe"
    $out = Join-Path $Root 'sunshine'
    New-Item -ItemType Directory -Force -Path $out | Out-Null
    Copy-Item (Join-Path $Src 'build\*') $out -Recurse -Force
    Log "installed: $out (do not overwrite C:\Program Files\Sunshine without a backup)"
} else {
    Log "ERROR: sunshine.exe not produced; see $Log"
    exit 1
}
