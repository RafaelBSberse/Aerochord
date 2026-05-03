# Aerochord

A digital hyperinstrument that turns hand gestures (captured by an RGB webcam) into real-time MIDI 2.0 musical events.

**Undergraduate thesis (TCC) — Computer Science, Universidade de Caxias do Sul (UCS).** Currently in the TCC 2 (implementation) phase.

---

## Architecture

Multithreaded pipeline with 5 core modules and an optional visualization module, communicating through lock-free SPSC queues:

```
Webcam
  │
  ▼
[Capture]            src/capture/             ← V4L2 / DirectShow / AVFoundation
  │  VideoFrame
  ▼
[Pose Detection]     src/pose_detection/      ← MediaPipe Hand Landmarker
  │  HandLandmarks
  ▼
[Gesture Analysis]   src/gesture_analysis/    ← FSM + EMA filter
  │  GestureEvent
  ▼
[Mapping]            src/mapping/             ← configurable profiles, legato, GM program
  │  MidiCommand
  ▼
[MIDI Generation]    src/midi_generation/     ← JUCE + ALSA UMP (MIDI 2.0)
  │
  ▼
Synthesizer (FluidSynth, DAW, hardware)

(parallel) [Visualization]  src/visualization/   ← OpenCV: preview, HUD and controls
```

End-to-end latency target: **< 30 ms**.

---

## Prerequisites

| Dependency | Minimum version | Provided by |
|------------|-----------------|-------------|
| CMake      | 3.22            | System      |
| C++17 compiler | GCC 11 / Clang 13 / MSVC 2019 | System |
| JUCE       | 8.x             | Automatic via FetchContent |
| MediaPipe C++ Tasks | any   | Manual — see below |
| OpenCV (core, highgui, imgproc) | 4.x | System (`libopencv-dev`) — optional |
| ALSA (Linux) | 1.2.10+       | `libasound2-dev` — optional, for native UMP |
| FluidSynth + SoundFont | any | `fluidsynth fluid-soundfont-gm` — used by `run.sh` |

### MediaPipe

MediaPipe has no official CMake integration. Two options:

