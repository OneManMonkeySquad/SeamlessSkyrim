<#
.SYNOPSIS
Builds SeamlessSkyrim and puts it into the game; optionally the Community Shaders fork too.

.DESCRIPTION
The one way to build from a shell: cmake isn't on PATH on this machine (it comes with Visual
Studio), and the post-build copy into the game fails while the game runs, with an error that
reads like a build failure.

Prints compiler warnings and errors only, then one line saying what was built and whether it
reached the game.

Runs on Windows PowerShell 5.1 as well.

.EXAMPLE
tools\build.ps1                      # RelWithDebInfo, copied into the game
.EXAMPLE
tools\build.ps1 -NoDeploy            # does it compile? Safe while the game runs
.EXAMPLE
tools\build.ps1 -CommunityShaders    # the fork's DLL too, copied into the game
#>
[CmdletBinding()]
param(
	# RelWithDebInfo is the day-to-day build: its PDB gives Crash Logger function names and lines.
	# Release is what the Vortex mod and the release archive ship.
	[ValidateSet('RelWithDebInfo', 'Release', 'Debug')][string]$Config = 'RelWithDebInfo',
	# Compile and link only; the game's files stay as they are.
	[switch]$NoDeploy,
	# Also build the Community Shaders fork (Release, about 1.5 minutes).
	[switch]$CommunityShaders,
	[string]$CsFork = 'E:\projects\CommunityShaders-Seamless',
	[string]$GamePlugins = 'E:\SteamLibrary\steamapps\common\Skyrim Special Edition\Data\SKSE\Plugins'
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent

if (-not $NoDeploy -and (Get-Process SkyrimSE -ErrorAction SilentlyContinue)) {
	throw 'Skyrim is running and holds the DLL: quit it first (never a game the user is playing), or pass -NoDeploy to only compile.'
}

$cmake = Get-Command cmake -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty Source
if (-not $cmake) {
	$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
	if (Test-Path -LiteralPath $vswhere) {
		$cmake = & $vswhere -latest -prerelease -find 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' | Select-Object -First 1
	}
}
if (-not $cmake) { throw 'cmake not found (neither on PATH nor in Visual Studio).' }

$msbuild = @('-nologo')
# The copy into the game is the project's post-build event; this turns it off for one build
# without reconfiguring.
$noCopy = @('-p:PostBuildEventUseInBuild=false')

$pluginArgs = $msbuild
if ($NoDeploy) { $pluginArgs += $noCopy }
& $cmake --build (Join-Path $repo 'build') --config $Config -- @pluginArgs
if ($LASTEXITCODE) { throw 'SeamlessSkyrim build failed.' }
$dll = Join-Path $repo "build\$Config\SeamlessSkyrim.dll"
# Where the project's post-build step copies it is CMake's SKYRIM_PLUGINS_DIR, not -GamePlugins
# (that one is the Community Shaders copy's): named here, the message pointed at the wrong folder.
$where = if ($NoDeploy) { 'not copied into the game' } else { 'copied into the game (SKYRIM_PLUGINS_DIR)' }
Write-Output "SeamlessSkyrim ($Config) built: $dll, $where."

if ($CommunityShaders) {
	& $cmake --build (Join-Path $CsFork 'build\ALL') --config Release --target CommunityShaders -- @msbuild
	if ($LASTEXITCODE) { throw 'Community Shaders build failed.' }
	$built = Join-Path $CsFork 'build\ALL\Release'
	if ($NoDeploy) {
		Write-Output "Community Shaders fork built: $built\CommunityShaders.dll, not copied into the game."
	}
	else {
		# In place, as the plugin's own post-build step does: the game's DLL is a Vortex hard link.
		$written = & (Join-Path $PSScriptRoot 'copy-in-place.ps1') -Destination $GamePlugins `
			(Join-Path $built 'CommunityShaders.dll') (Join-Path $built 'CommunityShaders.pdb')
		$state = if ($written) { "copied into $GamePlugins" } else { 'the game already had this build' }
		Write-Output "Community Shaders fork built: $state."
	}
}
