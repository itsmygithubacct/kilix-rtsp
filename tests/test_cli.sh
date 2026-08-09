#!/bin/sh
set -eu

binary=$1
fake=$2
scratch=$(mktemp -d)
trap 'rm -rf -- "$scratch"' EXIT HUP INT TERM

expect_status()
{
    expected=$1
    description=$2
    shift 2

    set +e
    "$@" >"$scratch/stdout" 2>"$scratch/stderr"
    actual=$?
    set -e
    if [ "$actual" -ne "$expected" ]; then
        printf 'not ok %s (expected %s, got %s)\n' \
            "$description" "$expected" "$actual" >&2
        sed -n '1,20p' "$scratch/stderr" >&2
        exit 1
    fi
}

export KILIX_RTSP_HOME=$scratch

expect_status 0 help "$binary" --help
expect_status 2 "unknown command" "$binary" unknown
expect_status 2 "view needs target" "$binary" view
expect_status 2 "view rejects extra target" "$binary" view one two
expect_status 2 "fps rejects text" "$binary" view --fps nope one
expect_status 2 "fps rejects negative" "$binary" view --fps -1 one
expect_status 2 "fps rejects explicit sign" "$binary" view --fps +1 one
expect_status 2 "fps rejects whitespace" "$binary" view --fps " 1" one
expect_status 2 "fps rejects overflow" "$binary" view --fps 999999999999 one
expect_status 2 "fps needs value" "$binary" view one --fps
expect_status 2 "config needs value" "$binary" list --config
expect_status 2 "list rejects target" "$binary" list one
expect_status 2 "probe rejects view option" "$binary" probe --fps 10 \
    rtsp://example.invalid/live
expect_status 2 "mosaic rejects tier" "$binary" mosaic --tier main
expect_status 2 "mosaic rejects seventeenth target" "$binary" mosaic \
    a b c d e f g h i j k l m n o p q

set +e
FAKE_FFPROBE_MODE=valid KILIX_RTSP_FFPROBE=$fake \
    "$binary" probe 'rtsp://u:p@example.invalid/a;literal' \
    >"$scratch/stdout" 2>"$scratch/stderr"
actual=$?
set -e
if [ "$actual" -ne 0 ] || ! grep -q 'codec_type=video' "$scratch/stdout"; then
    printf 'not ok valid CLI probe\n' >&2
    sed -n '1,20p' "$scratch/stderr" >&2
    exit 1
fi

set +e
FAKE_FFPROBE_MODE=novideo KILIX_RTSP_FFPROBE=$fake \
    "$binary" probe rtsp://example.invalid/audio \
    >"$scratch/stdout" 2>"$scratch/stderr"
actual=$?
set -e
if [ "$actual" -ne 1 ] || ! grep -q 'no video stream' "$scratch/stderr"; then
    printf 'not ok audio-only CLI probe\n' >&2
    exit 1
fi

set +e
FAKE_FFPROBE_MODE=emptyvideo KILIX_RTSP_FFPROBE=$fake \
    "$binary" probe rtsp://example.invalid/empty \
    >"$scratch/stdout" 2>"$scratch/stderr"
actual=$?
set -e
if [ "$actual" -ne 1 ] || ! grep -q 'did not describe' "$scratch/stderr"; then
    printf 'not ok incomplete CLI probe\n' >&2
    exit 1
fi

printf 'ok cli validation and probe\n'
