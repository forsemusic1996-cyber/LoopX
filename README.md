# Mini Sampler VST3

Minimal JUCE sampler test project.

## What it does

- Builds a VST3 plug-in and a Standalone test application.
- Starts with a generated test sample.
- Loads WAV, AIFF, or FLAC through the plug-in editor.
- Plays the loaded sample from MIDI or the on-screen keyboard.

## Local configuration

With JUCE at `D:\CppLibrary\JUCE`:

```powershell
cmake -S . -B build -G \"Visual Studio 17 2022\" -A x64 `
  -DJUCE_DIR=\"D:/CppLibrary/JUCE\"
cmake --build build --config Release
```

The local machine needs a C++ toolchain for the local build. The GitHub workflow downloads JUCE on the Windows runner and uploads the resulting VST3 artifact.

## GitHub build

The workflow runs on pushes to `main` or manually from the Actions tab. Download `MiniSampler-VST3` from the completed workflow run.
