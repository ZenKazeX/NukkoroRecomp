"""Packages the Vulkan renderer's Android activity into a signed APK (docs/VULKAN_RENDERER.md, stage V5).

A NativeActivity APK has no Java code: the manifest (compiled by aapt2 against the platform's android.jar), the native
library under lib/<abi>/, aligned and signed. No Gradle.

    python package_apk.py --lib <libshadow_vk.so> --abi x86_64 --out <dir>
        [--sdk C:/AndroidSDK] [--build-tools 36.1.0] [--platform android-34] [--min-sdk 33] [--target-sdk 34]

The APK is signed with a key made for this build in <out>/test-signing.keystore (made on first use with the JDK's
keytool; the user's own debug keystore is not touched).
"""

import argparse
import os
import shutil
import subprocess
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))


def run(command):
    print(" ".join(command), flush=True)
    subprocess.run(command, check=True)


def tool(directory, name):
    for suffix in ("", ".exe", ".bat"):
        path = os.path.join(directory, name + suffix)
        if os.path.exists(path):
            return path
    sys.exit(f"{name} not found in {directory}")


def keytool():
    java_home = os.environ.get("JAVA_HOME")
    if java_home:
        path = os.path.join(java_home, "bin", "keytool.exe" if os.name == "nt" else "keytool")
        if os.path.exists(path):
            return path
    found = shutil.which("keytool")
    if not found:
        sys.exit("keytool not found (set JAVA_HOME)")
    return found


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--lib", required=True)
    parser.add_argument("--abi", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--sdk", default=os.environ.get("ANDROID_HOME", "C:/AndroidSDK"))
    parser.add_argument("--build-tools", default="36.1.0")
    parser.add_argument("--platform", default="android-34")
    parser.add_argument("--min-sdk", default="33")
    parser.add_argument("--target-sdk", default="34")
    args = parser.parse_args()

    build_tools = os.path.join(args.sdk, "build-tools", args.build_tools)
    android_jar = os.path.join(args.sdk, "platforms", args.platform, "android.jar")
    os.makedirs(args.out, exist_ok=True)
    unsigned = os.path.join(args.out, "unaligned.apk")
    aligned = os.path.join(args.out, "aligned.apk")
    signed = os.path.join(args.out, "shadow_vk.apk")

    run([tool(build_tools, "aapt2"), "link", "-o", unsigned, "--manifest", os.path.join(HERE, "AndroidManifest.xml"),
         "-I", android_jar, "--min-sdk-version", args.min_sdk, "--target-sdk-version", args.target_sdk])
    with zipfile.ZipFile(unsigned, "a", zipfile.ZIP_DEFLATED) as apk:
        apk.write(args.lib, f"lib/{args.abi}/{os.path.basename(args.lib)}")
    run([tool(build_tools, "zipalign"), "-p", "-f", "4", unsigned, aligned])

    keystore = os.path.join(args.out, "test-signing.keystore")
    if not os.path.exists(keystore):
        # A throw-away key for this test build only (the password is a fixed test value, not a secret).
        run([keytool(), "-genkeypair", "-keystore", keystore, "-storepass", "test-signing", "-keypass", "test-signing",
             "-alias", "test", "-keyalg", "RSA", "-keysize", "2048", "-validity", "3650",
             "-dname", "CN=Shadow Vulkan test build"])
    run([tool(build_tools, "apksigner"), "sign", "--ks", keystore, "--ks-pass", "pass:test-signing", "--key-pass",
         "pass:test-signing", "--ks-key-alias", "test", "--out", signed, aligned])
    run([tool(build_tools, "apksigner"), "verify", signed])
    os.remove(unsigned)
    os.remove(aligned)
    print(f"APK: {signed}")


if __name__ == "__main__":
    main()
