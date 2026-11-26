<#
.SYNOPSIS
Puts SeamlessSkyrim and the patched Community Shaders into a mod folder in Vortex's staging
folder, so the game is tested the way a player would install it.

.DESCRIPTION
The mod holds:
  SKSE\Plugins\SeamlessSkyrim.dll           Release build, without its PDB (what a player gets)
  SKSE\Plugins\SeamlessSkyrim.ini           package\
  SKSE\Plugins\SeamlessSkyrim_Doors.ini     package\, the doors tuned so far
  SKSE\Plugins\SeamlessSkyrim_Translations\*.json   package\, the menu's text
  SKSE\Plugins\CommunityShaders.dll         the fork's build (its PDB only with -CsPdb)
  every file the fork changed or added under package\ and features\*\ since -CsBase, at the
  place Community Shaders' own packaging puts it (Shaders\Lighting.hlsl, Shaders\Skylighting\...)

In Vortex the mod has to win its conflicts with Community Shaders and Skylighting ("load after").

Files are written into existing ones rather than replaced (tools\copy-in-place.ps1), so a
deployed mod's hard links stay intact and running the script again needs no redeploy unless
files were added or removed.

Door tunings made in game (the game's SeamlessSkyrim_Doors_User.ini) are moved into package\'s
SeamlessSkyrim_Doors.ini first (tools\fold-door-tunings.ps1; commit them there).

SeamlessSkyrim files that sit loose in the game's Data folder (the CMake post-build step put them
there before Vortex owned them) are removed, so Vortex doesn't deploy over them. An ini among them
that differs from the mod's is kept in build\loose-game-files\.

.EXAMPLE
tools\deploy-vortex-mod.ps1 -Build
Builds both DLLs, then stages the mod.

