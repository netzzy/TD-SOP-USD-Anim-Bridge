param(
	[string]$TouchDesignerSamplesRoot = "",
	[string]$StatusJson = ""
)

$ErrorActionPreference = "Stop"

function Write-Status {
	param(
		[bool]$Ok,
		[string]$ErrorMessage = ""
	)
	if (-not $StatusJson) {
		return
	}
	$dir = Split-Path -Parent $StatusJson
	if ($dir) {
		New-Item -ItemType Directory -Force -Path $dir | Out-Null
	}
	@{
		ok = $Ok
		error = $ErrorMessage
	} | ConvertTo-Json | Set-Content -LiteralPath $StatusJson -Encoding UTF8
}

try {
	if (-not $IsWindows -and $env:OS -ne "Windows_NT") {
		throw "Native build currently supports Windows x64 only."
	}

	$root = Split-Path -Parent $PSScriptRoot
	$vsDevCmd = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat"
	if (-not (Test-Path -LiteralPath $vsDevCmd)) {
		throw "Visual Studio 2022 Build Tools not found: $vsDevCmd"
	}

	function Resolve-TDSamplesRoot {
		if ($TouchDesignerSamplesRoot) {
			return $TouchDesignerSamplesRoot
		}
		if ($env:TOUCHDESIGNER_SAMPLES) {
			return Join-Path $env:TOUCHDESIGNER_SAMPLES "CPlusPlus"
		}
		$default = Join-Path $env:ProgramFiles "Derivative\TouchDesigner\Samples\CPlusPlus"
		if (Test-Path -LiteralPath $default) {
			return $default
		}
		$derivative = Join-Path $env:ProgramFiles "Derivative"
		if (Test-Path -LiteralPath $derivative) {
			$found = Get-ChildItem -LiteralPath $derivative -Directory -Filter "TouchDesigner*" |
				Sort-Object LastWriteTime -Descending |
				ForEach-Object { Join-Path $_.FullName "Samples\CPlusPlus" } |
				Where-Object { Test-Path -LiteralPath $_ } |
				Select-Object -First 1
			if ($found) {
				return $found
			}
		}
		throw "TouchDesigner C++ sample headers not found. Set -TouchDesignerSamplesRoot or TOUCHDESIGNER_SAMPLES."
	}

	$tdSamplesRoot = Resolve-TDSamplesRoot
	$tdPopSamples = Join-Path $tdSamplesRoot "SimpleShapesPOP"
	if (-not (Test-Path -LiteralPath $tdPopSamples)) {
		throw "TouchDesigner C++ POP sample headers not found: $tdPopSamples"
	}
	$tdSopSamples = Join-Path $tdSamplesRoot "SimpleShapesSOP"
	if (-not (Test-Path -LiteralPath $tdSopSamples)) {
		throw "TouchDesigner C++ SOP sample headers not found: $tdSopSamples"
	}

	function Build-Plugin {
		param(
			[string]$Source,
			[string]$IncludePath,
			[string]$OutputDll,
			[string]$OutputObj
		)
		$out = Split-Path -Parent $OutputDll
		New-Item -ItemType Directory -Force -Path $out | Out-Null
		$cmd = "`"$vsDevCmd`" -arch=x64 -host_arch=x64 && cl /nologo /LD /EHsc /std:c++17 /O2 /MD /I `"$IncludePath`" `"$Source`" /Fe:`"$OutputDll`" /Fo:`"$OutputObj`""
		cmd.exe /c $cmd
		if ($LASTEXITCODE -ne 0) {
			throw "cl failed for $Source with exit code $LASTEXITCODE"
		}
	}

	Build-Plugin `
		-Source (Join-Path $PSScriptRoot "td_pop_usd_writer\TDPopUsdWriter.cpp") `
		-IncludePath $tdPopSamples `
		-OutputDll (Join-Path $PSScriptRoot "td_pop_usd_writer\build\TDPopUsdWriter.dll") `
		-OutputObj (Join-Path $PSScriptRoot "td_pop_usd_writer\build\TDPopUsdWriter.obj")

	Build-Plugin `
		-Source (Join-Path $PSScriptRoot "td_sop_usd_writer\TDSopUsdWriter.cpp") `
		-IncludePath $tdSopSamples `
		-OutputDll (Join-Path $PSScriptRoot "td_sop_usd_writer\build\TDSopUsdWriter.dll") `
		-OutputObj (Join-Path $PSScriptRoot "td_sop_usd_writer\build\TDSopUsdWriter.obj")

	Write-Status -Ok $true
	exit 0
}
catch {
	Write-Status -Ok $false -ErrorMessage $_.Exception.Message
	Write-Error $_.Exception.Message
	exit 1
}
