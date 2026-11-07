# do-windows.ps1 - builds SynthEdit ON WINDOWS, with llvm-mingw. The native twin of tools/do-windows
# (which cross-builds on the Mac); see Docs/windows-port.md.
#
#   powershell -ExecutionPolicy Bypass -File tools\do-windows.ps1 [-Arch arm64|x64] [-Debug] [-Clean]
#
# Result: build-win-<arch>\SynthEdit.exe, one self-contained file, with layouts\ beside it.
# Release (the default) is a GUI program at -Os; -Debug keeps the console window for the log, at -O1.
# GLFW and FreeType are compiled from SynthLib\ThirdParty once per architecture - no CMake.
#
# Environment: LLVM_MINGW, SYNTHEDIT_WIN_VERSION (the About box), SYNTHEDIT_WERROR=ON.

param([string]$Arch = $(if ($env:PROCESSOR_ARCHITECTURE -eq 'ARM64') { 'arm64' } else { 'x64' }),
      [switch]$Debug, [switch]$Clean)
. "$PSScriptRoot\windows-common.ps1"

$t = Get-Toolchain $Arch
if ($Clean) { Remove-Item -Recurse -Force $t.Out; $t = Get-Toolchain $Arch }

$libs = @((Build-Glfw $t), (Build-FreeType $t))

# --- the sources: the synchronized folders, less platform\windows\swaps.txt's, plus platform\windows ---
$win = "$Root\platform\windows"
$leftOut = Get-Content "$win\swaps.txt" | Where-Object { $_ -and -not $_.StartsWith('#') } |
           ForEach-Object { ($_ -split '\s+')[0] -replace '^SynthLib/', '../SynthLib/' } |
           ForEach-Object { [IO.Path]::GetFullPath((Join-Path $Root $_)) }
$sources = @(Get-ChildItem "$Root\src\*", "$SynthLib\src\*" -Include *.c, *.cpp | ForEach-Object FullName) +
           @(Get-ChildItem "$win\*.c" | ForEach-Object FullName)
$sources = @($sources | Where-Object { $leftOut -notcontains $_ })

# The warning switches the Xcode project turns on (its CLANG_WARN_* / GCC_WARN_* settings)
$warnings = @('-Werror=uninitialized', '-Wunused-function', '-Wunused-variable', '-Werror=return-type',
              '-Wuninitialized', '-Wempty-body', '-Wint-conversion', '-Wenum-conversion', '-Wstrict-prototypes',
              '-Wshorten-64-to-32', '-Wcomma', '-Wbool-conversion', '-Wconstant-conversion',
              '-Winfinite-recursion', '-Wunreachable-code', '-Wdocumentation')
if ($env:SYNTHEDIT_WERROR -eq 'ON') { $warnings += '-Werror' }

$config = @(if ($Debug) { '-O1', '-g', '-DDEBUG=1', '-DENABLE_LOG_DEBUG=1' } else { '-Os' })    # @(): one item stays an array
$version = Get-VersionString
$defs = @('-DGL_SILENCE_DEPRECATION=1', '-D_USE_MATH_DEFINES', '-D_POSIX_THREAD_SAFE_FUNCTIONS=200112L')
if ($version -ne 'development build') { $defs += Quote "-DSYNTHLIB_VERSION_STRING=`"$version`"" }
$incs = @("$SynthLib\src", "$Root\src", $win) | ForEach-Object { Quote "-I$_" }
# the third-party headers as system headers - their warnings are not ours
$incs += @("$Third\glfw\include", "$Third\freetype\include") | ForEach-Object { '-isystem', (Quote $_) }
$flags = $config + $defs + $incs + @('-include', (Quote "$win\winCompat.h"))

$obj = "$($t.Out)\app-obj"
if (Test-Path $obj) { Remove-Item -Recurse -Force $obj }
New-Item -ItemType Directory -Force $obj | Out-Null

Write-Host "==> SynthEdit for $($t.Triple), $(if ($Debug) { 'Debug' } else { 'Release' }) ($($sources.Count) files)"
$jobs = @()
foreach ($s in $sources) {
    $isCxx = $s.EndsWith('.cpp')
    $prefix = if ($s.StartsWith($SynthLib)) { 'sl_' } else { '' }    # SynthLib and the app may share a file name
    $o = Join-Path $obj ($prefix + ([IO.Path]::GetFileNameWithoutExtension($s)) + $(if ($isCxx) { '.oxx' } else { '.o' }))
    $jobs += New-CompileJob $(if ($isCxx) { $t.CXX } else { $t.CC }) $s $o `
             (@($(if ($isCxx) { '-std=gnu++20' } else { '-std=gnu17' })) + $flags + $warnings)
}
Invoke-Compile $jobs

$exe = "$($t.Out)\SynthEdit.exe"
$gui = @(if (-not $Debug) { '-mwindows' })    # a Release .exe opens no console window
& $t.CXX -static -o $exe @($jobs | ForEach-Object Obj) $libs `
    -lopengl32 -lgdi32 -luser32 -lshell32 -lole32 -lcomdlg32 -lwinmm -lpthread @gui
if ($LASTEXITCODE) { throw 'link failed' }

# The layouts folder the application looks for beside it (synthGraphics.c: "layouts", relative to the working folder)
$layouts = "$($t.Out)\layouts"
if (Test-Path $layouts) { Remove-Item -Recurse -Force $layouts }
Copy-Item -Recurse "$Root\layouts" $layouts

Write-Host ""
$info = Show-Binary $t $exe
Write-Host "needs: $($info.Needs)"
