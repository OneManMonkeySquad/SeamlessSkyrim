<#
.SYNOPSIS
Copies files by overwriting the destination's contents rather than replacing the destination file.

.DESCRIPTION
Vortex deploys a mod as hard links between its staging folder and the game's Data folder.
Copy-Item, `cmake -E copy` and CopyFile all put a new file in place of the destination, which cuts
that link: the game and the Vortex mod then hold different builds, and Vortex reports external
changes on its next deployment. Writing into the existing file updates both sides of the link.

Files whose contents already match are left alone, so their modification time stays put:
Community Shaders recompiles a shader when its file's time changes.

Prints the path of every file it wrote. Runs on Windows PowerShell 5.1 as well (the CMake
post-build step uses it).

.EXAMPLE
copy-in-place.ps1 -Destination <game>\Data\SKSE\Plugins build\RelWithDebInfo\SeamlessSkyrim.dll
#>
param(
	# A folder, or the destination file itself when there is a single source and no such folder.
	[Parameter(Mandatory = $true)][string]$Destination,
	[Parameter(Mandatory = $true, ValueFromRemainingArguments = $true)][string[]]$Source
)
$ErrorActionPreference = 'Stop'

# Not Get-FileHash: Windows PowerShell started from PowerShell 7 inherits its module path and
# can't load the cmdlet.
function Get-Hash([string]$Path) {
	$stream = [IO.File]::OpenRead($Path)
	try { [BitConverter]::ToString([Security.Cryptography.SHA256]::Create().ComputeHash($stream)) }
	finally { $stream.Dispose() }
}

$Destination = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Destination)
$toFolder = (Test-Path -LiteralPath $Destination -PathType Container) -or $Source.Count -gt 1

foreach ($item in $Source) {
	$from = (Resolve-Path -LiteralPath $item).ProviderPath
	$to = if ($toFolder) { Join-Path $Destination (Split-Path $from -Leaf) } else { $Destination }

	if ((Test-Path -LiteralPath $to) -and
		(Get-Item -LiteralPath $to).Length -eq (Get-Item -LiteralPath $from).Length -and
		(Get-Hash $to) -eq (Get-Hash $from)) {
		continue
	}

	New-Item -ItemType Directory -Force -Path (Split-Path $to) | Out-Null
	$in = [IO.File]::OpenRead($from)
	try {
		# FileMode.Create truncates an existing file instead of replacing it: hard links survive.
		$out = [IO.File]::Open($to, [IO.FileMode]::Create, [IO.FileAccess]::Write, [IO.FileShare]::Read)
		try { $in.CopyTo($out) } finally { $out.Dispose() }
	}
	finally { $in.Dispose() }
	Write-Output $to
}