.EXAMPLE
tools\deploy-vortex-mod.ps1 -RestoreStockCommunityShaders
Also puts the stock 1.9.1 files back into Vortex's own Community Shaders mod, after manual
deploys (`cp` into the game's Data folder) wrote the fork's files through its hard links.
#>
#Requires -Version 7
[CmdletBinding()]
param(
	[string]$StagingRoot = 'E:\Vortex Mods\skyrimse',
	[string]$ModName = 'SeamlessSkyrim',
	[string]$GameData = 'E:\SteamLibrary\steamapps\common\Skyrim Special Edition\Data',
	[string]$CsFork = 'E:\projects\CommunityShaders-Seamless',
	# The Community Shaders release the fork is based on (a tag or commit in the fork).
	[string]$CsBase = 'v1.9.1',
	# Build SeamlessSkyrim (Release) and the fork's CommunityShaders target first.
	[switch]$Build,
	# Also ship the fork's PDB (~100 MB), for Crash Logger stack traces inside Community Shaders.
	# Without it the stock mod's PDB stays deployed; it doesn't match the fork's DLL, so crash
	# logs show raw addresses there, nothing worse.
	[switch]$CsPdb,
	[switch]$RestoreStockCommunityShaders,
	# Vortex's Community Shaders mod; found by name in -StagingRoot when not given.
	[string]$StockCsMod,
	# The stock CommunityShaders.dll and .pdb of -CsBase.
	[string]$StockCsBinaries = (Join-Path $CsFork 'original-1.9.1'),
	# Delete the game's compiled shaders. Needed after a change to an include (Common\*.hlsli):
	# Community Shaders only notices changes to a shader's own file.
	[switch]$ClearShaderCache
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$copyInPlace = Join-Path $PSScriptRoot 'copy-in-place.ps1'
$modDir = Join-Path $StagingRoot $ModName
# Files of a running game are locked, and a half-written mod is worse than none.
if (Get-Process SkyrimSE -ErrorAction SilentlyContinue) {
	throw 'Skyrim is running; quit it first.'
}
$scratch = Join-Path ([IO.Path]::GetTempPath()) "SeamlessSkyrim-vortex-$PID"
New-Item -ItemType Directory -Force $scratch | Out-Null
# Gone again when a step below throws, too.
trap { Remove-Item -LiteralPath $scratch -Recurse -Force -ErrorAction SilentlyContinue; break }

function Invoke-Git {
	$output = & git -C $CsFork @args
	if ($LASTEXITCODE) { throw "git $args failed" }
	$output
}

# git's output through PowerShell is decoded as text; shaders must arrive byte for byte.
function Save-GitBlob([string]$Spec, [string]$To) {
	$psi = [Diagnostics.ProcessStartInfo]::new('git')
	foreach ($a in '-C', $CsFork, 'cat-file', 'blob', $Spec) { $psi.ArgumentList.Add($a) }
	$psi.RedirectStandardOutput = $true
	$process = [Diagnostics.Process]::Start($psi)
	New-Item -ItemType Directory -Force (Split-Path $To) | Out-Null
	$file = [IO.File]::Create($To)
	try { $process.StandardOutput.BaseStream.CopyTo($file) } finally { $file.Dispose() }
	$process.WaitForExit()
	if ($process.ExitCode) { throw "git cat-file blob $Spec failed" }
}

function Copy-InPlace([string]$From, [string]$To) {
	@(& $copyInPlace -Destination $To $From)
}

# A stale DLL is the easiest way to test the wrong thing.
function Assert-Newer([string]$Binary, [string]$SourceDir) {
	if (-not (Test-Path -LiteralPath $Binary)) { throw "$Binary is missing; run with -Build." }
	$newest = Get-ChildItem -LiteralPath $SourceDir -Recurse -File | Sort-Object LastWriteTime | Select-Object -Last 1
	if ($newest.LastWriteTime -gt (Get-Item -LiteralPath $Binary).LastWriteTime) {
		Write-Warning "$Binary is older than $($newest.FullName); run with -Build to include the change."
	}
}

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
	& $cmake --build (Join-Path $CsFork 'build\ALL') --config Release --target CommunityShaders
	if ($LASTEXITCODE) { throw 'Community Shaders build failed.' }
}

# ---- what goes into the mod: relative path -> source file -----------------------------------
$plugins = 'SKSE\Plugins'
$files = [ordered]@{}

$dll = Join-Path $repo 'build\Release\SeamlessSkyrim.dll'
Assert-Newer $dll (Join-Path $repo 'src')
$files["$plugins\SeamlessSkyrim.dll"] = $dll

$files["$plugins\SeamlessSkyrim.ini"] = Join-Path $repo "package\$plugins\SeamlessSkyrim.ini"
foreach ($json in Get-ChildItem -LiteralPath (Join-Path $repo "package\$plugins\SeamlessSkyrim_Translations") -Filter *.json) {
	$files["$plugins\SeamlessSkyrim_Translations\$($json.Name)"] = $json.FullName
}

& (Join-Path $PSScriptRoot 'fold-door-tunings.ps1') -GameData $GameData
$files["$plugins\SeamlessSkyrim_Doors.ini"] = Join-Path $repo "package\$plugins\SeamlessSkyrim_Doors.ini"

$csBuild = Join-Path $CsFork 'build\ALL\Release'
Assert-Newer (Join-Path $csBuild 'CommunityShaders.dll') (Join-Path $CsFork 'src')
$files["$plugins\CommunityShaders.dll"] = Join-Path $csBuild 'CommunityShaders.dll'
if ($CsPdb) {
	$files["$plugins\CommunityShaders.pdb"] = Join-Path $csBuild 'CommunityShaders.pdb'
}

# Committed or not: what is being tested is the working tree.
$forkChanged = @(Invoke-Git diff --name-only --diff-filter=d $CsBase -- package features) +
	@(Invoke-Git ls-files --others --exclude-standard -- package features)
foreach ($path in $forkChanged | Sort-Object -Unique) {
	# Community Shaders' packaging copies package\ and each features\<name>\ into Data as they are.
	$relative = ($path -replace '^(package|features/[^/]+)/', '') -replace '/', '\'
	$files[$relative] = Join-Path $CsFork $path
}

# ---- stage -----------------------------------------------------------------------------------
if ((Test-Path -LiteralPath $modDir) -and -not (Test-Path -LiteralPath (Join-Path $modDir "$plugins\SeamlessSkyrim.dll"))) {
	# Stale files are deleted below; never do that to some other mod's folder.
	throw "$modDir exists but doesn't look like this mod (no $plugins\SeamlessSkyrim.dll); pick another -ModName."
}

$added = @($files.Keys | Where-Object { -not (Test-Path -LiteralPath (Join-Path $modDir $_)) })
$written = foreach ($relative in $files.Keys) {
	Copy-InPlace $files[$relative] (Join-Path $modDir $relative)
}

$removed = foreach ($file in Get-ChildItem -LiteralPath $modDir -Recurse -File) {
	$relative = [IO.Path]::GetRelativePath($modDir, $file.FullName)
	if (-not $files.Contains($relative)) {
		Remove-Item -LiteralPath $file.FullName
		$relative
	}
}
Get-ChildItem -LiteralPath $modDir -Recurse -Directory | Sort-Object { $_.FullName.Length } -Descending |
	Where-Object { -not (Get-ChildItem -LiteralPath $_.FullName -Force) } | Remove-Item

# ---- loose copies in the game's Data folder --------------------------------------------------
$looseRemoved = foreach ($relative in $files.Keys | Where-Object { $_ -like "$plugins\SeamlessSkyrim*" }) {
	$gameFile = Get-Item -LiteralPath (Join-Path $GameData $relative) -ErrorAction SilentlyContinue
	# A hard link is Vortex's deployment (of this mod or, before it wins, another one).
	if (-not $gameFile -or $gameFile.LinkType -eq 'HardLink') { continue }
	if ($gameFile.Extension -eq '.ini' -and
		(Get-FileHash -LiteralPath $gameFile.FullName).Hash -ne (Get-FileHash -LiteralPath $files[$relative]).Hash) {
		$keep = Join-Path $repo 'build\loose-game-files'
		New-Item -ItemType Directory -Force $keep | Out-Null
		Copy-Item -LiteralPath $gameFile.FullName -Destination $keep
	}
	Remove-Item -LiteralPath $gameFile.FullName
	$relative
}

# ---- Vortex's own Community Shaders mod back to stock ---------------------------------------
$restored = @()
if ($RestoreStockCommunityShaders) {
	if (-not $StockCsMod) {
		$found = @(Get-ChildItem -LiteralPath $StagingRoot -Directory -Filter 'Community Shaders *')
		if ($found.Count -ne 1) { throw "Found $($found.Count) 'Community Shaders *' mods in $StagingRoot; pass -StockCsMod." }
		$StockCsMod = $found[0].FullName
	}
	foreach ($name in 'CommunityShaders.dll', 'CommunityShaders.pdb') {
		$restored += Copy-InPlace (Join-Path $StockCsBinaries $name) (Join-Path $StockCsMod "$plugins\$name")
	}
	# Only files stock has (M); the features' files belong to their own Vortex mods (Skylighting...).
	foreach ($path in Invoke-Git diff --name-only --diff-filter=M $CsBase -- package) {
		$relative = ($path -replace '^package/', '') -replace '/', '\'
		$stock = Join-Path $scratch "stock\$relative"
		Save-GitBlob "${CsBase}:$path" $stock
		$restored += Copy-InPlace $stock (Join-Path $StockCsMod $relative)
	}
}

if ($ClearShaderCache) {
	Get-ChildItem -LiteralPath (Join-Path $GameData 'ShaderCache') -ErrorAction SilentlyContinue | Remove-Item -Recurse
}

Remove-Item -LiteralPath $scratch -Recurse -Force

# ---- report ----------------------------------------------------------------------------------
Write-Host "`nMod folder: $modDir ($($files.Count) files)"
if ($written) { Write-Host "Written:`n  $(($written | ForEach-Object { [IO.Path]::GetRelativePath($modDir, $_) }) -join "`n  ")" }
else { Write-Host 'Nothing changed in the mod.' }
if ($removed) { Write-Host "Removed from the mod:`n  $($removed -join "`n  ")" }
if ($looseRemoved) { Write-Host "Removed loose from the game's Data folder:`n  $($looseRemoved -join "`n  ")" }
if ($restored) { Write-Host "Restored to stock in $(Split-Path $StockCsMod -Leaf):`n  $(($restored | ForEach-Object { [IO.Path]::GetRelativePath($StockCsMod, $_) }) -join "`n  ")" }
if (-not $ClearShaderCache -and (@($written) + @($restored)) -match '\.hlsli$') {
	Write-Host 'An include changed: run again with -ClearShaderCache, or Community Shaders keeps the old shaders.' -ForegroundColor Yellow
}
if ($added.Count -eq $files.Count) {
	Write-Host "New mod: restart Vortex so it lists '$ModName', enable it, make it load after Community Shaders and Skylighting, and deploy." -ForegroundColor Yellow
}
elseif ($added -or $removed -or $looseRemoved -or $restored) {
	Write-Host 'Files were added or removed: deploy in Vortex before starting the game.' -ForegroundColor Yellow
}
