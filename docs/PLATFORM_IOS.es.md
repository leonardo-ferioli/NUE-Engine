# NUE en iOS: cómo compilarlo en un Mac (o en CI)

> **Aviso honesto.** Este código se escribió en Windows. Nadie lo ha compilado todavía con Xcode: la primera compilación en un Mac o en
> GitHub Actions es la primera vez que el código Objective-C++ se compila de verdad, y es normal que aparezcan errores de sintaxis o de
> nombres de API que hay que corregir. La lógica pura (ciclo de vida, políticas, rutas, importación, entrada) sí está compilada y probada en
> Windows. Detalles técnicos en [PLATFORM_IOS.md](PLATFORM_IOS.md) (en inglés).

## 0. Qué necesitas

* Un Mac con **macOS 13 o superior**, **Xcode 15 o superior** (con el SDK de iOS 17+), y CMake 3.21+ (`brew install cmake ninja`).
* Para probar en un iPhone/iPad real: un cable (o Wi-Fi pairing) y un **Apple ID**. Una cuenta gratuita sirve para desarrollo; la app caduca a
  los 7 días y hay que volver a instalarla. Con el programa de pago (99 USD/año) la firma dura un año.
* Los datos del juego (la carpeta de NFSU2, unos 1,2 GB) que tú mismo copias al dispositivo. **No están en el repositorio** y no se incluyen en la app.
* Alternativa sin Mac: GitHub Actions (sección 6).

## 1. Comprobar la lógica en el ordenador (sirve en cualquier sistema)

```bash
cd nue_engine/ios
bash scripts/run_host_tests.sh
```

Debe terminar con `100% tests passed`. En Windows, desde la raíz del repositorio:

```powershell
cmake -S nue_engine/ios -B build_ios_host -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang
cmake --build build_ios_host
ctest --test-dir build_ios_host --output-on-failure
```

## 2. Compilar la app de ejemplo para el simulador (sin firma)

```bash
cd nue_engine/ios
bash scripts/build_ios.sh simulator Debug
```

La primera vez CMake descarga SDL3 (`release-3.4.18`) desde GitHub. El resultado es `build_ios_sim/Debug-iphonesimulator/nue_ios_sample.app`.
Para ejecutarlo en el simulador:

```bash
xcrun simctl boot "iPhone 15" 2>/dev/null || true   # usa un nombre de `xcrun simctl list devices`
xcrun simctl install booted build_ios_sim/Debug-iphonesimulator/nue_ios_sample.app
xcrun simctl launch booted org.nue.sample
```

En el simulador `os_proc_available_memory` devuelve 0 y no hay giroscopio ni Game Mode: es normal.

## 3. Compilar para un iPhone/iPad real

1. Conecta el dispositivo, desbloquéalo y pulsa "Confiar". En iOS 16+ activa **Ajustes → Privacidad y seguridad → Modo desarrollador** y reinicia.
2. Averigua tu Team ID: Xcode → Settings → Accounts → tu Apple ID → "Personal Team" (o el equipo de pago); es un código de 10 caracteres.
3. Elige un identificador de bundle propio (con una cuenta gratuita el predeterminado `org.nue.sample` suele estar ya ocupado):

```bash
cd nue_engine/ios
NUE_IOS_TEAM_ID=ABCDE12345 NUE_IOS_BUNDLE_ID=com.tunombre.nuesample bash scripts/build_ios.sh device Release
```

4. Instala la app: abre `build_ios_device/nue_ios.xcodeproj` en Xcode, selecciona tu dispositivo y pulsa Run; o usa
   Xcode → Window → Devices and Simulators → arrastra el `.app`.
5. La primera vez, en el dispositivo: **Ajustes → General → VPN y gestión de dispositivos → tu Apple ID → Confiar**.

### Alternativa: proyecto con XcodeGen

```bash
brew install xcodegen
cd nue_engine/ios
xcodegen generate
open NueSample.xcodeproj
```

En Xcode: target `NueSample` → Signing & Capabilities → elige tu equipo y cambia el Bundle Identifier. Run.

## 4. Copiar los datos del juego al iPhone/iPad

