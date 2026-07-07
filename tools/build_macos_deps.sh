#!/bin/sh
# Build the source-only KeeperFX dependencies as arm64 static libraries for a
# native Apple Silicon macOS build. These libraries have no Homebrew formula, so
# we compile them from upstream source into deps/ where macos.mk expects them.
#
#   deps/astronomy/{include/astronomy.h, libastronomy.a}
#   deps/centijson/{include/*.h, libjson.a}
#   deps/enet6/{include/enet6/*.h, libenet6.a}
#
# The Homebrew-provided deps (SDL2, ffmpeg, luajit, openal-soft, libspng,
# minizip, miniupnpc, libnatpmp, zlib, curl) are resolved via pkg-config.
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$(mktemp -d)"
CFLAGS="-O3 -arch arm64"
echo "Working in $WORK"

# Pin each source-only dep to a known-good commit so the static libs are
# reproducible and an upstream file rename/API change can't silently break the
# fixed compile lists below. Bump these deliberately, not implicitly.
CENTIJSON_REF=93395382de7ea59f7348759b78d5b2044370fcce
ASTRONOMY_REF=865d3da7d8112bbc7911238052c6af4aaf877181
ENET6_REF=319fef490af064658dce661b94f3650a3ef95dbd

# clone_pinned <url> <commit-sha> <dest> — fetch exactly one commit (GitHub
# allows fetching an arbitrary reachable SHA), so the checkout is deterministic.
clone_pinned() {
    mkdir -p "$3"
    ( cd "$3" \
      && git init -q \
      && git remote add origin "$1" \
      && git fetch -q --depth 1 origin "$2" \
      && git checkout -q FETCH_HEAD )
}

echo "== centijson =="
clone_pinned https://github.com/mity/centijson.git "$CENTIJSON_REF" "$WORK/centijson"
mkdir -p "$ROOT/deps/centijson/include"
cp "$WORK"/centijson/src/*.h "$ROOT/deps/centijson/include/"
( cd "$WORK/centijson/src" && clang $CFLAGS -c json.c json-dom.c json-ptr.c value.c )
ar rcs "$ROOT/deps/centijson/libjson.a" "$WORK"/centijson/src/*.o

echo "== astronomy =="
clone_pinned https://github.com/cosinekitty/astronomy.git "$ASTRONOMY_REF" "$WORK/astronomy"
mkdir -p "$ROOT/deps/astronomy/include"
cp "$WORK/astronomy/source/c/astronomy.h" "$ROOT/deps/astronomy/include/"
( cd "$WORK/astronomy/source/c" && clang $CFLAGS -c astronomy.c )
ar rcs "$ROOT/deps/astronomy/libastronomy.a" "$WORK"/astronomy/source/c/astronomy.o

echo "== enet6 =="
clone_pinned https://github.com/SirLynix/enet6.git "$ENET6_REF" "$WORK/enet6"
mkdir -p "$ROOT/deps/enet6/include/enet6"
cp -r "$WORK"/enet6/include/enet6/* "$ROOT/deps/enet6/include/enet6/"
( cd "$WORK/enet6/src" && clang $CFLAGS -DHAS_SOCKLEN_T=1 -I"$WORK/enet6/include" \
    -c address.c callbacks.c compress.c host.c list.c packet.c peer.c protocol.c unix.c )
ar rcs "$ROOT/deps/enet6/libenet6.a" "$WORK"/enet6/src/*.o

rm -rf "$WORK"
echo "Done. Built deps/{astronomy,centijson,enet6} for arm64."
