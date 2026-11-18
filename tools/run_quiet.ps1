# run_quiet.ps1 — run a command with Windows hard-error popups suppressed.
#
# GATES MUST BE RUN THROUGH THIS SCRIPT. Any ctest / test-binary invocation in this tree should be
# launched as `powershell -NoProfile -File tools/run_quiet.ps1 -Command "ctest -j8 ..." -WorkDir <build>`
# rather than invoked directly. Reason: a test executable built against a stale MinGW libstdc++ (or
# otherwise load-broken) fails at LOAD time, before main(), and Windows answers that with a csrss-owned
# modal hard-error dialog that no harness can dismiss and that survives the process — one stale build
# tree can therefore wedge a whole gate run and litter the operator's desktop with unclosable windows
# (see docs/engineering/TEST_TREE_HEALTH.md). SetErrorMode is inherited by children, so setting it once
# here turns every load failure in the run into a silent non-zero exit code instead.
#
# WHY: test binaries that fail at LOAD time (MinGW libstdc++ ABI mismatch, missing DLL,
# corrupt PE) do not exit with an error code -- they raise a modal "Entry Point Not Found"
# hard-error dialog *before* main() runs. The dialog is owned by csrss, so nothing can
# dismiss it programmatically except posting BM_CLICK, and the child just sits there until
# the harness timeout. One stale build tree can therefore wedge an entire gate run and
# litter the desktop with undismissable windows.
#
# SetErrorMode is INHERITED by child processes on Windows, so setting it here covers every
# test binary the harness spawns. With SEM_FAILCRITICALERRORS the loader failure becomes a
# silent failure with a non-zero exit code, which is what a gate actually wants.
#
# Usage:
#   powershell -NoProfile -File run_quiet.ps1 -Command "ctest -j8 --output-on-failure"
#   powershell -NoProfile -File run_quiet.ps1 -Command "ctest ... " -WorkDir "M:\...\build-b0-tests"

param(
    [Parameter(Mandatory=$true)][string]$Command,
    [string]$WorkDir = "",
    [int]$TimeoutMin = 60
)

$ErrorActionPreference = 'Continue'

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class ErrMode {
    [DllImport("kernel32.dll")] public static extern uint SetErrorMode(uint uMode);
    public const uint SEM_FAILCRITICALERRORS = 0x0001;
    public const uint SEM_NOGPFAULTERRORBOX  = 0x0002;
    public const uint SEM_NOOPENFILEERRORBOX = 0x8000;
    public static uint Quiet() {
        return SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    }
}
"@

$prev = [ErrMode]::Quiet()
Write-Host ("error mode set (previous=0x{0:X}) -- hard-error dialogs suppressed for this tree" -f $prev)

if ($WorkDir -ne "") { Set-Location -LiteralPath $WorkDir }

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $env:ComSpec
$psi.Arguments = '/c ' + $Command
$psi.UseShellExecute = $false
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$p = [System.Diagnostics.Process]::Start($psi)
$out = $p.StandardOutput.ReadToEnd()
$err = $p.StandardError.ReadToEnd()
if (-not $p.WaitForExit($TimeoutMin * 60 * 1000)) {
    Write-Host "TIMEOUT after $TimeoutMin min -- killing tree"
    try { $p.Kill($true) } catch {}
    exit 124
}
Write-Output $out
if ($err -ne "") { [Console]::Error.Write($err) }
Write-Host ("exit={0}" -f $p.ExitCode)
exit $p.ExitCode