La app expone su carpeta Documents (claves `UIFileSharingEnabled` y `LSSupportsOpeningDocumentsInPlace`).

* **Con Finder (Mac)**: conecta el dispositivo, selecciónalo en la barra lateral, pestaña **Archivos**, arrastra tu carpeta `NFSU2` debajo de "NUE Sample".
  Debe quedar `NFSU2/SPEED2.EXE` (no `NFSU2/NFSU2/SPEED2.EXE`; si te equivocas, el motor lo detecta y avisa).
* **Con la app Archivos (en el propio dispositivo)**: "En mi iPhone/iPad → NUE Sample", copia la carpeta desde iCloud Drive, un USB o un disco de red.
* Hacen falta unos 1,2 GB libres más un margen del 5 % (mínimo 64 MB).
* Importar desde el selector de documentos (copia dentro de la app, con progreso y comprobación CRC) está implementado en `nue_ios_pick_folder` /
  `nue_ios_import_folder`, pero la app de ejemplo todavía no tiene botón para ello.

## 5. Instalar como IPA (AltStore, Sideloadly)

```bash
cd nue_engine/ios
bash scripts/make_ipa.sh ../../build_ios_device/Release-iphoneos/nue_ios_sample.app nue_sample.ipa
```

El IPA sale **sin firmar**; AltStore o Sideloadly lo firman con tu Apple ID (con la cuenta gratuita caduca a los 7 días; AltStore lo renueva si el
ordenador está en la misma red). TestFlight/App Store no son una vía realista para un juego recompilado que necesita los archivos del jugador.

## 6. Compilar en GitHub Actions (sin Mac)

El workflow `.github/workflows/nue-ios.yml` se ejecuta en `macos-latest` cuando cambian `nue_engine/ios/**` o `nue_engine/src/platform/ios/**`, y
también a mano (Actions → nue-ios → Run workflow). Hace: tests del host, compilación para simulador y para dispositivo **sin firma**, el
xcframework y el proyecto XcodeGen. No usa secretos y no publica nada. Con "upload_ipa" marcado adjunta el IPA sin firmar a la ejecución (7 días,
solo visible para quien tenga acceso al repositorio). Si falla, el log de la ejecución contiene los errores de compilación que hay que arreglar.

## 7. Empaquetar el backend como xcframework (para integrarlo en otro proyecto)

```bash
cd nue_engine/ios
bash scripts/package_xcframework.sh ../../build_xcf
```

Genera `NueIos.xcframework` (dispositivo arm64 + simulador arm64) con la cabecera `nue_ios.h`.

## 8. Qué mirar en el dispositivo (checklist de verificación)

La app de ejemplo pinta un texto sobre un fondo de color animado. Comprueba y anota:

| Prueba | Resultado esperado |
|---|---|
| Arranca y se ve el fondo animado | sí; `state: active` |
| Pulsar Inicio y volver | `state` pasa por `background` y vuelve a `active`; el fondo sigue animado |
| Bajar el Centro de Control | `state: inactive` mientras está abierto |
| Línea `gpu:` | nombre del chip, `AppleFamily` 4–9, `BC:0`, `DXT->ASTC4x4` |
| Línea `memory:` | `avail` entre cientos de MB y ~2 GB; cambia si abres otras apps |
| Modo bajo consumo | `lpm:1` y `advised max fps: 30` |
| Conectar un mando Xbox/PlayStation | `pads: 1` y `last input: gamepad` |
| Inclinar el dispositivo en horizontal | `steering` cambia de signo al girar a derecha/izquierda; **anota si el signo es el contrario en alguno de los dos horizontales** |
| Tocar la pantalla | recalibra el volante (neutro) |
| Llamada o alarma | `audio interruptions` sube |
| Consola de macOS (Console.app, filtro `org.nue.engine`) | mensajes del motor |
| Game Mode | `game mode: plist=1 category=1 os18=1`; que el sistema lo active de verdad no se puede consultar |

Envíame (o apunta en `nue_engine/docs/REQUESTS_ios.md`) los errores de compilación y cualquier fila que no se cumpla.
