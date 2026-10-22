#!/bin/sh
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
build="${XPHONE_ANDROID_TEST_BUILD:-$repo/build-android-jni}"
cmake -S "$repo/android/app/src/main/cpp" -B "$build" -DCMAKE_BUILD_TYPE=Debug
cmake --build "$build" --parallel
mkdir -p "$build/classes"
javac --release 17 -d "$build/classes" \
    "$repo/android/app/src/main/java/org/xphone/client/NativeBridge.java" \
    "$repo/android/app/src/main/java/org/xphone/client/MediaPacket.java" \
    "$repo/android/app/src/main/java/org/xphone/client/JitterBuffer.java" \
    "$repo/android/tests/CoreTest.java" "$repo/android/tests/BridgeHarness.java"
java -Djava.library.path="$build" -cp "$build/classes" org.xphone.client.CoreTest
python3 "$repo/android/tests/interop.py" --build "$build" \
    --server "${XPHONE_TEST_SERVER:-$repo/build-release/xphoned}" \
    --client "${XPHONE_TEST_CLIENT:-$repo/build-release/xphone}"
