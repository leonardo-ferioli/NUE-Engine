# Headless emulator run of the NUE Android sample.
#
#   powershell -File android/scripts/run_emulator_tests.ps1 [-Abi x86_64|arm64-v8a] [-Install] [-Keep]
#
# What it does (nothing opens a window on the desktop: -no-window -no-audio):
#   1. checks hardware acceleration (WHPX / AEHD / HVF-style report of `emulator -accel-check`)
#   2. optionally installs the emulator + system image with sdkmanager (-Install)
#   3. creates the AVD "nue_test" if missing, boots it headless and waits for sys.boot_completed
#   4. installs the sample APK, starts it and runs scripted checks, each one verified in `adb logcat` (tag NUE):
#        capability report, lifecycle pause/resume, low-memory events, thermal events, battery saver, Game Mode,
#        audio focus + route facts, ADPF availability, rotation sensor, native import from a directory,
#        crash handler (SIGSEGV -> report converted at the next start)
#   5. writes logcat and a result table to android/emulator_results/ (git-ignored) and exits non-zero on any FAIL.
# Nothing here touches a real device: the script refuses to run if `adb devices` lists anything but the emulator it started.
param(
    [string]$Abi = "x86_64",
    [int]$Api = 36,
    [string]$ImageTag = "default",
    [string]$AvdName = "nue_test",
    [string]$Apk = "",
    [switch]$Install,
    [switch]$Keep,
    [int]$BootTimeoutSec = 600
)

$ErrorActionPreference = "Stop"
$root = Resolve-Path (Join-Path $PSScriptRoot "..")
$sdk = if ($env:ANDROID_HOME) { $env:ANDROID_HOME } elseif ($env:ANDROID_SDK_ROOT) { $env:ANDROID_SDK_ROOT } else { Join-Path $env:LOCALAPPDATA "Android\Sdk" }
if (-not $env:JAVA_HOME) {
    $jdk = Get-ChildItem "C:\Program Files\Eclipse Adoptium" -Directory -Filter "jdk-21*" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($jdk) { $env:JAVA_HOME = $jdk.FullName }
}
if ($env:JAVA_HOME) { $env:Path = "$env:JAVA_HOME\bin;$env:Path" }
$adb = Join-Path $sdk "platform-tools\adb.exe"
$emu = Join-Path $sdk "emulator\emulator.exe"
$sm = Join-Path $sdk "cmdline-tools\latest\bin\sdkmanager.bat"
$avdm = Join-Path $sdk "cmdline-tools\latest\bin\avdmanager.bat"
$image = "system-images;android-$Api;$ImageTag;$Abi"
$pkg = "org.nue.sample"
$activity = "$pkg/org.nue.engine.NueActivity"
$out = Join-Path $root "emulator_results"
New-Item -ItemType Directory -Force $out | Out-Null
if (-not $Apk) { $Apk = Join-Path $root "sample\build\outputs\apk\debug\sample-debug.apk" }

$results = New-Object System.Collections.Generic.List[object]
function Record($name, $ok, $detail) {
    $results.Add([pscustomobject]@{ Check = $name; Result = $(if ($ok) { "PASS" } else { "FAIL" }); Detail = $detail })
    Write-Host ("[{0}] {1}: {2}" -f $(if ($ok) { "PASS" } else { "FAIL" }), $name, $detail)
}
function Adb { param([Parameter(ValueFromRemainingArguments = $true)]$a) & $adb @a 2>&1 }
function Sh([string]$cmd) { (& $adb shell $cmd 2>&1 | Out-String).Trim() }
function Log { (& $adb logcat -d -s NUE:V AndroidRuntime:E DEBUG:F libc:F 2>&1 | Out-String) }
function WaitLog([string]$pattern, [int]$timeoutSec = 30, [string]$since = "") {
    $end = (Get-Date).AddSeconds($timeoutSec)
    do {
        $text = Log
        if ($since -and $text.Contains($since)) { $text = $text.Substring($text.LastIndexOf($since)) }
        if ($text -match $pattern) { return $true }
        Start-Sleep -Milliseconds 700
    } while ((Get-Date) -lt $end)
    return $false
}
function Marker([string]$name) { $m = "MARK-$name-$([guid]::NewGuid().ToString('N').Substring(0,6))"; Adb shell log -t NUE $m | Out-Null; return $m }

