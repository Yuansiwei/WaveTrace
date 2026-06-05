# qt5_oneclick.ps1
# v3 fixed:
#   - no Remove-Item confirmation prompts
#   - no failure when Win32/ARM64/Debug/Release folders do not exist
#   - keeps exactly one bat + one ps1 workflow
#
# Put qt5_oneclick.bat and qt5_oneclick.ps1 in project root, then double-click qt5_oneclick.bat.

param(
    [string]$ProjectRoot = "",
    [string]$QtVersion = "5.15.2",
    [string]$QtArch = "win64_msvc2019_64",
    [string[]]$Archives = @("qtbase"),
    [string[]]$ExtraModules = @(),
    [switch]$NoDebugLibs,
    [switch]$NoCleanBuild,
    [switch]$ForceRedownload,
    [switch]$DeployRuntime
)

$ErrorActionPreference = "Stop"
$ConfirmPreference = "None"
$ProgressPreference = "SilentlyContinue"

function I($m){Write-Host "[INFO] $m" -ForegroundColor Cyan}
function O($m){Write-Host "[OK]   $m" -ForegroundColor Green}
function W($m){Write-Host "[WARN] $m" -ForegroundColor Yellow}
function B($m){Write-Host "[BAD]  $m" -ForegroundColor Red}

function Root($p){
    if($p -and (Test-Path -LiteralPath $p)){return (Resolve-Path -LiteralPath $p).Path}
    return (Get-Location).Path
}

function SafeRemove($p){
    if([string]::IsNullOrWhiteSpace($p)){return}
    if(Test-Path -LiteralPath $p){
        W "Deleting: $p"
        Remove-Item -LiteralPath $p -Recurse -Force -Confirm:$false -ErrorAction SilentlyContinue
    } else {
        I "Skip missing: $p"
    }
}

function GetPython(){
    try{
        & py -3 --version *> $null
        if($LASTEXITCODE -eq 0){return @{Exe="py"; Prefix=@("-3")}}
    }catch{}

    foreach($c in @("python","python3")){
        try{
            & $c --version *> $null
            if($LASTEXITCODE -eq 0){return @{Exe=$c; Prefix=@()}}
        }catch{}
    }
    return $null
}

function PyRun {
    param(
        [hashtable]$Py,
        [Parameter(ValueFromRemainingArguments=$true)]
        [string[]]$CmdArgs
    )

    $all = @()
    if($Py.ContainsKey("Prefix") -and $Py.Prefix){ $all += @($Py.Prefix) }
    if($CmdArgs){ $all += @($CmdArgs) }

    I ("RUN: " + $Py.Exe + " " + ($all -join " "))
    & $Py.Exe @all
    if($LASTEXITCODE -ne 0){throw "Python command failed, exit code=$LASTEXITCODE"}
}

function EnsurePython(){
    $p=GetPython
    if($p){return $p}

    W "Python not found. Trying winget install Python 3.11..."
    $winget = Get-Command winget -ErrorAction SilentlyContinue
    if(!$winget){
        throw "Python is missing and winget is not available. Install Python 3.10+ manually, then rerun."
    }

    & winget install -e --id Python.Python.3.11 --silent --accept-package-agreements --accept-source-agreements
    if($LASTEXITCODE -ne 0){throw "winget failed to install Python."}

    $env:Path = [System.Environment]::GetEnvironmentVariable("Path","Machine") + ";" + [System.Environment]::GetEnvironmentVariable("Path","User")
    $p=GetPython
    if(!$p){throw "Python installation finished but python command is still unavailable. Reopen terminal and rerun."}
    return $p
}

function EnsureAqt($py){
    PyRun $py -m pip --version
    PyRun $py -m pip install --user --upgrade aqtinstall py7zr
    try{
        PyRun $py -m aqt version
    }catch{
        W "aqt version check failed, but continuing."
    }
}

function KitNameFromArch($arch){
    if($arch.StartsWith("win64_")){return $arch.Substring(6)}
    if($arch.StartsWith("win32_")){return $arch.Substring(6)}
    return $arch
}

function IsQt($p){
    return $p -and
           (Test-Path -LiteralPath (Join-Path $p "bin\qmake.exe")) -and
           (Test-Path -LiteralPath (Join-Path $p "include\QtCore")) -and
           (Test-Path -LiteralPath (Join-Path $p "lib"))
}

