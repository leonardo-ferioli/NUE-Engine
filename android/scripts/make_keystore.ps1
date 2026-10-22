# Creates a keystore for signing sideloaded release APKs and the keystore.properties read by sample/build.gradle.kts.
# Both files are git-ignored. Needs keytool (comes with the JDK). Run once and back up sideload.jks: if it is lost,
# players have to uninstall before they can update.
$root = Join-Path $PSScriptRoot ".."
$jks = Join-Path $root "sideload.jks"
if (Test-Path $jks) { Write-Host "sideload.jks already exists, not touching it"; exit 0 }
$bytes = New-Object byte[] 18
[System.Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($bytes)
$pw = [Convert]::ToBase64String($bytes).Replace("/", "x").Replace("+", "y")
keytool -genkeypair -v -keystore $jks -alias nue -keyalg RSA -keysize 4096 -validity 10000 -storepass $pw -keypass $pw -dname "CN=NUE sideload, O=NUE, C=ES"
if ($LASTEXITCODE -ne 0) { Write-Error "keytool failed"; exit 1 }
$props = "storeFile=sideload.jks`nstorePassword=$pw`nkeyAlias=nue`nkeyPassword=$pw`n"
Set-Content -Path (Join-Path $root "keystore.properties") -Value $props -Encoding ASCII -NoNewline
Write-Host "Created sideload.jks and keystore.properties (both git-ignored). Back them up."
