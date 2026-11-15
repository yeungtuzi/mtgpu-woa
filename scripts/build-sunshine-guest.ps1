# 在 guest（Windows 11 ARM64）内源码构建 Foundation Sunshine
# 用法（管理员 PowerShell）：scripts/build-sunshine-guest.ps1
# 依赖：MSYS2 ARM64（CLANGARM64）、CMake/Ninja、Node.js ARM64（WebUI）
$ErrorActionPreference = 'Stop'
$Root = 'C:\mtgpu-woa'
$Src  = Join-Path $Root 'src\foundation-sunshine'
$Log  = Join-Path $Root 'build-sunshine.log'
$Msys = 'C:\msys64'
$MsysExe = Join-Path $Msys 'msys2_shell.cmd'

function Log($m) { $m | Tee-Object -FilePath $Log -Append }

Log "== 1. MSYS2 ARM64 =="
if (-not (Test-Path $MsysExe)) {
    $u = 'https://github.com/msys2/msys2-installer/releases/download/nightly-x86_64/msys2-arm64-latest.exe'
    $out = Join-Path $Root 'msys2-arm64.exe'
    if (-not (Test-Path $out) -or (Get-Item $out).Length -lt 80000000) {
        Log "下载 MSYS2 ARM64 ..."
        & curl.exe -L --retry 3 --max-time 600 -o $out $u
    }
    $sz = (Get-Item $out).Length
    Log "安装包大小=$sz"
    if ($sz -lt 80000000) { throw "MSYS2 安装包不完整（$sz）" }
    $env:QT_QPA_PLATFORM = 'minimal'
    $p = Start-Process -FilePath $out -ArgumentList 'in','--confirm-command','--accept-messages','--root','C:/msys64' -Wait -PassThru
    if (-not (Test-Path $MsysExe)) { throw "MSYS2 安装失败（exit=$($p.ExitCode)）" }
}
Log "MSYS2 OK: $MsysExe"

function Msys([string]$cmd) {
    & $MsysExe -clangarm64 -no-start -defterm -c $cmd
    if ($LASTEXITCODE -ne 0) { throw "MSYS2 命令失败($LASTEXITCODE): $cmd" }
}

Log "== 2. pacman 依赖 =="
Msys 'pacman -Syu --noconfirm'
Msys 'pacman -Syu --noconfirm'
Msys 'pacman -S --needed --noconfirm git mingw-w64-clang-aarch64-boost mingw-w64-clang-aarch64-cmake mingw-w64-clang-aarch64-cppwinrt mingw-w64-clang-aarch64-curl-winssl mingw-w64-clang-aarch64-miniupnpc mingw-w64-clang-aarch64-onevpl mingw-w64-clang-aarch64-openssl mingw-w64-clang-aarch64-opus mingw-w64-clang-aarch64-toolchain mingw-w64-clang-aarch64-qt6-static'

Log "== 3. 源码 =="
New-Item -ItemType Directory -Force -Path (Split-Path $Src) | Out-Null
if (-not (Test-Path (Join-Path $Src '.git'))) {
    Msys "git clone --recurse-submodules https://github.com/AlkaidLab/foundation-sunshine.git /c/mtgpu-woa/src/foundation-sunshine"
} else {
    Msys "cd /c/mtgpu-woa/src/foundation-sunshine && git submodule update --init --recursive"
}

Log "== 4. 构建 =="
Msys 'cd /c/mtgpu-woa/src/foundation-sunshine && cmake -B build -G Ninja -S . && ninja -C build'

Log "== 完成 =="
Log '产物：C:\mtgpu-woa\src\foundation-sunshine\build\sunshine.exe（具体路径以构建输出为准）'
