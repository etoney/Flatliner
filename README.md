# Flatliner

An automatic vocal fader rider (VST3 / AU) by **Castle Danger Music Company**.

Flatliner listens to the vocal and moves a virtual fader for you, pushing quiet words up and pulling loud ones down so the performance sits on one flat line.

- **Target / Range / Speed:** the level to aim for, how far the fader may move, and how fast it moves (cuts are always faster than boosts)
- **Gate:** the fader freezes below this level so breaths and room noise don't get pushed up
- **Lookahead:** 0-50 ms; the audio is delayed while the rider listens early, so the fader is already moving when a phrase arrives (latency reported to the host)
- **Music:** route the backing track to the sidechain and the vocal target rises with the music
- **Extras:** live scrolling scope, fader gauge, VOICE / FADER / OUT meters, factory + user presets

See `controls.txt` for every control and `readme.txt` for installing.

## Building

Pushing to `main` runs GitHub Actions, which builds macOS (VST3 + AU, universal) and Windows (VST3), then:

- uploads the builds as workflow artifacts, and
- pushes the release zips to the `downloads-macos` / `downloads-windows` branches.

A Linux job runs the headless checks (`test/snapshot.cpp`): DSP tests (levelling, RIDE off, range, gate, lookahead latency and effect, music sidechain, output trim), preset checks, and renders the editor to PNGs, uploaded as the `gui-snapshots` artifact.

Local build:

```
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --target Flatliner_VST3
```

Built with JUCE 8.0.15 (same as Cutthroat, DREAD, Possessed and Dead Air).
