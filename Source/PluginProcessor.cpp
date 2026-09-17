#include "PluginProcessor.h"
#include "PluginEditor.h"

#if JUCE_WINDOWS
 #include <Windows.h>
 #include <DbgHelp.h>
 #include <string>
#endif

namespace
{
#if JUCE_WINDOWS
std::atomic_flag crashDumpWritten = ATOMIC_FLAG_INIT;
std::wstring crashDumpDirectory;

LONG WINAPI miniSamplerVectoredExceptionHandler(EXCEPTION_POINTERS* exceptionInfo) noexcept
{
    if (exceptionInfo == nullptr || crashDumpWritten.test_and_set())
        return EXCEPTION_CONTINUE_SEARCH;
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t dumpPath[MAX_PATH]{};
    const auto* directory = crashDumpDirectory.empty() ? L"." : crashDumpDirectory.c_str();
    swprintf_s(dumpPath, MAX_PATH, L"%s\\MiniSampler-crash-%04u%02u%02u-%02u%02u%02u-%lu.dmp", directory,
               static_cast<unsigned int>(now.wYear), static_cast<unsigned int>(now.wMonth),
               static_cast<unsigned int>(now.wDay), static_cast<unsigned int>(now.wHour),
               static_cast<unsigned int>(now.wMinute), static_cast<unsigned int>(now.wSecond),
               static_cast<unsigned long>(GetCurrentProcessId()));
    const auto dumpFile = CreateFileW(dumpPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
    if (dumpFile != INVALID_HANDLE_VALUE)
    {
        MINIDUMP_EXCEPTION_INFORMATION dumpInfo{};
        dumpInfo.ThreadId = GetCurrentThreadId();
        dumpInfo.ExceptionPointers = exceptionInfo;
        dumpInfo.ClientPointers = FALSE;
        const auto dumpType = static_cast<MINIDUMP_TYPE>(MiniDumpWithDataSegs | MiniDumpWithThreadInfo
            | MiniDumpWithUnloadedModules | MiniDumpWithIndirectlyReferencedMemory);
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), dumpFile, dumpType, &dumpInfo, nullptr, nullptr);
        CloseHandle(dumpFile);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void installMiniSamplerCrashDumpHandler()
{
    const auto desktop = juce::File::getSpecialLocation(juce::File::userDesktopDirectory).getFullPathName();
    crashDumpDirectory.assign(desktop.toWideCharPointer());
    AddVectoredExceptionHandler(1, miniSamplerVectoredExceptionHandler);
}
#endif
}

MiniSamplerAudioProcessor::MiniSamplerAudioProcessor()
    : AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true))
{
#if JUCE_WINDOWS
    installMiniSamplerCrashDumpHandler();
#endif
    writeDiagnostic("processor: constructor begin");
    formatManager.registerBasicFormats();
    writeDiagnostic("processor: formats registered");
    for (auto i = 0; i < 16; ++i)
        synth.addVoice(new juce::SamplerVoice());
    writeDiagnostic("processor: voices added");

    juce::AudioBuffer<float> generatedSample(1, 22050);
    writeDiagnostic("processor: generated buffer allocated");
    auto* samples = generatedSample.getWritePointer(0);
    for (int i = 0; i < generatedSample.getNumSamples(); ++i)
    {
        const auto t = static_cast<double>(i) / 44100.0;
        samples[i] = static_cast<float>(std::sin(juce::MathConstants<double>::twoPi * 440.0 * t) * std::exp(-t * 7.0));
    }

    auto* wavData = new juce::MemoryOutputStream();
    juce::WavAudioFormat wavFormat;
    std::unique_ptr<juce::AudioFormatWriter> writer(wavFormat.createWriterFor(wavData, 44100.0, 1, 16, {}, 0));
    writeDiagnostic(writer != nullptr ? "processor: wav writer created" : "processor: wav writer FAILED");
    if (writer != nullptr)
    {
        writer->writeFromAudioSampleBuffer(generatedSample, 0, generatedSample.getNumSamples());
        {
            auto* input = new juce::MemoryInputStream(wavData->getData(), wavData->getDataSize(), false);
            std::unique_ptr<juce::AudioFormatReader> reader(wavFormat.createReaderFor(input, true));
            writeDiagnostic(reader != nullptr ? "processor: wav reader created" : "processor: wav reader FAILED");
            if (reader != nullptr)
            {
                juce::BigInteger notes;
                notes.setRange(0, 128, true);
                synth.addSound(new juce::SamplerSound("Generated test sample", *reader, notes, 60, 0.01, 0.2, 2.0));
                writeDiagnostic("processor: generated sound added");
            }
        }
        writer.reset();
    }
    else
    {
        delete wavData;
    }
    writeDiagnostic("processor: constructor complete");
}

void MiniSamplerAudioProcessor::writeDiagnostic(const juce::String& message) const
{
    const auto logFile = juce::File::getSpecialLocation(juce::File::userDesktopDirectory).getChildFile("MiniSampler-diagnostics.log");
    logFile.appendText(juce::Time::getCurrentTime().toISO8601(true) + " " + message + "\n", false, false, "UTF-8");
}

void MiniSamplerAudioProcessor::prepareToPlay(double sampleRate, int)
{
    writeDiagnostic("processor: prepareToPlay " + juce::String(sampleRate));
    const juce::ScopedLock lock(synthLock);
    synth.setCurrentPlaybackSampleRate(sampleRate);
}

void MiniSamplerAudioProcessor::releaseResources() {}

bool MiniSamplerAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono()
        && layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;
    return layouts.getMainInputChannelSet().isDisabled();
}

void MiniSamplerAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    static std::atomic<bool> firstProcessBlock { false };
    if (!firstProcessBlock.exchange(true))
        writeDiagnostic("processor: first processBlock channels=" + juce::String(buffer.getNumChannels())
                        + " samples=" + juce::String(buffer.getNumSamples()));
    juce::ScopedNoDenormals noDenormals;
    buffer.clear();
    const juce::ScopedLock lock(synthLock);
    keyboardState.processNextMidiBuffer(midiMessages, 0, buffer.getNumSamples(), true);
    synth.renderNextBlock(buffer, midiMessages, 0, buffer.getNumSamples());
}

bool MiniSamplerAudioProcessor::loadSample(const juce::File& file)
{
    std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(file));
    if (reader == nullptr)
        return false;
    juce::BigInteger notes;
    notes.setRange(0, 128, true);
    auto* newSound = new juce::SamplerSound(file.getFileNameWithoutExtension(), *reader, notes, 60, 0.01, 0.2, 30.0);
    {
        const juce::ScopedLock lock(synthLock);
        synth.clearSounds();
        synth.addSound(newSound);
        loadedSampleName = file.getFileName();
    }
    return true;
}

juce::String MiniSamplerAudioProcessor::getLoadedSampleName() const
{
    const juce::ScopedLock lock(synthLock);
    return loadedSampleName;
}

juce::AudioProcessorEditor* MiniSamplerAudioProcessor::createEditor()
{
    writeDiagnostic("processor: createEditor begin");
    auto* editor = new MiniSamplerAudioProcessorEditor(*this);
    writeDiagnostic("processor: createEditor complete");
    return editor;
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new MiniSamplerAudioProcessor();
}
