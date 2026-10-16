# Run only on an isolated GitHub Actions runner.
$ErrorActionPreference = 'Stop'
$source = Join-Path $env:GITHUB_WORKSPACE 'release/obs-textppt'
$parent = Join-Path $env:ProgramData 'obs-studio/plugins'
$target = Join-Path $parent 'obs-textppt'
if (Test-Path -LiteralPath $target) { throw 'The test requires an empty installation path' }
New-Item -ItemType Directory -Path $parent -Force | Out-Null
Copy-Item -LiteralPath $source -Destination $target -Recurse
Set-Content -LiteralPath (Join-Path $target 'obsolete.txt') -Value 'old plugin file'
$sibling = Join-Path $parent 'installer-test.txt'
Set-Content -LiteralPath $sibling -Value 'preserve'
$installer = Join-Path $env:GITHUB_WORKSPACE 'release/install_obs-textppt.bat'
$log = Join-Path $env:RUNNER_TEMP 'textppt-installer.log'
$timer = [Diagnostics.Stopwatch]::StartNew()
# Run the real batch installer with a hidden console.
$process = Start-Process -FilePath "$env:SystemRoot/System32/cmd.exe" -ArgumentList "/d /c `"`"$installer`" > `"$log`" 2>&1`"" -WindowStyle Hidden -PassThru
if (-not $process.WaitForExit(60000)) {
    Get-Content -LiteralPath $log
    $process.Kill()
    throw 'The installer did not finish within 60 seconds'
}
$process.Refresh()
Get-Content -LiteralPath $log
if ($process.ExitCode -ne 0) { throw "Installer failed: $($process.ExitCode)" }
if ($timer.Elapsed.TotalSeconds -lt 4) { throw 'The five-second countdown did not run' }
$prompts = [regex]::Matches([IO.File]::ReadAllText($log), '\rClosing in ([1-5])s\.\.\. Press any key to exit\. ')
if (-not $prompts.Count -or $prompts[0].Groups[1].Value -ne '5') {
    throw 'The single-line English countdown did not start'
}
for ($index = 1; $index -lt $prompts.Count; ++$index) {
    if ([int]$prompts[$index].Groups[1].Value -ge [int]$prompts[$index - 1].Groups[1].Value) {
        throw 'The countdown did not decrease'
    }
}
if (Test-Path -LiteralPath (Join-Path $target 'obsolete.txt')) { throw 'Obsolete files remain' }
if ((Get-Content -LiteralPath $sibling) -ne 'preserve') { throw 'The sibling file was changed' }
$binary = 'bin/64bit/obs-textppt.dll'
if ((Get-FileHash (Join-Path $target $binary)).Hash -ne (Get-FileHash (Join-Path $source $binary)).Hash) {
    throw 'The installed binary differs from the package'
}
foreach ($language in 'en-US','zh-CN','zh-TW') {
    if (-not (Test-Path -LiteralPath (Join-Path $target "data/locale/$language.ini"))) { throw 'A locale is missing' }
}
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class TextPPTConsole {
    [StructLayout(LayoutKind.Explicit, CharSet = CharSet.Unicode, Size = 20)]
    public struct InputRecord {
        [FieldOffset(0)] public ushort Type;
        [FieldOffset(4)] public int Down;
        [FieldOffset(8)] public ushort Repeat;
        [FieldOffset(10)] public ushort Key;
        [FieldOffset(14)] public char Character;
    }
    [DllImport("kernel32.dll")] public static extern bool FreeConsole();
    [DllImport("kernel32.dll", SetLastError = true)] public static extern bool AttachConsole(uint pid);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern IntPtr CreateFile(string path, uint access, uint share, IntPtr security, uint creation, uint flags, IntPtr template);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern bool WriteConsoleInputW(IntPtr input, InputRecord[] records, uint count, out uint written);
    [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr handle);
}
'@
# Send real key events to the installer's own hidden console.
function Read-InstallerLog([string]$Path) {
    $stream = [IO.FileStream]::new($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
    $reader = [IO.StreamReader]::new($stream)
    try { $reader.ReadToEnd() } finally { $reader.Dispose() }
}
foreach ($key in @(@(0x58, 'x'), @(0x0D, "`r"), @(0x20, ' '), @(0x1B, [string][char]27))) {
    $keyLog = Join-Path $env:RUNNER_TEMP ('textppt-key-' + $key[0] + '.log')
    $process = Start-Process -FilePath "$env:SystemRoot/System32/cmd.exe" -ArgumentList "/d /c `"`"$installer`" > `"$keyLog`" 2>&1`"" -WindowStyle Hidden -PassThru
    $deadline = [Diagnostics.Stopwatch]::StartNew()
    while ($deadline.Elapsed.TotalSeconds -lt 30 -and -not $process.HasExited) {
        if ((Test-Path -LiteralPath $keyLog) -and ((Read-InstallerLog $keyLog) -match 'Closing in 5s')) { break }
        Start-Sleep -Milliseconds 20
    }
    if ($process.HasExited -or $deadline.Elapsed.TotalSeconds -ge 30) { throw 'Countdown prompt was not reached' }
    $null = [TextPPTConsole]::FreeConsole()
    if (-not [TextPPTConsole]::AttachConsole([uint32]$process.Id)) { throw 'Cannot attach to the isolated installer console' }
    $input = [TextPPTConsole]::CreateFile('CONIN$', 3221225472, 3, [IntPtr]::Zero, 3, 0, [IntPtr]::Zero)
    try {
        $record = New-Object TextPPTConsole+InputRecord
        $record.Type = 1; $record.Down = 1; $record.Repeat = 1
        $record.Key = [ushort]$key[0]; $record.Character = [char]$key[1]
        [uint32]$written = 0
        if (-not [TextPPTConsole]::WriteConsoleInputW($input, @($record), 1, [ref]$written) -or $written -ne 1) { throw 'Key injection failed' }
        if (-not $process.WaitForExit(2000)) { $process.Kill(); throw 'Any-key countdown exit took too long' }
        $process.Refresh()
        if ($process.ExitCode -ne 0) { throw 'Key exit changed the installer result' }
    } finally {
        $null = [TextPPTConsole]::CloseHandle($input)
        $null = [TextPPTConsole]::FreeConsole()
    }
}
Write-Output 'PASS: Windows upgrade, directory scope, binary, locales, timeout, and letter/Enter/Space/Escape exit'
