<#
.SYNOPSIS
Builds the release archive: a FOMOD installer with SeamlessSkyrim and, as options, the patched
Community Shaders for each release the fork is ported to.

.DESCRIPTION
The archive holds:
  fomod\info.xml, fomod\ModuleConfig.xml   package\fomod\, the version taken from CMakeLists.txt
  Core\SKSE\Plugins\SeamlessSkyrim.dll     Release build, without its PDB
  Core\SKSE\Plugins\SeamlessSkyrim.ini     package\, as a player gets it
  Core\SKSE\Plugins\SeamlessSkyrim_Doors.ini   package\, with the doors tuned in game moved in
  Core\SKSE\Plugins\SeamlessSkyrim_Translations\*.json   package\, the menu's text
  CommunityShaders-1.9.1\SKSE\Plugins\CommunityShaders.dll   the 1.9.1 fork's build
  CommunityShaders-1.9.1\...               every file that fork changed or added under package\
                                           and features\*\ since -CsBase
  CommunityShaders-2.0\...                 the same for the 2.0 port (-CsFork20, -CsBase20)
  LICENSE, SOURCE.txt                      the GPL text and where the source of both parts is
  THIRD-PARTY-NOTICES.txt                  the licenses of the libraries built into both DLLs

Each Community Shaders part is a separate option because it is only a patch: installed over
another release, or without Community Shaders, its DLL runs with shaders and data it doesn't
expect and rendering breaks. In MO2 the installer recommends the one for the installed release,
blocks the other, and blocks both when Bottled Shaders is installed (same DLL name).