function CpFile($s,$d){
    if(Test-Path -LiteralPath $s){
        New-Item -ItemType Directory -Force -Path $d | Out-Null
        Copy-Item -LiteralPath $s -Destination $d -Force
        I "Copied: $s"
    }
}

function CpGlob($pat,$d){
    $a=@(Get-ChildItem -Path $pat -File -ErrorAction SilentlyContinue)
    if($a.Count){
        New-Item -ItemType Directory -Force -Path $d | Out-Null
        foreach($x in $a){Copy-Item -LiteralPath $x.FullName -Destination $d -Force}
    }
}

function CpDir($s,$d){
    if(!(Test-Path -LiteralPath $s)){return}
    New-Item -ItemType Directory -Force -Path $d | Out-Null
    $cmd='robocopy "{0}" "{1}" /E /COPY:DAT /DCOPY:DAT /R:2 /W:1 /NFL /NDL /NP' -f $s,$d
    I $cmd
    cmd /c $cmd
    if($LASTEXITCODE -ge 8){throw "robocopy failed: $s"}
}

function Rel($from,$to){
    $f=[IO.Path]::GetFullPath($from); $t=[IO.Path]::GetFullPath($to)
    if(!$f.EndsWith([IO.Path]::DirectorySeparatorChar)){$f += [IO.Path]::DirectorySeparatorChar}
    $fu=New-Object Uri($f); $tu=New-Object Uri($t)
    return [Uri]::UnescapeDataString($fu.MakeRelativeUri($tu).ToString()).Replace('/','\')
}

function DownloadQt($py, $root, $version, $arch, $archives, $force){
    $downloadRoot = Join-Path $root "third_party\_qt_download"
    $kit = KitNameFromArch $arch
    $qtPath = Join-Path $downloadRoot "$version\$kit"

    if((Test-Path -LiteralPath $qtPath) -and (IsQt $qtPath) -and !$force){
        O "Downloaded Qt already exists: $qtPath"
        return $qtPath
    }

    if($force){SafeRemove $qtPath}
    New-Item -ItemType Directory -Force -Path $downloadRoot | Out-Null

    $cmdArgs = @("-m","aqt","install-qt","windows","desktop",$version,$arch,"-O",$downloadRoot)
    if($archives -and $archives.Count -gt 0){
        $cmdArgs += @("--archives")
        $cmdArgs += $archives
    }

    PyRun $py @cmdArgs

    if(!(IsQt $qtPath)){
        throw "Downloaded Qt path is invalid or incomplete: $qtPath"
    }

    O "Downloaded Qt: $qtPath"
    return $qtPath
}

function WriteQtConf($qt){
    $bin=Join-Path $qt "bin"
    New-Item -ItemType Directory -Force -Path $bin | Out-Null
@"
[Paths]
Prefix=..
Binaries=bin
Libraries=lib
Headers=include
Plugins=plugins
Data=.
ArchData=.
HostPrefix=..
HostBinaries=bin
"@ | Set-Content (Join-Path $bin "qt.conf") -Encoding ASCII
    O "Generated qt.conf"
}

function CopyQt5Local($src,$dst,$debug,$extra){
    $srcBin=Join-Path $src "bin"; $dstBin=Join-Path $dst "bin"
    $srcLib=Join-Path $src "lib"; $dstLib=Join-Path $dst "lib"
    $srcInc=Join-Path $src "include"; $dstInc=Join-Path $dst "include"

    $mods=@("Core","Gui","Widgets") + $extra
    $mods=@($mods|Where-Object{$_}|Sort-Object -Unique)
    I "Copy Qt modules: $($mods -join ', ')"

    foreach($m in $mods){
        CpDir (Join-Path $srcInc "Qt$m") (Join-Path $dstInc "Qt$m")
        CpFile (Join-Path $srcInc "Qt$m") $dstInc
        CpFile (Join-Path $srcLib "Qt5$m.lib") $dstLib
        CpFile (Join-Path $srcLib "Qt5$m.prl") $dstLib
        CpFile (Join-Path $srcBin "Qt5$m.dll") $dstBin
        if($debug){
            CpFile (Join-Path $srcLib "Qt5${m}d.lib") $dstLib
            CpFile (Join-Path $srcLib "Qt5${m}d.prl") $dstLib
            CpFile (Join-Path $srcBin "Qt5${m}d.dll") $dstBin
        }
    }

    CpFile (Join-Path $srcLib "qtmain.lib") $dstLib
    CpFile (Join-Path $srcLib "qtmain.prl") $dstLib
    if($debug){
        CpFile (Join-Path $srcLib "qtdmain.lib") $dstLib
        CpFile (Join-Path $srcLib "qtdmain.prl") $dstLib
    }

    CpDir (Join-Path $src "mkspecs") (Join-Path $dst "mkspecs")
    foreach($tool in @("moc.exe","uic.exe","rcc.exe","qtpaths.exe","windeployqt.exe","qmake.exe")){
        CpFile (Join-Path $srcBin $tool) $dstBin
    }

    foreach($pat in @("icudt*.dll","icuin*.dll","icuuc*.dll","pcre2*.dll","zlib*.dll","double-conversion*.dll","harfbuzz*.dll","freetype*.dll","png*.dll","brotli*.dll","bz2*.dll","zstd*.dll","lib*.dll","libEGL*.dll","libGLESv2*.dll","d3dcompiler*.dll","opengl32sw.dll")){
        CpGlob (Join-Path $srcBin $pat) $dstBin
    }

    $sp=Join-Path $src "plugins"; $dp=Join-Path $dst "plugins"
    foreach($pl in @("platforms","styles","imageformats","iconengines","platformthemes","tls","bearer")){
        CpDir (Join-Path $sp $pl) (Join-Path $dp $pl)
    }

    WriteQtConf $dst
}

function WriteQtLocalProps($file,$localQt,$debug){
    $base=Split-Path -Parent $file
    $r=Rel $base $localQt
    if(!$r.EndsWith("\")){$r+="\"}

    $dCore=""; $dGui=""; $dWid=""; $dEnt=""
    if($debug){
        $dCore="<QtCoreLib Condition=`"'`$(Configuration)'=='Debug' and Exists('`$(QtLocalLib)Qt5Cored.lib')`">Qt5Cored.lib</QtCoreLib>"
        $dGui="<QtGuiLib Condition=`"'`$(Configuration)'=='Debug' and Exists('`$(QtLocalLib)Qt5Guid.lib')`">Qt5Guid.lib</QtGuiLib>"
        $dWid="<QtWidgetsLib Condition=`"'`$(Configuration)'=='Debug' and Exists('`$(QtLocalLib)Qt5Widgetsd.lib')`">Qt5Widgetsd.lib</QtWidgetsLib>"
        $dEnt="<QtEntryPointLib Condition=`"'`$(Configuration)'=='Debug' and Exists('`$(QtLocalLib)qtdmain.lib')`">qtdmain.lib</QtEntryPointLib>"
    }

@"
<?xml version="1.0" encoding="utf-8"?>
<Project ToolsVersion="4.0" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <PropertyGroup Label="UserMacros">
    <QtLocalRoot>`$(MSBuildThisFileDirectory)$r</QtLocalRoot>
    <QtLocalBin>`$(QtLocalRoot)bin\</QtLocalBin>
    <QtLocalLib>`$(QtLocalRoot)lib\</QtLocalLib>
    <QtLocalInclude>`$(QtLocalRoot)include\</QtLocalInclude>
    <QtLocalPlugins>`$(QtLocalRoot)plugins\</QtLocalPlugins>
  </PropertyGroup>
  <PropertyGroup>
    <QtInstallDir>`$(QtLocalRoot)</QtInstallDir>
    <QtToolsPath>`$(QtLocalBin)</QtToolsPath>
    <QtDllPath>`$(QtLocalBin)</QtDllPath>
    <QTDIR>`$(QtLocalRoot)</QTDIR>
    <ExecutablePath>`$(QtLocalBin);`$(ExecutablePath)</ExecutablePath>
    <LibraryPath>`$(QtLocalLib);`$(LibraryPath)</LibraryPath>
    <IncludePath>`$(QtLocalInclude);`$(IncludePath)</IncludePath>
    <LocalDebuggerEnvironment>PATH=`$(QtLocalBin);%25PATH%25&#x0D;&#x0A;QT_PLUGIN_PATH=`$(QtLocalPlugins)</LocalDebuggerEnvironment>
  </PropertyGroup>
  <PropertyGroup>
    $dCore
    <QtCoreLib Condition="'`$(QtCoreLib)'==''">Qt5Core.lib</QtCoreLib>
    $dGui
    <QtGuiLib Condition="'`$(QtGuiLib)'==''">Qt5Gui.lib</QtGuiLib>
    $dWid
    <QtWidgetsLib Condition="'`$(QtWidgetsLib)'==''">Qt5Widgets.lib</QtWidgetsLib>
    $dEnt
    <QtEntryPointLib Condition="'`$(QtEntryPointLib)'==''">qtmain.lib</QtEntryPointLib>
  </PropertyGroup>
  <ItemDefinitionGroup>
    <ClCompile>
      <AdditionalIncludeDirectories>`$(QtLocalInclude);`$(QtLocalInclude)QtCore;`$(QtLocalInclude)QtGui;`$(QtLocalInclude)QtWidgets;%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>
      <PreprocessorDefinitions>QT_WIDGETS_LIB;QT_GUI_LIB;QT_CORE_LIB;%(PreprocessorDefinitions)</PreprocessorDefinitions>
    </ClCompile>
    <Link>
      <AdditionalLibraryDirectories>`$(QtLocalLib);%(AdditionalLibraryDirectories)</AdditionalLibraryDirectories>
      <AdditionalDependencies>`$(QtWidgetsLib);`$(QtGuiLib);`$(QtCoreLib);`$(QtEntryPointLib);%(AdditionalDependencies)</AdditionalDependencies>
    </Link>
  </ItemDefinitionGroup>
</Project>
"@ | Set-Content $file -Encoding UTF8
    O "Generated QtLocal.props"
}

function PatchVcx($proj,$props){
    $txt=Get-Content -LiteralPath $proj -Raw
    Copy-Item -LiteralPath $proj -Destination "$proj.bak_auto_qt5" -Force
    W "Backup: $proj.bak_auto_qt5"

    $txt=[regex]::Replace($txt,'(?m)^\s*<Import\s+Project="[^"]*QtLocal\.props"[^>]*/>\s*\r?\n?','')
    $rel=Rel (Split-Path -Parent $proj) $props
    $imp="  <Import Project=`"`$(MSBuildProjectDirectory)\$rel`" Condition=`"Exists('`$(MSBuildProjectDirectory)\$rel')`" />`r`n"

    $patterns=@(
        '(\s*<Import Project="props\\zstd_embed\.props" Condition="Exists\(''props\\zstd_embed\.props''\)" />\s*)',
        '(\s*<ImportGroup Condition="Exists\(''\$\(QtMsBuild\)\\qt_defaults\.props''\)">)',
        '(\s*<Import Project="\$\(QtMsBuild\)\\Qt\.props" />)',
        '(\s*<Import Project="\$\(VCTargetsPath\)\\Microsoft\.Cpp\.targets" />)'
    )

    $done=$false
    foreach($pat in $patterns){
        if(!$done -and $txt -match $pat){
            $txt=[regex]::Replace($txt,$pat,{param($m)$m.Groups[1].Value+$imp},1)
            $done=$true
        }
    }
    if(!$done){$txt=$txt -replace '</Project>\s*$',"$imp</Project>`r`n"}

    if($txt -match '<QtInstall>'){
        $txt=[regex]::Replace($txt,'<QtInstall>[^<]*</QtInstall>','<QtInstall>$(QtLocalRoot)</QtInstall>')
    }

    Set-Content -LiteralPath $proj -Value $txt -Encoding UTF8
    O "Patched: $proj"
}

function RewriteQt6ToQt5($root){
    $files=@(Get-ChildItem $root -Recurse -File -Include *.vcxproj,*.props,*.targets -ErrorAction SilentlyContinue | Where-Object {$_.FullName -notlike "*\third_party\*" -and $_.FullName -notlike "*\x64\*" -and $_.FullName -notlike "*\Win32\*" -and $_.Name -ne "QtLocal.props"})
    foreach($f in $files){
        $t=Get-Content -LiteralPath $f.FullName -Raw; $o=$t
        $t=$t -replace 'Qt6EntryPointd\.lib','qtdmain.lib'
        $t=$t -replace 'Qt6EntryPoint\.lib','qtmain.lib'
        $t=[regex]::Replace($t,'Qt6([A-Za-z0-9_]+?d?)\.lib','Qt5$1.lib')
        if($t -ne $o){
            Copy-Item -LiteralPath $f.FullName -Destination "$($f.FullName).bak_qt5libs" -Force
            Set-Content -LiteralPath $f.FullName -Value $t -Encoding UTF8
            O "Rewrote Qt6 libs to Qt5: $($f.FullName)"
        }
    }
}

function CleanBuild($root){
    foreach($p in @("x64","Win32","ARM64","Debug","Release")){
        SafeRemove (Join-Path $root $p)
    }
}

function DeployRuntime($root,$localQt){
    $exe=@("x64\Release\QtSingalViewer.exe","x64\Debug\QtSingalViewer.exe","Release\QtSingalViewer.exe","Debug\QtSingalViewer.exe") | ForEach-Object{Join-Path $root $_} | Where-Object{Test-Path -LiteralPath $_} | Select-Object -First 1
    if(!$exe){W "No exe found. Build first, then rerun with -DeployRuntime."; return}
    $dir=Split-Path -Parent $exe; $bin=Join-Path $localQt "bin"
    foreach($dll in @("Qt5Core.dll","Qt5Gui.dll","Qt5Widgets.dll")){CpFile (Join-Path $bin $dll) $dir}
    foreach($pat in @("icudt*.dll","icuin*.dll","icuuc*.dll","pcre2*.dll","zlib*.dll","double-conversion*.dll","harfbuzz*.dll","freetype*.dll","png*.dll","brotli*.dll","bz2*.dll","zstd*.dll","lib*.dll","libEGL*.dll","libGLESv2*.dll","d3dcompiler*.dll","opengl32sw.dll")){CpGlob (Join-Path $bin $pat) $dir}
    $sp=Join-Path $localQt "plugins"; $dp=Join-Path $dir "plugins"
    foreach($pl in @("platforms","styles","imageformats","iconengines","platformthemes","tls","bearer")){CpDir (Join-Path $sp $pl) (Join-Path $dp $pl)}
@"
[Paths]
Prefix=.
Binaries=.
Libraries=.
Plugins=plugins
"@ | Set-Content (Join-Path $dir "qt.conf") -Encoding ASCII
    O "Runtime deployed to: $dir"
}

try{
    $root=Root $ProjectRoot
    I "ProjectRoot = $root"

    $py=EnsurePython
    EnsureAqt $py

    $downloadedQt=DownloadQt $py $root $QtVersion $QtArch $Archives $ForceRedownload
    $kit=KitNameFromArch $QtArch
    $localQt=Join-Path $root "third_party\Qt\$QtVersion\$kit"

    SafeRemove (Join-Path $root "third_party\Qt")
    if(!$NoCleanBuild){CleanBuild $root}

    $debug = !$NoDebugLibs
    CopyQt5Local $downloadedQt $localQt $debug $ExtraModules

    $props=Join-Path $root "QtLocal.props"
    WriteQtLocalProps $props $localQt $debug

    $vcx=@(Get-ChildItem $root -Recurse -File -Filter *.vcxproj -ErrorAction SilentlyContinue | Where-Object {$_.FullName -notlike "*\third_party\*" -and $_.FullName -notlike "*\x64\*" -and $_.FullName -notlike "*\Win32\*" -and $_.FullName -notlike "*\ARM64\*"})
    foreach($v in $vcx){PatchVcx $v.FullName $props}
    RewriteQt6ToQt5 $root

    if($DeployRuntime){DeployRuntime $root $localQt}

    O "Qt5 auto download + localization finished."
    Write-Host ""
    Write-Host "Next:"
    Write-Host "  1) Close and reopen Visual Studio."
    Write-Host "  2) Build x64."
    Write-Host "  3) If Qt5 compile errors mention event->position()/globalPosition(), send the errors."
    Write-Host ""
    Write-Host "Local Qt:"
    Write-Host "  $localQt"
    exit 0
}catch{
    B $_.Exception.Message
    exit 1
}
