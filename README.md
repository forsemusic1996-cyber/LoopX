# LoopX v0.4.1
JUCE is reproducibly pinned to release 9.0.2, commit `72782788ce18c2d4d760b28e0921d6ffc6431102`; the cloud build does not use a JUCE installation from the developer PC.
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
If a region is clipped at a boundary, its beat length scales proportionally so START never changes its playback speed.

## Playback and MIDI
Settings > Playback: MIDI Trigger (default) or Continuous / Host Sync.
MIDI Trigger starts/restarts on Note On, releases when no note is held. Continuous follows host play/stop and PPQ.
MIDI key tracking is optional and off by default.
Settings > MIDI: Velocity Off, Velocity -> Slot, or Velocity -> Loop Position.
Velocity slot boundaries: 1–13, 14–26, 27–39, 40–51, 52–64, 65–77, 78–89, 90–102, 103–115, 116–127.
Settings > MIDI also offers Note -> Slot and Note -> Loop Position (both optional, initially off).
Note -> Slot: root note (default MIDI 60, shown as C3 in LoopX) selects Slot 1; the next nine semitones select Slots 2–10.
Slot markers show the note name. Note mapping shows note/velocity ranges. Empty slots are silent.
Note -> Loop Position: MIDI notes 0–127 select the first 128 grid divisions, preserving loop length, beat length and pitch. No banks.
Positions past the end clamp to the last valid loop start. Root note affects only Slot mode.
Note mappings take precedence over velocity mappings. Host Slot/Loop Position writes take effect until the next mapped Note On.
JUCE exports Slot/Grid note labels to hosts that support custom note names; DAW octave conventions differ. Channel-specific labels are not guaranteed by VST3 hosts.

## Editing and automation
Drag a selection and press SET, or right-click to apply it. Segments activate immediately on mouse down/drag and disable ordinary Selection while that mode is active.
Drag loop boundaries or the bottom loop handle. The bottom handle directly touches and writes the host-visible Loop Position parameter, so DAW Last Tweaked/automation recording works without opening Settings. Its four-arrow cursor and drag action share the exact same hit-zone.
Drag the two upper fade handles to change independent fade-in/out.
Mouse wheel zooms; Shift+wheel or middle-drag pans. Scrollbar is above waveform.
+ stores up to ten slots. Left-click recalls, right-click removes. Keys 1–9/0 recall.
Host automation: Slot 0 = manual loop, 1–10 = saved slots; Loop Position = normalized sample position.
Settings > Loop Position (automation) remains available as an alternative host-linked slider; choose the parameter named Loop Position for an envelope.
Parameter IDs remain `slot` and `loopPosition` for compatibility with existing automation.
Loop is a fixed-label on/off toggle; switching it off preserves the current region.
Loop Position and velocity movement follow the chosen grid when Snap is enabled.
Short transitions reduce switch clicks.
Space requests host play/pause through supported transport control or Windows host-window key forwarding.
Some hosts intercept keys differently; this needs checking in each DAW.

## Project state
LoopX keeps the existing manufacturer/plugin codes and automation parameter IDs. New state is saved with the `LoopX` tag; the previous state tag is still accepted when loading existing projects.
Sample file paths are linked, not embedded. Keep the source WAV available when moving projects.
START, BPM matching, loops, slots, fades, grid, playback/MIDI settings, theme, and editor size are saved.
Five complete professional palettes: Studio Dark, Graphite, Slate, Warm Gray, Studio Light. Bright Grid is off by default.
Settings > Themes > Theme editor: 33 separately editable RGBA colours, HSV colour selection, independent Selection/Loop alpha, live preview, user-theme save/rename, reset base, Cancel/Done, `.theme.json` import/export.
The complete active palette and saved user-theme library are embedded in project state; external theme files are not needed to restore a project.
Last-used grid/theme and UI settings also persist locally in the application-data `LoopX/settings.json`; project state takes precedence. Closing only the editor does not reset settings.
The BPM button uses a compact 256 x 138 panel. Zoomed-out waveforms use a dimmer, anti-aliased envelope through the original pixel extrema. Audio is unchanged.
The bottom Loop handle uses JUCE's standard four-direction cursor (Windows IDC_SIZEALL), including while dragging.
Unload safety: bounded/chunked decode, cancellable waveform/render work, editor-owned control panels/timers, asynchronous non-parameter host notifications outside locks. Automated unload stress is not a substitute for a real FL Studio project test.

## Build
GitHub Actions uses Windows 2022 / MSVC and CMake, builds VST3 and Standalone with static runtime and PDBs.
Download LoopX-VST3 from the successful run and preserve the complete .vst3 bundle.
The symbols artifact is optional for crash analysis.
CTest checks file-drop targets, exact waveform peaks, UI state, loop tools, MIDI/automation, START, Signalsmith duration/pitch/stereo, and audio/resize concurrency.
It also checks fresh-instance theme/grid restoration, local defaults, 33-field theme roundtrip/validation, note mappings/names, START speed invariance, cross-thread host state queries and repeated unload during rendering with an open panel.
EngineChecks independently verifies pitch preservation at several live tempo ratios.

Signalsmith Stretch and its DSP dependency are MIT-licensed; both notices are included in the artifact.
JUCE licensing still applies independently.
