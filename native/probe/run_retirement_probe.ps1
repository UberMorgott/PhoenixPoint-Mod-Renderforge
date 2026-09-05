param([string]$OutputRoot = 'D:\RenderforgeWork\retirement-proof\managed-probe')
$ErrorActionPreference='Stop'
$env:DOTNET_SKIP_FIRST_TIME_EXPERIENCE='1'; $env:DOTNET_CLI_TELEMETRY_OPTOUT='1'; $env:DOTNET_CLI_HOME='D:\RenderforgeWork\dotnet-home'
$env:TEMP='D:\RenderforgeWork\retirement-proof'; $env:TMP=$env:TEMP
$repo=Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
New-Item -ItemType Directory -Force $OutputRoot | Out-Null
# Compile unchanged production method bodies, omitting unrelated Unity rendering/diagnostic methods.
$source=Get-Content (Join-Path $repo 'src\DlssDriver.cs') -Raw
$methods=@('public static DlssDriver Create()', 'internal void RequestShutdown()', 'private void OnDestroy()', 'private void Update()', 'private void BeginRelease()', 'private bool TryRetire()', 'private void TeardownResources()')
$body='using System; using UnityEngine; namespace Renderforge { partial class DlssDriver {'
foreach($method in $methods) {
 $begin=$source.IndexOf($method); if($begin -lt 0){throw "Missing production method $method"}
 $brace=$source.IndexOf('{',$begin); $level=1; $end=$brace+1
 while($level -gt 0 -and $end -lt $source.Length){ if($source[$end] -eq '{'){$level++}; if($source[$end] -eq '}'){$level--};$end++ }
 if($level -ne 0){throw "Unbalanced production method $method"}
 $body+=$source.Substring($begin,$end-$begin)+"`n"
}
$body+='}}'; Set-Content (Join-Path $OutputRoot 'DriverMethods.cs') $body
$frame=Join-Path $repo 'src\FrameGen.cs'; $stub=Join-Path $PSScriptRoot 'retirement_managed_probe.cs'
@"
<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup><TargetFramework>net8.0</TargetFramework><OutputType>Exe</OutputType><EnableDefaultCompileItems>false</EnableDefaultCompileItems><NoWarn>CS0649;CS0169;CS0414</NoWarn></PropertyGroup><ItemGroup><Compile Include="$frame"/><Compile Include="$stub"/><Compile Include="DriverMethods.cs"/></ItemGroup></Project>
"@ | Set-Content (Join-Path $OutputRoot 'probe.csproj')
dotnet run --project (Join-Path $OutputRoot 'probe.csproj') -c Release --nologo
if($LASTEXITCODE -ne 0){throw "Managed retirement proof failed: $LASTEXITCODE"}
