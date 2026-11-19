# Audio loading and playback

`import deren.audio;` provides file decoding, an owning PCM buffer and independently
controlled `deren::audio::audio` objects. WAV, MP3 and FLAC use miniaudio; Ogg
Vorbis uses stb_vorbis; Ogg Opus uses libopusfile/libopus/libogg; QOA uses the
reference decoder. The `.ogg` and `.oga` routes accept either Vorbis or Opus.
Every decoder produces interleaved f32 PCM in an owning `ma_audio_buffer` through
`audio_buffer::from_pcm`. Whole-file input is limited to 256 MiB and decoded
output to 512 MiB.

## Build and linkage

`deren_audio` is a shared target (`deren_audio.dll` on Windows), built beside
`deren.exe`. The executable links its import library and imports `deren.audio`;
the OS loads the DLL at startup. Loading the library does not open an output
device: the Meyers singleton still initializes only on first valid creation.
Decoder libraries link privately and statically into this DLL. The runtime
package includes it beside the executable, like `deren_assets.dll`.
Both sides use the same compiler, module interfaces and shared C++ runtime.

## Creating audio

`load_audio_buffer_from_file(path)` returns
`std::expected<std::shared_ptr<audio_buffer>, audio_file_error>`. The shared owner
can be passed straight to `create`, reused by several audio objects, or retained
by an asset cache. `create_from_file` reuses that owner without another allocation
of the buffer wrapper or another copy of PCM samples.

The factories are namespace-level functions. `create(buffer)` returns
`std::expected<std::shared_ptr<audio>, playback_error>`. `create_from_file(path)`
loads, owns and wraps a buffer in one call, returning
`std::expected<std::shared_ptr<audio>, audio_error>`; `audio_error` is a variant of
`audio_file_error` and `playback_error`, preserving the failing stage.
Both factories create stopped audio at frame zero, with volume and pitch 1,
center pan and looping disabled.

```cpp
namespace sound = deren::audio;
auto music = sound::create_from_file("music.opus");
if (!music) { return; }
(*music)->play();
auto another = (*music)->clone();
```

Call `create(buffer)` when decoding and sharing PCM explicitly:

```cpp
auto buffer = sound::load_audio_buffer_from_file("effect.qoa");
if (!buffer) { return; }
auto effect = sound::create(*buffer);
```

## Ownership and the internal engine

Each audio stores `std::shared_ptr<audio_buffer const> buffer`. Treat published
buffers as immutable: do not call `release` or move from a buffer while audio
objects use it. Each audio owns a separate miniaudio data source, cursor and
`ma_sound`; clones share PCM without another decode or sample copy. Initialized
audio objects keep stable addresses, so copy and move construction are disabled.

The engine is an internal Meyers singleton: `instance()` owns a function-local
static, initialized on first valid creation. Device setup and mixing are shared
by all audio objects. No public engine initialization or shutdown calls exist.
A setup failure produces `engine_init_failed` on creation; this first version
does not retry initialization or implement output device switching.
Unlike shared engine ownership, singleton device lifetime lasts until process
shutdown, including periods with no active audio objects.
Normal static destruction reverses initialization order; audio owners created
through these factories are destroyed before the engine. Stop worker threads
and release thread-owned audio before process shutdown. Audio objects created
from other static destructors are outside this lifecycle contract.
Sound detachment and source teardown happen before the buffer owner releases.

## Cloning and transport

- `clone(false)` shares the buffer, resets settings to defaults, starts at frame
  zero, and remains stopped.
- `clone(true)` additionally copies volume, pan, pitch, looping, the source-frame
  cursor and whether playback is active. A paused source produces a paused copy.
- Cloning captures a moment in time; it does not sample-synchronize live audio.
- `pause` retains position. `stop` returns to frame zero. `play` resumes, or restarts
  at the beginning after reaching the end. `seek`, `position` and `duration` use seconds.
- Volume must be finite and nonnegative, pitch finite and positive, and pan in
  [-1, 1]. Seeking rejects negative, nonfinite and out-of-range positions.
- Control calls on each audio are serialized by the caller. The mixer publishes
  its cursor through an atomic snapshot instead of exposing its mutable cursor.

## Validation status

Fixtures are synthesized two-channel 48 kHz tones, with different tones per
channel. QOA was encoded with the pinned reference encoder; Vorbis, Opus, MP3
and FLAC fixtures were encoded with FFmpeg. The WAV block fixture has exactly
4096 frames to cover clean decoder end-of-stream handling.
`test_audio_load` covers codecs and ownership; `test_audio_playback` describes
factory, clone and transport behavior. Test-only hooks select offline mixing
before the singleton initializes; no device-selection setting is added to the
public API.
The audio targets compile and both audio CTests pass. A 44.1 kHz stereo FLAC
preview opened the default output device and its source cursor advanced normally;
audible quality is checked by the listener.

For a local device preview, pass one audio file path to the playback test program:

```powershell
./build/test_audio_playback.exe "path/to/song.flac"
```

Without arguments it runs the headless tests. With one path it plays through
`create_from_file`, at volume 0.35, until the song ends. The Windows entry point
accepts Unicode paths.

## C# scripting

The managed host resolves optional audio module 3. `deren.audio_buffer` and
`deren.audio` wrap opaque session ids and implement `IDisposable`; no native
pointer or shared_ptr crosses into C#. Relative paths use the managed project
directory. Keep playback in a game field, then dispose it on shutdown:

```csharp
// Game field:
private deren.audio? music;

// Initialization:
music = deren.audio.create_from_file("audio/music.opus");
music?.try_set_volume(0.5f);
music?.play();

// Shutdown:
music?.Dispose();
music = null;
```

Use `audio_buffer.try_load_from_file(path, out buffer, out status)` to share one
load between `audio.create(buffer)` calls. Use `try_create_from_file` or
`try_clone` when the failure status is needed. A disposed buffer does not stop
existing audio; each native playback object retains the shared PCM owner.