Door tunings made in game (the game's SeamlessSkyrim_Doors_User.ini) are moved into package\'s
SeamlessSkyrim_Doors.ini first (tools\fold-door-tunings.ps1; commit them there). The user file is
never shipped: it is where a player's own tunings live, safe from updates.

.EXAMPLE
tools\make-release.ps1 -Build
#>
#Requires -Version 7
[CmdletBinding()]
param(
	[string]$GameData = 'E:\SteamLibrary\steamapps\common\Skyrim Special Edition\Data',
	[string]$CsFork = 'E:\projects\CommunityShaders-Seamless',
	# The Community Shaders release the fork is based on (a tag or commit in the fork).
	[string]$CsBase = 'v1.9.1',
	# The port to Community Shaders 2.0 (a worktree of the same repository) and its base.
	[string]$CsFork20 = 'E:\projects\CommunityShaders-Seamless-2.0',
	[string]$CsBase20 = 'v2.0.0-rc.2',
	# Build SeamlessSkyrim (Release) and each fork's CommunityShaders target first.
	[switch]$Build,
	# Written into the archive's SOURCE.txt.
	[string]$SourceUrl = 'https://github.com/OneManMonkeySquad/SeamlessSkyrim',
	[string]$CsSourceUrl = 'https://github.com/OneManMonkeySquad/CommunityShaders-Seamless',
	# Defaults to build\release\SeamlessSkyrim-<version>.zip.
	[string]$OutFile
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$plugins = 'SKSE\Plugins'

# One installer option each; Folder is the option's folder in the archive (ModuleConfig.xml).
$forks = @(
	[pscustomobject]@{ Name = 'Community Shaders 1.9.1'; Folder = 'CommunityShaders-1.9.1'; Path = $CsFork; Base = $CsBase; Branch = 'seamless-portal' }
	[pscustomobject]@{ Name = 'Community Shaders 2.0'; Folder = 'CommunityShaders-2.0'; Path = $CsFork20; Base = $CsBase20; Branch = 'seamless-portal-2.0' }
)

function Invoke-Git([string]$Repo) {
	$output = & git -C $Repo @args
	if ($LASTEXITCODE) { throw "git $args failed" }
	$output
}

# A stale DLL is the easiest way to ship the wrong thing. $Sources: everything it is built from,
# folders and single files.
function Assert-Newer([string]$Binary, [string[]]$Sources) {
	if (-not (Test-Path -LiteralPath $Binary)) { throw "$Binary is missing; run with -Build." }
	$newest = Get-ChildItem -LiteralPath $Sources -Recurse -File | Sort-Object LastWriteTime | Select-Object -Last 1
	if ($newest.LastWriteTime -gt (Get-Item -LiteralPath $Binary).LastWriteTime) {
		throw "$Binary is older than $($newest.FullName); run with -Build."
	}
}

$cmakeLists = Get-Content -Raw -LiteralPath (Join-Path $repo 'CMakeLists.txt')
if ($cmakeLists -notmatch 'project\s*\([^)]*\bVERSION\s+([0-9.]+)') { throw 'No project VERSION in CMakeLists.txt.' }
$version = $Matches[1]
if (-not $OutFile) { $OutFile = Join-Path $repo "build\release\SeamlessSkyrim-$version.zip" }

# ---- build -----------------------------------------------------------------------------------
if ($Build) {
	$cmake = (Get-Command cmake -ErrorAction SilentlyContinue)?.Source
	if (-not $cmake) {
		$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
		$cmake = & $vswhere -latest -prerelease -find 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' | Select-Object -First 1
	}
	if (-not $cmake) { throw 'cmake not found (neither on PATH nor in Visual Studio).' }

	& $cmake --build (Join-Path $repo 'build') --config Release
	if ($LASTEXITCODE) { throw 'SeamlessSkyrim build failed.' }
	foreach ($fork in $forks) {
		& $cmake --build (Join-Path $fork.Path 'build\ALL') --config Release --target CommunityShaders
		if ($LASTEXITCODE) { throw "$($fork.Name) build failed." }
	}
}

# ---- doors -----------------------------------------------------------------------------------
& (Join-Path $PSScriptRoot 'fold-door-tunings.ps1') -GameData $GameData
$doors = Join-Path $repo "package\$plugins\SeamlessSkyrim_Doors.ini"

# ---- what goes into the archive: path in the archive -> source file --------------------------
$files = [ordered]@{}

$dll = Join-Path $repo 'build\Release\SeamlessSkyrim.dll'
# Not src alone: the version the DLL reports comes from CMakeLists.txt, and after a bump without
# a build the archive was named for the new version with the old one's DLL in it.
Assert-Newer $dll ('src', 'extern\SKSEMenuFramework', 'CMakeLists.txt' | ForEach-Object { Join-Path $repo $_ })
$files["Core\$plugins\SeamlessSkyrim.dll"] = $dll
$ini = Join-Path $repo "package\$plugins\SeamlessSkyrim.ini"
if ((Get-Content -Raw -LiteralPath $ini) -match '(?m)^bEnabled=0') {
	Write-Warning "package\$plugins\SeamlessSkyrim.ini has bEnabled=0; players get the portals switched off."
}
$files["Core\$plugins\SeamlessSkyrim.ini"] = $ini
$files["Core\$plugins\SeamlessSkyrim_Doors.ini"] = $doors
foreach ($json in Get-ChildItem -LiteralPath (Join-Path $repo "package\$plugins\SeamlessSkyrim_Translations") -Filter *.json) {
	$files["Core\$plugins\SeamlessSkyrim_Translations\$($json.Name)"] = $json.FullName
}

foreach ($fork in $forks) {
	$csDll = Join-Path $fork.Path 'build\ALL\Release\CommunityShaders.dll'
	Assert-Newer $csDll (Join-Path $fork.Path 'src')
	$files["$($fork.Folder)\$plugins\CommunityShaders.dll"] = $csDll

	$forkChanged = @(Invoke-Git $fork.Path diff --name-only --diff-filter=d $fork.Base -- package features) +
		@(Invoke-Git $fork.Path ls-files --others --exclude-standard -- package features)
	foreach ($path in $forkChanged | Sort-Object -Unique) {
		# Community Shaders' packaging copies package\ and each features\<name>\ into Data as they are.
		$relative = ($path -replace '^(package|features/[^/]+)/', '') -replace '/', '\'
		$files["$($fork.Folder)\$relative"] = Join-Path $fork.Path $path
	}
	# The archive is built from the working tree; uncommitted fork changes would ship without a
	# commit to rebase onto the next Community Shaders release.
	$forkDirty = Invoke-Git $fork.Path status --porcelain -- src package features
	if ($forkDirty) { Write-Warning "$($fork.Name): the fork has uncommitted changes:`n$($forkDirty -join "`n")" }
	# SOURCE.txt points at the published branch: the GPL asks for the source of what ships.
	$published = & git -C $fork.Path rev-parse --verify --quiet "fork/$($fork.Branch)"
	if ($published -ne (Invoke-Git $fork.Path rev-parse $fork.Branch)) {
		Write-Warning "$($fork.Name): branch $($fork.Branch) isn't on the fork remote as built (git push fork $($fork.Branch))."
	}
}

# ---- third-party notices ---------------------------------------------------------------------
# Both DLLs link vcpkg's libraries statically and compile in the code under extern\; MIT and BSD
# ask for each one's notice to travel with the binary. Collected from the build trees, so a new
# dependency can't be left out.
# Compared without whitespace: the copies differ in line endings.
function Get-Words([string]$Path) { ((Get-Content -Raw -LiteralPath $Path) -replace '\s+', ' ').Trim() }
$gplWords = Get-Words (Join-Path $repo 'LICENSE')

function Get-Notices([string]$VcpkgShare, [string]$Extern, [hashtable]$More = @{}) {
	$sections = [ordered]@{}
	foreach ($dir in Get-ChildItem -LiteralPath $VcpkgShare -Directory) {
		# Folders without one hold only CMake config of a port with another name (nlohmann_json).
		$copyright = Join-Path $dir.FullName 'copyright'
		if (Test-Path -LiteralPath $copyright) { $sections[$dir.Name] = @($copyright) }
	}
	foreach ($dir in Get-ChildItem -LiteralPath $Extern -Directory) {
		$places = @($dir.FullName, (Join-Path $dir.FullName 'licenses')) | Where-Object { Test-Path -LiteralPath $_ }
		$sections[$dir.Name] = @(Get-ChildItem -LiteralPath $places -File |
			Where-Object { $_.Name -match '^(LICENSE|COPYING|EXCEPTIONS)|notices|licenses' -and $_.Extension -in '', '.txt', '.md' } |
			ForEach-Object FullName)
		if (-not $sections[$dir.Name]) { throw "No license file found in $($dir.FullName)." }
	}
	foreach ($name in $More.Keys) { $sections[$name] = @($More[$name]) }

	$text = [Text.StringBuilder]::new()
	foreach ($name in $sections.Keys) {
		[void]$text.Append("`n$('=' * 100)`n$name`n$('=' * 100)`n")
		foreach ($path in $sections[$name]) {
			$content = Get-Content -Raw -LiteralPath $path
			# CommonLib and Community Shaders are GPL-3 themselves; one copy of it is enough.
			if ((Get-Words $path) -eq $gplWords) { $content = "GNU General Public License, version 3: see LICENSE.`n" }
			[void]$text.Append("`n--- $(Split-Path $path -Leaf) ---`n`n$content")
		}
	}
	$text.ToString()
}

$notices = @"
SeamlessSkyrim.dll and the patched CommunityShaders.dll are built with the code below. Each part's
copyright notice and license follow.

####  In SeamlessSkyrim.dll  ####
$(Get-Notices (Join-Path $repo 'build\vcpkg_installed\x64-windows-static-md\share') (Join-Path $repo 'extern'))
$(foreach ($fork in $forks) {
	"`n####  In CommunityShaders.dll (the $($fork.Name) option)  ####`n"
	Get-Notices (Join-Path $fork.Path 'build\ALL\vcpkg_installed\x64-windows-static-md-release\share') (Join-Path $fork.Path 'extern') @{ 'Community Shaders' = (Join-Path $fork.Path 'EXCEPTIONS.md') }
})
"@

# ---- stage and zip ---------------------------------------------------------------------------
$stage = Join-Path ([IO.Path]::GetTempPath()) "SeamlessSkyrim-release-$PID"
if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
try {
	foreach ($relative in $files.Keys) {
		$to = Join-Path $stage $relative
		New-Item -ItemType Directory -Force (Split-Path $to) | Out-Null
		Copy-Item -LiteralPath $files[$relative] -Destination $to
	}
	# The GPL asks for the license text and the way to the source to travel with the binaries.
	Copy-Item -LiteralPath (Join-Path $repo 'LICENSE') -Destination $stage
	[IO.File]::WriteAllText((Join-Path $stage 'THIRD-PARTY-NOTICES.txt'), $notices)
	[IO.File]::WriteAllText((Join-Path $stage 'SOURCE.txt'), @"
SeamlessSkyrim $version is free software, licensed under the GNU General Public License,
version 3 or (at your option) any later version: see LICENSE. The libraries built into it and
into CommunityShaders.dll, and their licenses: see THIRD-PARTY-NOTICES.txt.

Source of SeamlessSkyrim:
  $SourceUrl
Source of the patched Community Shaders, in the branch of each option:
$(($forks | ForEach-Object { "  $($_.Name): $CsSourceUrl/tree/$($_.Branch)" }) -join "`n")

"@)

	$fomod = Join-Path $stage 'fomod'
	New-Item -ItemType Directory -Force $fomod | Out-Null
	Copy-Item -LiteralPath (Join-Path $repo 'package\fomod\ModuleConfig.xml') -Destination $fomod
	$info = Get-Content -Raw -LiteralPath (Join-Path $repo 'package\fomod\info.xml')
	[IO.File]::WriteAllText((Join-Path $fomod 'info.xml'), ($info -replace '<Version>[^<]*</Version>', "<Version>$version</Version>"))

	New-Item -ItemType Directory -Force (Split-Path $OutFile) | Out-Null
	if (Test-Path -LiteralPath $OutFile) { Remove-Item -LiteralPath $OutFile }
	# Not Compress-Archive: older versions of it write backslashes into entry names, which some
	# mod managers' extractors don't take as folders.
	Add-Type -AssemblyName System.IO.Compression.FileSystem
	[IO.Compression.ZipFile]::CreateFromDirectory($stage, $OutFile, [IO.Compression.CompressionLevel]::Optimal, $false)
}
finally {
	Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host "`n$OutFile ($($files.Count) files + fomod)"
$files.Keys | ForEach-Object { Write-Host "  $_" }