**Option A — pre-built binaries (recommended):**
1. Download the [MediaPipe Hand Landmarker](https://developers.google.com/mediapipe/solutions/vision/hand_landmarker) artifacts for C++.
2. Extract them into `<some-path>/mediapipe-sdk/`.
3. Pass `-DMEDIAPIPE_ROOT=<some-path>/mediapipe-sdk` to CMake.

**Option B — build from source:**
Follow the [official MediaPipe guide](https://google.github.io/mediapipe/getting_started/install.html) (requires Bazel).

> The `assets/hand_landmarker.task` model is committed to the repository.

Without `-DMEDIAPIPE_ROOT`, the project compiles with a **stub** that keeps the pipeline running with zeroed landmarks — useful for developing and testing the downstream modules.

---

## Build

```bash
# MediaPipe stub (development)
cmake -B build -DCMAKE_BUILD_TYPE=Debug

# With real MediaPipe
cmake -B build -DCMAKE_BUILD_TYPE=Release -DMEDIAPIPE_ROOT=/path/to/mediapipe-sdk

# With tests
cmake -B build -DAEROCHORD_BUILD_TESTS=ON

# Compile
cmake --build build -j$(nproc)
```

---

## Running

### Recommended — `run.sh` (Linux with FluidSynth)

```bash
sudo apt install fluidsynth fluid-soundfont-gm
./run.sh
```

The script starts FluidSynth, discovers its ALSA port, and launches Aerochord wired straight into the synthesizer's MIDI input. It forwards any extra arguments to the binary (`./run.sh --device 1`, `./run.sh --eval`, etc.).

### Direct

```bash
./build/aerochord_artefacts/Debug/aerochord [options]
```

### Command-line options

| Flag              | Description                                                              |
|-------------------|--------------------------------------------------------------------------|
| `--device N`      | Webcam index (default: 0)                                                |
| `--midi-out NAME` | Substring of the target MIDI output port                                 |
| `--res WxH`       | Target resolution (e.g. `1280x720`); default: maximum the camera reports |
| `--width N`       | Width only (combine with `--height`)                                     |
| `--height N`      | Height only                                                              |
| `--fps N`         | Target FPS; default: highest the camera supports                         |
| `--no-viz`        | Disable the OpenCV visualization window                                  |
| `--eval`          | Record a latency CSV (`aerochord_eval_<timestamp>.csv`)                  |

### Tests

```bash
ctest --test-dir build --output-on-failure
```

---

## Visualization window and runtime controls

When OpenCV is available, Aerochord opens a window with:

- **Webcam preview** with the detected landmarks overlaid
- **Note guide** showing the 12 vertical zones of the chromatic scale (with a tolerance gap at the top and bottom — the hand can drift slightly out of frame without losing the lowest/highest note)
- **HUD** with FPS, MIDI mode (1.0 / 2.0), active note, and dropped frames
- **Bottom panel** with:
  - **Legato** toggle (enables note overlap during a zone transition while the pinch is held)
  - **Instrument** selector (20 General MIDI voices: piano, e.piano, marimba, organ, etc.)

Press `Ctrl+C` in the terminal to shut the pipeline down cleanly.

---

## Gesture-to-sound mapping

| Function          | Hand   | Gesture                                                |
|-------------------|--------|--------------------------------------------------------|
| Octave selection  | Left   | Pinch + vertical movement                              |
| Global volume     | Left   | Open hand + vertical movement                          |
| Note on           | Right  | Pinch (thumb + index) inside a vertical zone           |
| Note off          | Right  | Release the pinch                                      |
| Legato            | Right  | Move between zones while holding the pinch (UI toggle) |
| Timbre / Filter   | Right  | Middle-finger to palm distance (during an active pinch)|
| Pitch bend        | Right  | Hand tilt forward/back (during an active pinch)        |
| Vibrato           | Right  | Slight lateral oscillation (during an active pinch)    |
| Velocity          | Right  | Speed of the pinch closing                             |

If the hand leaves the frame during a pinch hold, Aerochord emits `NOTE_OFF` automatically after 200 ms — preventing stuck notes.

---

## Directory layout

```
Aerochord/
├── CMakeLists.txt
├── cmake/
│   └── FetchDependencies.cmake     # JUCE, MediaPipe, ALSA, OpenCV
├── run.sh                          # Linux launcher with FluidSynth
├── assets/
│   └── hand_landmarker.task        # MediaPipe model (versioned)
├── src/
│   ├── main.cpp                    # Entry point — CLI and pipeline assembly
│   ├── common/                     # Shared types, lock-free queue, logger, thread utils
│   ├── capture/                    # CaptureModule (V4L2 / DirectShow / AVFoundation)
│   ├── pose_detection/             # PoseDetectionModule (MediaPipe Tasks)
│   ├── gesture_analysis/           # GestureAnalysisModule (FSM + EMA)
│   ├── mapping/                    # MappingModule (profiles, legato, GM program)
│   ├── midi_generation/            # MidiGenerationModule (JUCE + ALSA UMP)
│   └── visualization/              # VisualizationModule (OpenCV + UI)
├── tests/                          # GoogleTest: queue, gesture, mapping, latency
└── tools/
    └── analyze_eval.py             # Analysis of the CSVs produced by --eval
```

---

## Code conventions

- Lock-free (SPSC) queues for **all** inter-module communication
- Vision and MIDI threads are never blocked — no mutexes on the hot path
- Timestamps are stamped onto UMP packets **immediately** after gesture detection
- Structured logging in every module (`AEROCHORD_LOG_*`)
- EMA smoothing applied to landmarks **before** they reach the gesture analysis stage
- Runtime controls (UI → MappingModule) exchange state through `std::atomic` — never with mutexes on the audio path
