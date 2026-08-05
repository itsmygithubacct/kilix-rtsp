# kilix-rtsp

`kilix-rtsp` is a C11/POSIX library and command that pulls RTSP camera streams
and presents them inside a terminal through the [Kitty graphics
protocol](https://sw.kovidgoyal.net/kitty/graphics-protocol/). One camera fills
the window; several compose into a mosaic.

Acquisition is an `ffmpeg` subprocess per stream, supervised: the library reads
fixed-size frames from a pipe, and a watchdog restarts a stream that dies *or*
that stays connected while silently delivering nothing — which is how network
cameras usually fail. Frames reach the terminal through
[`kitty-framebuffer`](https://github.com/itsmygithubacct/kitty-framebuffer).

The library is acquisition and presentation only. Recording, retention, and
object detection are a separate product's concern.

## Checkout, build and test

```sh
git clone --recurse-submodules https://github.com/itsmygithubacct/kilix-rtsp.git
cd kilix-rtsp
make
make test
make sanitize
```

The submodules are required: the terminal-facing commands build against
`kitty-terminal-session`, `soft-raster` and `kitty-pty-broker`. The library
itself — acquisition — depends on none of them.

Dependencies are a C11 compiler, POSIX, pthreads, and the **`ffmpeg` binary at
runtime** — not the FFmpeg libraries at link time. `kilix-rtsp` never links
`libavcodec`; it spawns `ffmpeg` and reads raw frames from its stdout. That
keeps codec failures in a separate, restartable process, makes the pipe
self-framing (a fixed pixel format means one frame is exactly
`width * height * 4` bytes), and picks up whatever hardware acceleration the
installed ffmpeg has.

## Use

```sh
kilix-rtsp list                    # configured cameras and groups
kilix-rtsp probe <name|url>        # stream properties
kilix-rtsp view   <name|url>       # one camera filling the window
kilix-rtsp mosaic [group|name...]  # several cameras in a grid
```

`probe` accepts a bare URL as well as a configured name, so it is useful before
any configuration exists. It prints `key=value` on stdout and progress on
stderr, so the output pipes cleanly. `mosaic` with no argument shows every
configured camera.

Options: `--tier main|sub`, `--fps <n>`, `--config <path>`, and `--tab` to open
a view in a new terminal tab. `q` or escape quits.

Every camera publishes more than one stream, and the right one depends on how
large it will be drawn: a mosaic tile wants the substream, because scaling a
640×360 source into a 320×180 tile already discards half the pixels, while a
full-window view wants the main stream rather than upscaling a substream the
camera is already encoding at higher resolution. `view` and `mosaic` default
accordingly.

Cost follows output pixels rather than camera count, which is not the intuitive
result: on one machine a seven-camera grid measured cheaper per camera than a
single full-window view, because each tile decodes to a small frame.

## Configuration

Configuration and data live **outside this repository**, under
`~/.local/gpu_terminal/kilix-rtsp/`:

```
config/   camera definitions and groups
data/     durable output
cache/    regenerable scratch
state/    saved layout and per-camera status
logs/     per-source ffmpeg stderr
```

Override the root with `KILIX_RTSP_HOME`. The `ffmpeg` and `ffprobe` binaries
can be overridden with `KILIX_RTSP_FFMPEG` and `KILIX_RTSP_FFPROBE`, which
matters on hosts carrying a vendor build with different codec support than the
distribution's.

Start from `examples/cameras.conf.example`:

```sh
mkdir -p ~/.local/gpu_terminal/kilix-rtsp/config
cp examples/cameras.conf.example \
   ~/.local/gpu_terminal/kilix-rtsp/config/cameras.conf
chmod 600 ~/.local/gpu_terminal/kilix-rtsp/config/cameras.conf
```

**Camera configuration is a secret file.** RTSP URLs embed credentials as
`rtsp://user:password@host/path`, so it is refused unless it is a regular file
owned by you with no group or world permission bits, it never lives in the work
tree, and errors never quote a URL. URLs are redacted wherever they are shown.

Passwords containing `@` or `:` are handled automatically. Passwords containing
`/`, `?` or `#` must be percent-encoded in the config file — each of those ends
the URL's authority, so `rtsp://u:a/b@host/1` genuinely parses as host `u` with
path `/b@host/1` under any conforming parser, and nothing downstream can recover
what was meant.

## Detached views

Under a terminal that separates a pane's lifetime from its frontend — kilix does
this with `kitty-pty-broker` — closing a tab detaches the session rather than
ending it, so it can be attached again later. A detached view has nobody to show
frames to, so it stops its stream and starts a new one on reattach. Outside such
a terminal there is nothing to detect and the view always streams.

## License

MIT. See `LICENSE`. The pinned dependencies retain their own notices.
