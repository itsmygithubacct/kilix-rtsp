# kilix-rtsp

`kilix-rtsp` is a C11/POSIX library and command that pulls RTSP camera streams
and presents them inside a terminal through the [Kitty graphics
protocol](https://sw.kovidgoyal.net/kitty/graphics-protocol/). One camera fills
the terminal; several compose into a mosaic.

Acquisition is an `ffmpeg` subprocess per stream, supervised: the library reads
fixed-size RGBA frames from a pipe, and a watchdog restarts a stream that dies
*or* that stays connected while silently delivering nothing — which is how
network cameras usually fail. Frames reach the terminal through
[`kitty-framebuffer`](https://github.com/itsmygithubacct/kitty-framebuffer).

The library is acquisition and presentation only. Recording, retention, and
object detection are a separate product's concern.

## Status

**Design complete, implementation not started.** The plan, the measurements
behind it, and the reading of the reference implementation it is based on are
written up but deliberately not shipped in this repository.

## Build and test

Nothing builds yet — there are no sources in this repository. When there are,
the module follows the workspace convention:

```sh
make
make test
make sanitize
```

Dependencies will be a C11 compiler, POSIX, pthreads, and the **`ffmpeg` binary
at runtime** — not the FFmpeg libraries at link time. `kilix-rtsp` never links
`libavcodec`; it spawns `ffmpeg` and reads raw frames from its stdout. That
keeps codec failures in a separate, restartable process and picks up whatever
hardware acceleration the installed ffmpeg has.

## Use

```sh
kilix-rtsp probe   <url>          # stream properties
kilix-rtsp view    <camera>       # one camera, full terminal
kilix-rtsp mosaic  <group>        # several cameras in a grid
```

Every camera has more than one stream tier, and the right one depends on the
view: a mosaic tile wants the substream, a full-terminal view wants the main
stream. `view` and `mosaic` default accordingly.

## Configuration

Configuration and data live **outside this repository**, under
`~/.local/gpu_terminal/kilix-rtsp/`:

```
config/   camera definitions and mosaic layouts
data/     durable output
cache/    regenerable scratch
state/    saved layout and per-camera status
logs/     per-source ffmpeg stderr
```

Override the root with `KILIX_RTSP_HOME`.

**Camera configuration is a secret file.** RTSP URLs embed credentials as
`rtsp://user:password@host/path`, so `config/` is mode 0700 with 0600 files and
never lives in the work tree. `examples/cameras.conf.example` ships placeholders
only.

## License

MIT. See `LICENSE`.
