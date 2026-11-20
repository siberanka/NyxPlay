#!/bin/sh
# Test suite of the shared moonlight-common-c copy.
#
#   run-tests.sh [--san] [--keep] [name ...]
#
#   --san   build everything with AddressSanitizer and UndefinedBehaviorSanitizer
#   --keep  keep the build directory
#   name    run only these tests (default: all)
#
# Environment: CC, CXX (default gcc and g++), OUT (build directory), JOBS.
# Works with MSYS2 gcc (Windows) and with gcc on Linux. The MSVC pass is msvc-check.cmd.

set -u

HERE=$(cd "$(dirname "$0")" && pwd)
COMMON=$(cd "$HERE/.." && pwd)
LIB=$COMMON/moonlight-common-c
HOSTDIR=$COMMON/../host
CC=${CC:-gcc}
CXX=${CXX:-g++}
OUT=${OUT:-${TMPDIR:-/tmp}/mcc-tests}
SAN=""
KEEP=0
ONLY=""

for a in "$@"; do
    case "$a" in
        --san) SAN="-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer" ;;
        --keep) KEEP=1 ;;
        *) ONLY="$ONLY $a" ;;
    esac
done

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        WIN=1
        PLATLIBS="-lws2_32 -lwinmm -lcrypto"
        ENETSYS=win32
        ENETDEFS="-DHAS_QOS_FLOWID=1 -DHAS_PQOS_FLOWID=1"
        ;;
    *)
        WIN=0
        PLATLIBS="-lcrypto -lpthread -lm"
        ENETSYS=unix
        ENETDEFS="-DHAS_FCNTL=1 -DHAS_POLL=1 -DHAS_GETADDRINFO=1 -DHAS_GETNAMEINFO=1 -DHAS_INET_PTON=1 -DHAS_INET_NTOP=1 -DHAS_MSGHDR_FLAGS=1"
        ;;
esac

rm -rf "$OUT"
mkdir -p "$OUT/obj" "$OUT/bin" "$OUT/log" "$OUT/tmp"
# compilers write their temporary files here, not into the system temp directory
TMPDIR="$OUT/tmp"; TMP="$OUT/tmp"; TEMP="$OUT/tmp"; export TMPDIR TMP TEMP
FAILED=0
SUMMARY="$OUT/summary.txt"
: > "$SUMMARY"

note() { echo "$*" | tee -a "$SUMMARY"; }

# ---------------------------------------------------------------- 1. build gate
# The library compiles warning free with -Wall -Wextra -Werror. The only exception is the
# upstream GetProcAddress function pointer cast in Platform.c (Windows only).
LIBFLAGS="-std=gnu11 -g -O1 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-cast-function-type -DNDEBUG -DHAS_SOCKLEN_T=1 $SAN"
INC="-I$LIB/src -I$LIB/enet/include -I$LIB/nanors -I$LIB/nanors/deps -I$LIB/nanors/deps/obl"

