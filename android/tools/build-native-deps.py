#!/usr/bin/env python3
"""Build checksum-pinned, static PIC transport/codec dependencies with the Android NDK.

Requires Python 3.12+, CMake, Ninja, make, Perl, and a Linux x86_64 NDK.
No system installation; all outputs stay inside the Android project.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--ndk', type=Path, default=os.environ.get('ANDROID_NDK_HOME'))
parser.add_argument('--abi', action='append', choices=['arm64-v8a', 'x86_64'])
parser.add_argument('--jobs', type=int, default=min(os.cpu_count() or 4, 8))
args = parser.parse_args()
if args.ndk is None or not (args.ndk / 'build/cmake/android.toolchain.cmake').is_file():
    parser.error('Set ANDROID_NDK_HOME or pass --ndk /path/to/android-ndk-r28c')
ndk = args.ndk.resolve()
work = ROOT / '.native-build'
work.mkdir(exist_ok=True)
lock = json.loads((ROOT / 'tools/sources.json').read_text())
fingerprint = hashlib.sha256((ROOT / 'tools/sources.json').read_bytes()).hexdigest()
stamp = work / 'sources.lock'
if stamp.exists() and stamp.read_text() != fingerprint:
    raise SystemExit('Dependency versions changed; remove android/.native-build and android/native-deps, then rebuild')


def run(command, cwd, env=None):
    print('+', ' '.join(map(str, command)), flush=True)
    subprocess.run(list(map(str, command)), cwd=cwd, env=env, check=True)


for entry in lock:
    archive = work / (entry['name'] + '.tar.gz')
    if not archive.exists():
        temporary = archive.with_suffix('.download')
        with urllib.request.urlopen(entry['url'], timeout=90) as response, temporary.open('wb') as output:
            shutil.copyfileobj(response, output)
        temporary.rename(archive)
    with archive.open('rb') as input_file:
        if hashlib.file_digest(input_file, 'sha256').hexdigest() != entry['sha256']:
            raise SystemExit(f'Checksum mismatch: {archive}; remove it and retry')
    source = work / 'sources' / entry.get('path', entry['name'])
    if not source.exists() or not any(source.iterdir()):
        unpack = work / ('unpack-' + entry['name'])
        unpack.mkdir(exist_ok=True)
        with tarfile.open(archive) as tar:
            tar.extractall(unpack, filter='data')
        children = list(unpack.iterdir())
        if len(children) != 1 or not children[0].is_dir():
            raise SystemExit(f'Unexpected archive layout: {archive}')
        source.parent.mkdir(parents=True, exist_ok=True)
        if source.exists(): source.rmdir()
        children[0].rename(source)
        unpack.rmdir()
stamp.write_text(fingerprint)

for abi in args.abi or ['arm64-v8a', 'x86_64']:
    prefix = ROOT / 'native-deps' / abi
    prefix.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env['ANDROID_NDK_ROOT'] = str(ndk)
    env['PATH'] = str(ndk / 'toolchains/llvm/prebuilt/linux-x86_64/bin') + os.pathsep + env['PATH']
    env['PKG_CONFIG_LIBDIR'] = str(prefix / 'lib/pkgconfig')
    env.pop('PKG_CONFIG_PATH', None)
    build = work / abi
    build.mkdir(exist_ok=True)
    openssl = build / 'openssl'
    openssl.mkdir(exist_ok=True)
    if not (prefix / 'lib/libssl.a').exists():
        run(['perl', work / 'sources/openssl/Configure',
             'android-arm64' if abi == 'arm64-v8a' else 'android-x86_64',
             '-D__ANDROID_API__=26', '-fPIC', 'no-shared', 'no-tests', 'no-module',
             '--prefix=' + str(prefix), '--libdir=lib'], openssl, env)
        run(['make', '-j' + str(args.jobs)], openssl, env)
        run(['make', 'install_sw'], openssl, env)

    common = [
        '-G', 'Ninja', '-DCMAKE_TOOLCHAIN_FILE=' + str(ndk / 'build/cmake/android.toolchain.cmake'),
        '-DANDROID_ABI=' + abi, '-DANDROID_PLATFORM=android-26', '-DCMAKE_BUILD_TYPE=Release',
        '-DCMAKE_POLICY_VERSION_MINIMUM=3.5',
        # Bionic supplies pthreads in libc, but lacks pthread_cancel, which
        # upstream FindThreads uses in its generic libc detection probe.
        '-DCMAKE_HAVE_LIBC_PTHREAD=1',
        '-DCMAKE_C_FLAGS=-D_GNU_SOURCE',
        '-DCMAKE_INSTALL_PREFIX=' + str(prefix), '-DCMAKE_INSTALL_LIBDIR=lib',
        '-DCMAKE_PREFIX_PATH=' + str(prefix), '-DCMAKE_POSITION_INDEPENDENT_CODE=ON',
        '-DBUILD_SHARED_LIBS=OFF', '-DBUILD_STATIC_LIBS=ON', '-DBUILD_TESTING=OFF',
        '-DOPENSSL_ROOT_DIR=' + str(prefix), '-DOPENSSL_USE_STATIC_LIBS=TRUE',
        '-DOPENSSL_INCLUDE_DIR=' + str(prefix / 'include'),
        '-DOPENSSL_SSL_LIBRARY=' + str(prefix / 'lib/libssl.a'),
        '-DOPENSSL_CRYPTO_LIBRARY=' + str(prefix / 'lib/libcrypto.a'),
    ]
    projects = {
        'libevent': ['-DEVENT__LIBRARY_TYPE=STATIC', '-DEVENT__DISABLE_TESTS=ON',
                     '-DEVENT__DISABLE_REGRESS=ON', '-DEVENT__DISABLE_SAMPLES=ON', '-DEVENT__DISABLE_BENCHMARK=ON'],
        'nghttp2': ['-DENABLE_LIB_ONLY=ON'],
        'nghttp3': ['-DENABLE_LIB_ONLY=ON', '-DENABLE_STATIC_LIB=ON', '-DENABLE_SHARED_LIB=OFF'],
        'ngtcp2': ['-DENABLE_LIB_ONLY=ON', '-DENABLE_STATIC_LIB=ON', '-DENABLE_SHARED_LIB=OFF', '-DENABLE_OPENSSL=ON'],
        'jansson': ['-DJANSSON_BUILD_DOCS=OFF', '-DJANSSON_WITHOUT_TESTS=ON', '-DJANSSON_BUILD_SHARED_LIBS=OFF'],
        'opus': ['-DOPUS_BUILD_TESTING=OFF', '-DOPUS_BUILD_PROGRAMS=OFF', '-DOPUS_INSTALL_PKG_CONFIG_MODULE=ON'],
    }
    for name, options in projects.items():
        run(['cmake', '-S', work / 'sources' / name, '-B', build / name, *common, *options], ROOT, env)
        run(['cmake', '--build', build / name, '--parallel', args.jobs], ROOT, env)
        run(['cmake', '--install', build / name], ROOT, env)
    # Archive license texts alongside binaries; copied into the app's About notices.
    notices = ROOT / 'app/src/main/assets/third-party'
    notices.mkdir(parents=True, exist_ok=True)
    for entry in lock:
        source = work / 'sources' / entry.get('path', entry['name'])
        texts = [p for p in source.iterdir() if p.is_file() and
                 p.name.upper().split('.')[0] in ('LICENSE', 'COPYING', 'AUTHORS', 'NOTICE')]
        for text in texts: shutil.copyfile(text, notices / (entry['name'] + '-' + text.name))
    shutil.copyfile(ROOT.parent / 'docs/third-party.md', notices / 'xphone-transport-notice.md')
    print('Built', abi, 'in', prefix, flush=True)
