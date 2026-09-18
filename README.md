# Mini Sampler / LoopX v0.3
JUCE Windows x64 VST3 sampler, built and tested by GitHub Actions.

## Sample and BPM
Drop a local audio file from Explorer or a DAW media browser, or use Settings > Load audio file.
BPM opens the sample's Original BPM field. Enter the source tempo (20–400 BPM, decimals allowed), then Match BPM.
Target BPM is always the project tempo. Signalsmith Stretch 1.1.0 performs pitch-preserving conversion on a background worker, from the original audio, never a previously stretched copy.
Host-tempo changes automatically rebuild the processed audio. The original file is never changed.
The live engine uses streaming WSOLA during transitions and for musical loop lengths.
Very large tempo ratios can produce stretch artifacts; source tempo is not detected automatically.

START enters original-file start-marker editing. Move the marker without Snap or ZC, then press OK.
The logical waveform starts at that sample, and loop/slots are remapped to retain their source material.

## Playback and MIDI
Settings > Playback: MIDI Trigger (default) or Continuous / Host Sync.
MIDI Trigger starts/restarts on Note On, releases when no note is held. Continuous follows host play/stop and PPQ.
MIDI key tracking is optional and off by default.
Settings > MIDI: Velocity Off, Velocity -> Slot, or Velocity -> Loop Position.
Velocity slot boundaries: 1–13, 14–26, 27–39, 40–51, 52–64, 65–77, 78–89, 90–102, 103–115, 116–127.

## Editing and automation
Drag a selection and press SET, or right-click to apply it. Segments activate on mouse down.
Drag loop boundaries or the bottom loop handle. Drag the two upper fade handles to change independent fade-in/out.
Mouse wheel zooms; Shift+wheel or middle-drag pans. Scrollbar is above waveform.
+ stores up to ten slots. Left-click recalls, right-click removes. Keys 1–9/0 recall.
Host automation: Slot 0 = manual loop, 1–10 = saved slots; Loop Position = normalized sample position.
Loop Position and velocity movement follow the chosen grid when Snap is enabled.
Short transitions reduce switch clicks.
Space requests host play/pause through supported transport control or Windows host-window key forwarding.
Some hosts intercept keys differently; this needs checking in each DAW.

## Project state
Sample file paths are linked, not embedded. Keep the source WAV available when moving projects.
START, BPM matching, loops, slots, fades, grid, playback/MIDI settings, theme, and editor size are saved.
Five themes: Default, Graphite, Coral, Violet, Amber. Bright Grid is off by default.

## Build
GitHub Actions uses Windows 2022 / MSVC and CMake, builds VST3 and Standalone with static runtime and PDBs.
Download MiniSampler-VST3 from the successful run and preserve the complete .vst3 bundle.
The symbols artifact is optional for crash analysis.
CTest checks file-drop targets, exact waveform peaks, UI state, loop tools, MIDI/automation, START, Signalsmith duration/pitch/stereo, and audio/resize concurrency.
EngineChecks independently verifies pitch preservation at several live tempo ratios.

Signalsmith Stretch and its DSP dependency are MIT-licensed; both notices are included in the artifact.
JUCE licensing still applies independently.
