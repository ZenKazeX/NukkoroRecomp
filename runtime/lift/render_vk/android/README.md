# render_vk on Android (stage V5)

A NativeActivity (no Java, `main.cpp`) that drives `render_vk::Renderer` with FramePackets - the packet contract of
`render/frame.h`, the same the game hands the D3D12 renderer - on the `ANativeWindow`, through a pre-rotated
swapchain. On the first surface it logs the GPU's capabilities and runs the headless checks
(`tests/smoke_checks.h`: off-screen split-screen packets, read back, compared, rendered twice to the same bytes); then
it presents a split-screen packet every frame. The packets are made by the activity until the game itself runs on
Android.

Never run the build or the emulator while a `ShadowTheHedgehog*` process is running (the user playing).

## Build (from `source/`)

```
cmake -S runtime/lift/render_vk -B .build/vk-android-x86_64 -G Ninja ^
  -DCMAKE_MAKE_PROGRAM=C:/AndroidSDK/cmake/3.22.1/bin/ninja.exe ^
  -DCMAKE_TOOLCHAIN_FILE=C:/AndroidSDK/ndk/27.2.12479018/build/cmake/android.toolchain.cmake ^
  -DANDROID_ABI=x86_64 -DANDROID_PLATFORM=android-33 -DCMAKE_BUILD_TYPE=Release
cmake --build .build/vk-android-x86_64 -j 2
```

`x86_64` for the emulator on this PC, `arm64-v8a` for phones (`.build/vk-android-arm64-v8a`). Run the configure from
`cmd` (PowerShell turns the NDK toolchain's deprecation warnings on stderr into errors).

## Package (no Gradle)

```
set JAVA_HOME=G:\Program Files\Android\Android Studio\jbr
python runtime/lift/render_vk/android/package_apk.py --lib .build/vk-android-x86_64/libshadow_vk.so --abi x86_64 --out .build/vk-android-x86_64/apk
```

aapt2 links the manifest against android-34's `android.jar`, the library goes under `lib/<abi>/`, zipalign, then
apksigner with a throw-away key made in the output folder.

## Run

Emulator device: `shadow_vk_api36` (Pixel 7 profile, Android 36.1 x86_64 Google Play image, `hw.gpu.mode=host`,
defined in `C:\AndroidSDK\.android\avd`).

```
C:\AndroidSDK\emulator\emulator.exe -avd shadow_vk_api36 -no-snapshot-save -gpu host
adb install -r .build/vk-android-x86_64/apk/shadow_vk.apk
adb shell am start -n com.shadow.vk/android.app.NativeActivity
adb logcat -s shadow_vk
```

Replay mode (real game frames): push a recorded packet stream (render/packet_stream.h) before starting the app -

```
adb shell mkdir -p /sdcard/Android/data/com.shadow.vk/files
adb push .build/vk-replay/westopolis/packets.stream /sdcard/Android/data/com.shadow.vk/files/packets.stream
```

- and the activity presents its frames in a loop instead of the split-screen packets (`REPLAY LOOP <n>: 91 frames,
0 resource failures` for the Westopolis stream; 2026-10-27 on `shadow_vk_api36`, about 2.5 s per loop). Each frame
stays up for its tick distance to the previous one (at most 4 vsyncs). `adb shell rm -r
/sdcard/Android/data/com.shadow.vk/files` returns to the split-screen mode.

Pass: `CHECKS PASS` (off-screen checks) and `PRESENT PASS: 300 frames ...`. Failure: `CHECKS FAIL: <n>` or
`FAIL: <what>` (a missing device feature is named).
The emulator's Vulkan is the host GPU through gfxstream: right for bring-up and lifecycle, not evidence of a phone's
formats, features or speed.
