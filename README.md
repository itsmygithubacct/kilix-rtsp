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

## Sharing one decode

Decoding is the most expensive thing this library does — a full-screen
source runs 21–44% of a core — so a second consumer that decodes its own copy
doubles the largest cost in the system to produce bytes that already exist. Two
viewers on one camera used to do exactly that.

A source can instead publish frames into a named POSIX shared-memory ring that
other processes attach to:

```c
krtsp_frame *ring;
krtsp_frame_init_shared(&ring, "poolcam", 640, 360, /* max_readers */ 4);

/* elsewhere, in another process */
krtsp_frame *reader;
krtsp_frame_attach(&reader, "poolcam");
```

Readers get the same borrow/release contract as an in-process consumer. The ring
holds `max_readers + 2` slots — one the producer is filling, one holding the
newest frame, and one per reader currently holding a borrow — and a borrow that
would leave the producer nowhere to write is refused rather than overwriting a
slot somebody is reading. `max_readers` counts *simultaneous borrows*, not
attached processes; readers that borrow and release promptly share far fewer
slots than their number.

Frames are not authenticated. Anything able to open the object can read the
camera's pixels, so it is created `0600`.

## Recording

The same ffmpeg process can also write the camera's own bitstream to disk, so
one RTSP session serves both a viewer and an archive — which matters because
some cameras refuse a second concurrent session:

```c
options.roles = KRTSP_ROLE_DECODE | KRTSP_ROLE_RECORD;
options.record_dir = "/srv/video/poolcam";
```

`roles` is a bitmask because all three combinations are real. `KRTSP_ROLE_RECORD`
alone never decodes at all: the segmenter copies packets straight to disk, so the
camera costs I/O and nothing else.

Recording never re-encodes — at the same resolution, re-encoding reliably
produces *larger* files than the camera's own stream while burning a core.

**Segments are Matroska by default.** mp4 cannot mux the `pcm_alaw` audio many
cameras carry — `-c copy` fails outright — and MPEG-TS accepts it while silently
dropping the audio stream. Matroska carries it untouched and tolerates a segment
truncated by a power cut. The pattern's extension picks the container, so
overriding it is a one-line change.

Two things about segment timing are worth knowing before they surprise you:

- **`segment_seconds` is a lower bound, honoured at the next keyframe.** With
  `-c copy` there is nowhere else to cut, so a camera with a 4-second GOP turns a
  10-second request into ~12-second files.
- **The pattern must resolve to a unique name per segment.** The default has
  second resolution, which is ample at 10-second segments in real time — but a
  process catching up faster than real time can produce two segments within one
  second and silently overwrite the first.

A pattern containing `/` builds a date hierarchy, which needs the muxer to create
directories. Not every ffmpeg can: **5.1 has `-strftime` but not
`-strftime_mkdir`**, and the failure is silent until an hour rolls over and every
segment starts failing. The default pattern is therefore flat, and
`krtsp_ffmpeg_supports_segment_mkdir()` reports whether a hierarchical one is
safe on the binary in use.

Health is tracked per role. A camera can deliver frames while its segmenter is
stuck on a full disk, or write segments after the decode pipe has stopped, so
each sink has its own staleness timer and `krtsp_source_status()` reports the
worse of the two.

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