# ---------------------------------------------------------------------------------------------- 1. acceleration
Write-Host "== acceleration"
if (Test-Path $emu) {
    $accel = (& $emu -accel-check 2>&1 | Out-String).Trim()
    Write-Host $accel
    Set-Content (Join-Path $out "accel_check.txt") $accel
} else {
    Write-Host "emulator not installed (use -Install)"
}

# ---------------------------------------------------------------------------------------------- 2. install
if ($Install) {
    Write-Host "== sdkmanager: emulator + $image"
    ("y`n" * 20) | cmd /c "`"$sm`" --sdk_root=`"$sdk`" `"emulator`" `"$image`" 2>&1"
}
if (-not (Test-Path $emu)) { throw "emulator missing: run with -Install" }
if (-not (Test-Path $Apk)) { throw "APK missing: $Apk (gradle :sample:assembleDebug -Pnue.abis=$Abi)" }

# ---------------------------------------------------------------------------------------------- 3. AVD + boot
$avdList = (& $emu -list-avds 2>&1 | Out-String)
if ($avdList -notmatch "(?m)^$AvdName\s*$") {
    Write-Host "== creating AVD $AvdName"
    ("no`n") | cmd /c "`"$avdm`" create avd -n $AvdName -k `"$image`" --device pixel_6 --force 2>&1"
}
$existing = (& $adb devices 2>&1 | Out-String)
if ($existing -match "(?m)^(?!emulator-)\S+\s+device") {
    throw "A real device is attached; this script only runs on the emulator it starts. Unplug it or use ANDROID_SERIAL carefully."
}
Write-Host "== starting emulator (headless)"
$emuProc = Start-Process -FilePath $emu -ArgumentList @("-avd", $AvdName, "-no-window", "-no-audio", "-no-boot-anim", "-no-snapshot", "-no-metrics", "-gpu", "swiftshader_indirect", "-netdelay", "none", "-netspeed", "full") -PassThru -WindowStyle Hidden -RedirectStandardOutput (Join-Path $out "emulator_stdout.txt") -RedirectStandardError (Join-Path $out "emulator_stderr.txt")
try {
    Adb wait-for-device | Out-Null
    $deadline = (Get-Date).AddSeconds($BootTimeoutSec)
    $booted = $false
    while ((Get-Date) -lt $deadline) {
        if ((Sh "getprop sys.boot_completed") -eq "1") { $booted = $true; break }
        Start-Sleep -Seconds 3
    }
    Record "emulator boot" $booted ("sdk " + (Sh "getprop ro.build.version.sdk") + ", abi " + (Sh "getprop ro.product.cpu.abi") + ", page size " + (Sh "getconf PAGE_SIZE"))
    if (-not $booted) { throw "boot timed out" }
    Adb shell input keyevent KEYCODE_WAKEUP | Out-Null
    Adb shell wm dismiss-keyguard | Out-Null

    # ------------------------------------------------------------------------------------------ 4. install + run
    Adb logcat -c | Out-Null
    $inst = (Adb install -r -t $Apk | Out-String)
    Record "install sample" ($inst -match "Success") ($inst.Trim())
    Adb shell pm grant $pkg android.permission.POST_NOTIFICATIONS 2>$null | Out-Null

    # first start creates the app-specific directories
    Adb shell am start -n $activity | Out-Null
    $started = WaitLog "NUE Android bridge ready" 60
    Record "engine bridge loaded (libnue.so + JNI)" $started "log line 'NUE Android bridge ready'"
    Record "capability report" (WaitLog "NUE capability report" 60) "dump_capabilities printed"
    $log = Log
    Record "device info" ($log -match "device: .* sdk=\d+ abi=\S+ 64bit=1") (($log -split "`n" | Where-Object { $_ -match "NUE.*device:" } | Select-Object -First 1))
    Record "display info" ($log -match "display: \d+x\d+") (($log -split "`n" | Where-Object { $_ -match "NUE.*display: \d" } | Select-Object -First 1))
    Record "GPU probe" ($log -match "gpu probe done|gpu: ") (($log -split "`n" | Where-Object { $_ -match "gpu probe done" } | Select-Object -First 1))
    Record "audio facts" ($log -match "audio: native \d+ Hz") (($log -split "`n" | Where-Object { $_ -match "NUE.*audio: native" } | Select-Object -First 1))
    $adpfLine = ($log -split "`n" | Where-Object { $_ -match "power: thermal" } | Select-Object -First 1)
    Record "power facts (ADPF flag reported)" ($adpfLine -match "adpf=\d") $adpfLine
    Record "rotation vector query" ($log -match "AUTO rotation vector available") (($log -split "`n" | Where-Object { $_ -match "AUTO rotation" } | Select-Object -First 1))
    Record "native crash handler installed" ($log -match "crash handler installed") "libnue installs the signal handlers at start"

    # ---- lifecycle: HOME (pause+stop) then relaunch (start+resume)
    $m = Marker "lifecycle"
    Adb shell input keyevent KEYCODE_HOME | Out-Null
    $paused = WaitLog "lifecycle 4" 20 $m
    Start-Sleep -Seconds 2
    Adb shell am start -n $activity | Out-Null
    $resumed = WaitLog "lifecycle 3" 20 $m
    $txt = Log; $txt = $txt.Substring($txt.LastIndexOf($m))
    Record "lifecycle pause" $paused "NUE_ANDROID_LIFE_PAUSE (4) seen"
    Record "lifecycle resume" $resumed "NUE_ANDROID_LIFE_RESUME (3) seen"
    Record "surface released + save-state actions" (($txt -match "ACTION 3") -and ($txt -match "ACTION 9")) "RELEASE_SURFACE (3) and SAVE_STATE (9) actions emitted on background"

    # ---- low memory / trim
    $m = Marker "trim"
    Adb shell am send-trim-memory $pkg RUNNING_CRITICAL | Out-Null
    Record "onTrimMemory RUNNING_CRITICAL" (WaitLog "memory trim=15 pressure=3" 15 $m) "memory event level 15 -> pressure CRITICAL"
    Adb shell am send-trim-memory $pkg MODERATE | Out-Null
    Record "onTrimMemory MODERATE (cached)" (WaitLog "memory trim=60" 15 $m) "memory event level 60"

    # ---- thermal override (thermalservice)
    $m = Marker "thermal"
    Adb shell cmd thermalservice override-status 3 | Out-Null
    Record "thermal status SEVERE -> governor" (WaitLog "thermal level=2 status=3" 15 $m) "NUE_ANDROID_THERMAL_SERIOUS reported"
    Adb shell cmd thermalservice reset | Out-Null

    # ---- battery saver
    $m = Marker "battery_saver"
    Adb shell cmd power set-mode 1 | Out-Null
    Record "battery saver on" (WaitLog "battery saver 1" 15 $m) "event received"
    Adb shell cmd power set-mode 0 | Out-Null

    # ---- Game Mode (polled every 5 s)
    $m = Marker "game_mode"
    Adb shell cmd game mode performance $pkg | Out-Null
    Record "Game Mode performance" (WaitLog "game mode 2" 20 $m) "GameManager.getGameMode polled"
    Adb shell cmd game reset $pkg | Out-Null

    # ---- audio route (device query is part of the capability report) + focus is exercised by the system on call/other app: not forced

    # ---- native import from a directory (drop-folder path of the storage layer)
    Write-Host "== import test"
    $tmp = Join-Path $env:TEMP "nue_emu_src"
    Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force (Join-Path $tmp "GLOBAL") | Out-Null
    New-Item -ItemType Directory -Force (Join-Path $tmp "Cars\BMW M3") | Out-Null
    $rnd = New-Object byte[] 3000000; (New-Object Random 7).NextBytes($rnd)
    [IO.File]::WriteAllBytes((Join-Path $tmp "GLOBAL\GlobalA.lzc"), $rnd)
    [IO.File]::WriteAllBytes((Join-Path $tmp "Cars\BMW M3\geo.bin"), $rnd[0..9999])
    $extDir = "/sdcard/Android/data/$pkg/files"
    Adb shell am force-stop $pkg | Out-Null
    Adb shell mkdir -p "$extDir/import_src" | Out-Null
    Adb push $tmp/. "$extDir/import_src/" | Out-Null
    $rel = Sh "cd $extDir/import_src && find . -type f | sort"
    "import_dir $extDir/import_src`n" | Set-Content -NoNewline (Join-Path $out "auto_test.txt")
    Adb push (Join-Path $out "auto_test.txt") "$extDir/auto_test.txt" | Out-Null
    $m = Marker "import"
    Adb shell am start -n $activity | Out-Null
    $done = WaitLog "import finished: ok" 90 $m
    $txt = Log
    Record "import_dir copied + verified" $done (($txt -split "`n" | Where-Object { $_ -match "import finished" } | Select-Object -Last 1))
    $listing = (& $adb shell "run-as $pkg ls -R files/gamedata" 2>&1 | Out-String)
    Record "imported files present (lower-case names, marker)" (($listing -match "globala.lzc") -and ($listing -match "geo.bin") -and ($listing -match "complete.txt")) (($listing -replace "`r?`n", " | ").Trim())
    $sizeA = (& $adb shell "run-as $pkg wc -c files/gamedata/global/globala.lzc" 2>&1 | Out-String).Trim()
    Record "imported file size" ($sizeA -match "^3000000 ") $sizeA
    Adb shell "rm -f $extDir/auto_test.txt" | Out-Null

    # ---- crash handler: SIGSEGV from outside; report converted at the next start
    Write-Host "== crash handler"
    $apid = (Sh "pidof $pkg")
    $m = Marker "crash"
    Adb shell kill -SEGV $apid | Out-Null
    Start-Sleep -Seconds 4
    $gone = -not (Sh "pidof $pkg")
    Record "process died on SIGSEGV" $gone "pid $apid"
    $bin = (& $adb shell "run-as $pkg ls files/crash" 2>&1 | Out-String).Trim()
    Record "raw crash record written" ($bin -match "crash.bin") $bin
    Adb shell am start -n $activity | Out-Null
    Record "report converted at next start" (WaitLog "previous run crashed \(SIGSEGV\)" 60 $m) "stack.txt created, text logged line by line"
}
finally {
    $log = Log
    Set-Content (Join-Path $out "logcat_nue.txt") $log
    (& $adb logcat -d 2>&1) | Set-Content (Join-Path $out "logcat_full.txt")
    $results | Format-Table -AutoSize | Out-String | Set-Content (Join-Path $out "results.txt")
    if (-not $Keep) {
        Adb emu kill | Out-Null
        Start-Sleep -Seconds 3
        if ($emuProc -and -not $emuProc.HasExited) { Stop-Process -Id $emuProc.Id -Force -ErrorAction SilentlyContinue }
    }
}
Write-Host ""
$results | Format-Table -AutoSize
$failed = @($results | Where-Object { $_.Result -eq "FAIL" }).Count
Write-Host ("{0} checks, {1} failed. Logs: {2}" -f $results.Count, $failed, $out)
if ($failed -gt 0) { exit 1 }
