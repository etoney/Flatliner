# Flatliner

An auto leveler (VST3 / AU) by **Castle Danger Music Company**. *Level the Field.*

Flatliner listens to the track and levels it for you, pushing quiet passages up and pulling loud ones down so the performance sits on one flat line. Use it on a lead vocal, lead guitar, bass, voice-over or anything that jumps around.

- **Target / Range / Speed:** the level to aim for, how far the level may move, and how fast it moves (cuts are always faster than boosts)
- **Gate:** Flatliner holds below this level so breaths, hiss and room noise don't get pushed up
- **Lookahead:** 0-50 ms; the audio is delayed while Flatliner listens early, so the level is already moving when a phrase arrives (latency reported to the host)
- **Music:** route the backing track to the sidechain and the target rises with the music
- **Extras:** live scrolling scope, gain gauge, input / gain / output meters, factory + user presets
- **ARA:** apply Flatliner to the clip itself and it plans the leveling from the whole take, with no latency

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
