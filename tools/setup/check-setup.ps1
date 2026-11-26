# Puts VeloPlay-Setup.exe together from the setup program and a package, and tries it out.
#
#   check-setup.ps1 -Program <VeloPlay-Setup-Program.exe> -Package <folder of the unpacked package>
#                   -Out <folder for what is made> [-Zip <archive to attach>] [-Install -Version <x.y.z>]
#
# Without -Install nothing is installed: the package is attached, read back and unpacked into a
# temporary folder, and that is all. That much may run on any PC, also the one that builds VeloPlay
# every day. With -Install the file installs VeloPlay for this user, installs it once more over
# itself and removes it again. That belongs on a machine that is thrown away afterwards.
#
# Prints one line "SETUP-CHECK ok|FAILED ..." per step and ends with 1 if one of them failed.
param(
  [Parameter(Mandatory)] [string]$Program,
  [Parameter(Mandatory)] [string]$Package,
  [Parameter(Mandatory)] [string]$Out,
  [string]$Zip,
  [switch]$Install,
  [string]$Version
)
$ErrorActionPreference = 'Stop'
$ok = $true
function Check([string]$what, [bool]$fine) {
  Write-Host ("SETUP-CHECK " + $(if ($fine) { 'ok    ' } else { 'FAILED' }) + " $what")
  if (-not $fine) { $script:ok = $false }
}
function Run([string]$file, [string[]]$arguments) {
  # The setup program has no console; without -Wait PowerShell would not wait for it.
  $process = Start-Process -FilePath $file -ArgumentList $arguments -PassThru
  $null = $process.Handle # without it PowerShell forgets the exit code of a process it did not wait for
  if (-not $process.WaitForExit(180000)) { $process.Kill(); return -1 }
  return $process.ExitCode
}

# The names of the libraries a program asks Windows for, read by whatever tool this machine has.
# Visual Studio brings its tools once per kind of processor; only the ones for x64 start here.
function Imports([string]$file) {
  $tools = @()
  $onPath = (Get-Command llvm-objdump -ErrorAction SilentlyContinue).Source
  if ($onPath) { $tools += @{ File = $onPath; Arguments = @('-p'); Pattern = 'DLL Name:\s*(\S+)' } }
  $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
  if (Test-Path $vswhere) {
    $studio = & $vswhere -latest -products * -property installationPath | Select-Object -First 1
    if ($studio) {
      $found = Get-ChildItem (Join-Path $studio 'VC\Tools\Llvm') -Recurse -Filter llvm-objdump.exe -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match '\\x64\\bin\\' } | Select-Object -First 1 -ExpandProperty FullName
      if ($found) { $tools += @{ File = $found; Arguments = @('-p'); Pattern = 'DLL Name:\s*(\S+)' } }
      $found = Get-ChildItem (Join-Path $studio 'VC\Tools\MSVC') -Recurse -Filter dumpbin.exe -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match 'Hostx64\\x64' } | Select-Object -First 1 -ExpandProperty FullName
      if ($found) { $tools += @{ File = $found; Arguments = @('/nologo', '/dependents'); Pattern = '^\s+(\S+\.dll)\s*$' } }
    }
  }
  foreach ($tool in $tools) {
    try {
      $arguments = $tool.Arguments + $file
      $names = @(& $tool.File @arguments | Select-String $tool.Pattern | ForEach-Object { $_.Matches[0].Groups[1].Value })
      if ($names.Count -gt 0) {
        Write-Host "Read with $($tool.File)"
        return , $names
      }
    } catch {
      Write-Host "Could not use $($tool.File): $($_.Exception.Message)"
    }
  }
  return $null
}

$Program = (Resolve-Path $Program).Path
$Package = (Resolve-Path $Package).Path
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$Out = (Resolve-Path $Out).Path
if (-not $Zip) {
  $Zip = Join-Path $Out 'VeloPlay-windows-x64.zip'
  Remove-Item $Zip -ErrorAction SilentlyContinue
  Compress-Archive -Path (Join-Path $Package '*') -DestinationPath $Zip
}
$Zip = (Resolve-Path $Zip).Path
$setup = Join-Path $Out 'VeloPlay-Setup.exe'
Remove-Item $setup -ErrorAction SilentlyContinue

# It runs before anything is unpacked, so it may need nothing but Windows itself.
$needs = Imports $Program
if ($null -eq $needs) {
  Check 'a tool to read what the setup program needs (llvm-objdump or dumpbin)' $false
} else {
  $own = @($needs | Where-Object { $_ -notmatch '^(kernel32|user32|gdi32|shell32|ole32|advapi32)\.dll$' })
  Check "needs only Windows itself ($($needs -join ', '))" ($needs.Count -gt 0 -and $own.Count -eq 0)
}

$made = Run $Program @('--make', "`"$Zip`"", "`"$setup`"")
Check "package attached (exit $made)" ($made -eq 0 -and (Test-Path $setup))
if (Test-Path $setup) {
  $size = (Get-Item $setup).Length
  Check "size is program plus package ($size bytes)" ($size -eq (Get-Item $Program).Length + 16 + (Get-Item $Zip).Length)
  $looked = Run $setup @('--check')
  Check "unpacks and finds the program in it (exit $looked)" ($looked -eq 0)
  $bare = Run $Program @('--check')
  Check "says so when no package is attached (exit $bare)" ($bare -eq 2)
}

if ($Install -and $ok) {
  $target = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'Programs\VeloPlay'
  $link = Join-Path ([Environment]::GetFolderPath('Programs')) 'VeloPlay.lnk'
  $key = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\VeloPlay'
  $installed = Run $setup @('--quiet')
  Check "installs without a question (exit $installed)" ($installed -eq 0)
  Check "program at $target" (Test-Path (Join-Path $target 'VeloPlay.exe'))
  $missing = @(Get-ChildItem $Package -File | Where-Object { -not (Test-Path (Join-Path $target $_.Name)) })
  Check "every file of the package is there ($($missing.Count) missing)" ($missing.Count -eq 0)
  Check 'entry in the start menu' (Test-Path $link)
  $entry = Get-ItemProperty -Path $key -ErrorAction SilentlyContinue
  Check "listed as VeloPlay $($entry.DisplayVersion)" ($entry -and $entry.DisplayName -eq 'VeloPlay' -and "$($entry.DisplayVersion)" -like "$Version*")
  $left = @(Get-ChildItem $env:TEMP -Directory -Filter 'VeloPlay-Setup-*' -ErrorAction SilentlyContinue)
  Write-Host "Temporary folders left behind: $($left.Count)"
  # Over an installation that is there already, as someone would who fetches the setup file again.
  $again = Run $setup @('--quiet')
  Check "installs over itself (exit $again)" ($again -eq 0)
  if (Test-Path (Join-Path $target 'VeloPlay.exe')) {
    $removed = Run (Join-Path $target 'VeloPlay.exe') @('--uninstall', '--quiet')
    Check "removes itself again (exit $removed)" ($removed -eq 0)
  }
}

if (-not $ok) { exit 1 }
Write-Host "The setup file passed: $setup"
exit 0