OBJS=""
for f in "$LIB"/src/*.c; do
    n=$(basename "$f" .c)
    if ! $CC $LIBFLAGS $INC -c "$f" -o "$OUT/obj/lib_$n.o" > "$OUT/log/build_$n.txt" 2>&1; then
        note "BUILD-GATE FAIL $n"
        cat "$OUT/log/build_$n.txt"
        FAILED=1
    fi
    OBJS="$OBJS $OUT/obj/lib_$n.o"
done
note "build gate: $(ls "$LIB"/src/*.c | wc -l) library sources with -Wall -Wextra -Werror ${SAN:+(sanitizers)}"

# Third party code is built as it is, without -Werror
for f in "$LIB"/nanors/rs.c "$LIB"/nanors/deps/obl/oblas_common.c "$LIB"/nanors/deps/obl/oblas_lite.c; do
    n=$(basename "$f" .c)
    $CC -std=gnu11 -g -O1 -w -DNDEBUG $SAN $INC -c "$f" -o "$OUT/obj/nanors_$n.o" > "$OUT/log/build_nanors_$n.txt" 2>&1 || { note "BUILD FAIL nanors $n"; cat "$OUT/log/build_nanors_$n.txt"; FAILED=1; }
    OBJS="$OBJS $OUT/obj/nanors_$n.o"
done
for n in callbacks compress host list packet peer protocol $ENETSYS; do
    $CC -std=gnu11 -g -O1 -w $ENETDEFS $SAN -I"$LIB/enet/include" -c "$LIB/enet/$n.c" -o "$OUT/obj/enet_$n.o" > "$OUT/log/build_enet_$n.txt" 2>&1 || { note "BUILD FAIL enet $n"; cat "$OUT/log/build_enet_$n.txt"; FAILED=1; }
    OBJS="$OBJS $OUT/obj/enet_$n.o"
done

if [ "$FAILED" != 0 ]; then
    note "RESULT build failed"
    exit 1
fi

# ---------------------------------------------------------------- 2. tests
TESTFLAGS="-std=gnu11 -g -O1 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -DNDEBUG -DHAS_SOCKLEN_T=1 $SAN"

# Tests that #include a library .c file to reach its static functions must not link that
# object file a second time.
# name:excluded-library-object
TESTS="
t_control:ControlStream
t_platform:
t_replay:
t_rtsp_parser:
t_audio:AudioStream
t_video:VideoDepacketizer
t_input:InputStream
t_stun:
t_rtsp_conn:RtspConnection
"

wanted() {
    [ -z "$ONLY" ] && return 0
    for w in $ONLY; do [ "$w" = "$1" ] && return 0; done
    return 1
}

for entry in $TESTS; do
    name=${entry%%:*}
    excl=${entry#*:}
    wanted "$name" || continue
    objs=""
    for o in $OBJS; do
        case "$o" in
            "$OUT/obj/lib_${excl}.o") ;;
            *) objs="$objs $o" ;;
        esac
    done
    if ! $CC $TESTFLAGS $INC -I"$HERE" "$HERE/$name.c" $objs $PLATLIBS -o "$OUT/bin/$name" > "$OUT/log/build_$name.txt" 2>&1; then
        note "TEST BUILD FAIL $name"
        cat "$OUT/log/build_$name.txt"
        FAILED=1
        continue
    fi
    ASAN_OPTIONS=detect_leaks=0 timeout 240 "$OUT/bin/$name" > "$OUT/log/run_$name.txt" 2>&1
    rc=$?
    grep -E "^(FAIL|RESULT)" "$OUT/log/run_$name.txt" | tee -a "$SUMMARY"
    if [ $rc != 0 ] || ! grep -q "^RESULT $name.* passed\|^RESULT .* passed" "$OUT/log/run_$name.txt"; then
        note "TEST FAIL $name (exit $rc)"
        tail -20 "$OUT/log/run_$name.txt"
        FAILED=1
    fi
done

# ---------------------------------------------------------------- C++ header gate
# The host includes the library headers from C++20.
if wanted cxx_header_gate; then
    for std in c++17 c++20; do
        if $CXX -std=$std -Wall -Wextra -Werror -fsyntax-only -I"$COMMON" -I"$LIB/enet/include" "$HERE/cxx_header_gate.cpp" > "$OUT/log/cxx_$std.txt" 2>&1; then
            note "RESULT cxx_header_gate_$std passed 1 0"
        else
            note "TEST FAIL cxx_header_gate $std"
            cat "$OUT/log/cxx_$std.txt"
            FAILED=1
        fi
    done
fi

# ---------------------------------------------------------------- FEC round trip
# host nanors (19f07b5, used by the host encoder) against library nanors (a9b1636, used by PC and
# Android decoders): identical parity bit for bit and recovery of lost shards in both directions.
if wanted fec_roundtrip && [ -f "$HOSTDIR/src/rswrapper.c" ] && [ -d "$HOSTDIR/third-party/nanors" ]; then
    FECOK=1
    $CC -std=gnu11 -g -O2 -w -DHOST_NANORS -I"$HERE" -I"$HOSTDIR/src" -I"$HOSTDIR/third-party/nanors"         -I"$HOSTDIR/third-party/nanors/deps/obl" $SAN "$HERE/fec_roundtrip.c" "$HOSTDIR/src/rswrapper.c"         -o "$OUT/bin/fec_host" > "$OUT/log/build_fec_host.txt" 2>&1 || { note "TEST BUILD FAIL fec_host"; cat "$OUT/log/build_fec_host.txt"; FECOK=0; }
    $CC -std=gnu11 -g -O2 -w -I"$HERE" -I"$LIB/nanors" -I"$LIB/nanors/deps" -I"$LIB/nanors/deps/obl" $SAN         "$HERE/fec_roundtrip.c" "$OUT/obj/nanors_rs.o" "$OUT/obj/nanors_oblas_common.o" "$OUT/obj/nanors_oblas_lite.o"         -o "$OUT/bin/fec_lib" > "$OUT/log/build_fec_lib.txt" 2>&1 || { note "TEST BUILD FAIL fec_lib"; cat "$OUT/log/build_fec_lib.txt"; FECOK=0; }
    if [ "$FECOK" = 1 ]; then
        for step in "fec_host gen host.bin" "fec_lib gen lib.bin" "fec_host check host.bin" "fec_host check lib.bin" "fec_lib check host.bin" "fec_lib check lib.bin"; do
            set -- $step
            ASAN_OPTIONS=detect_leaks=0 timeout 600 "$OUT/bin/$1" $2 "$OUT/$3" > "$OUT/log/run_$1_$2_$3.txt" 2>&1
            rc=$?
            grep -E "^(FAIL|RESULT|fec )" "$OUT/log/run_$1_$2_$3.txt" | tee -a "$SUMMARY"
            if [ $rc != 0 ]; then
                note "TEST FAIL $step (exit $rc)"
                FAILED=1
            fi
        done
        cmp "$OUT/host.bin" "$OUT/lib.bin" > /dev/null 2>&1 && note "fec: host and library files are byte identical" || { note "TEST FAIL fec files differ"; FAILED=1; }
    else
        FAILED=1
    fi
fi

if [ "$KEEP" = 0 ] && [ "$FAILED" = 0 ]; then :; fi
if [ "$FAILED" = 0 ]; then
    note "ALL PASSED ${SAN:+(sanitizers)}"
    exit 0
fi
note "FAILED"
exit 1
