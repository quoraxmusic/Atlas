/* Copyright 2013-2019 Matt Tytel
 *
 * pylon is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * pylon is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with pylon.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "synth_editor.h"

#include "synth_plugin.h"
#include "value_bridge.h"
#include "sound_engine.h"
#include "load_save.h"
#include "line_generator.h"
#include "producers_module.h"
#include "sample_source.h"
#include "synth_lfo.h"
#include "synth_oscillator.h"
#include "synth_strings.h"
#include "tuning.h"
#include "utils.h"
#include "formant_filter.h"
#include "wavetable.h"
#include "wavetable_group.h"
#include "wave_source.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <set>

namespace {
  constexpr const char* kPresetSection = "Presets";
  constexpr const char* kSignalRoutingSection = "Signal routing";
  constexpr const char* kEffectsChainSection = "Chain and routing";
  constexpr const char* kAllLibraries = "All libraries";
  constexpr const char* kFactoryLibrary = "Factory";
  constexpr const char* kUserLibrary = "User";
  constexpr const char* kOtherLibrary = "Other";
  constexpr const char* kAllBanks = "All banks";
  constexpr const char* kAllCategories = "All categories";
  constexpr int kBrowserRescanMenuId = 0x3fff0001;

  struct AccessibilitySpeechSettings {
    bool speech_feedback = true;
    bool navigation = false;
    bool presets = false;
    bool lfo_editor = true;
    bool wavetable_editor = false;
    bool sample_browser = false;
    bool modulation = true;
    bool parameters = false;
    bool other = false;
    bool frequency_values_in_hz = false;
  };

  AccessibilitySpeechSettings& accessibilitySpeechSettings() {
    static AccessibilitySpeechSettings settings;
    static bool loaded = false;
    if (!loaded) {
      loaded = true;
      json config = LoadSave::getConfigJson();
      if (config.count("accessibility_speech") && config["accessibility_speech"].is_object()) {
        const json& data = config["accessibility_speech"];
        auto loadBool = [&data](const char* name, bool fallback) {
          return data.count(name) && data[name].is_boolean() ? data[name].get<bool>() : fallback;
        };
        settings.speech_feedback = loadBool("speech_feedback", settings.speech_feedback);
        settings.navigation = loadBool("navigation", settings.navigation);
        settings.presets = loadBool("presets", settings.presets);
        settings.lfo_editor = loadBool("lfo_editor", settings.lfo_editor);
        settings.wavetable_editor = loadBool("wavetable_editor", settings.wavetable_editor);
        settings.sample_browser = loadBool("sample_browser", settings.sample_browser);
        settings.modulation = loadBool("modulation", settings.modulation);
        settings.parameters = loadBool("parameters", settings.parameters);
        settings.other = loadBool("other", settings.other);
        settings.frequency_values_in_hz = loadBool("frequency_values_in_hz", settings.frequency_values_in_hz);
      }
    }
    return settings;
  }

  void saveAccessibilitySpeechSettings() {
    const auto& settings = accessibilitySpeechSettings();
    json config = LoadSave::getConfigJson();
    json data;
    data["speech_feedback"] = settings.speech_feedback;
    data["navigation"] = settings.navigation;
    data["presets"] = settings.presets;
    data["lfo_editor"] = settings.lfo_editor;
    data["wavetable_editor"] = settings.wavetable_editor;
    data["sample_browser"] = settings.sample_browser;
    data["modulation"] = settings.modulation;
    data["parameters"] = settings.parameters;
    data["other"] = settings.other;
    data["frequency_values_in_hz"] = settings.frequency_values_in_hz;
    config["accessibility_speech"] = data;
    LoadSave::saveJsonToConfig(config);
  }

  String onOff(bool enabled) {
    return enabled ? "On" : "Off";
  }

  bool containsAny(const String& text, std::initializer_list<const char*> words) {
    for (const auto* word : words) {
      if (text.contains(word))
        return true;
    }
    return false;
  }

  String compactFloat(float value, int decimals) {
    String result(value, decimals);
    if (result.containsChar('.')) {
      while (result.endsWithChar('0'))
        result = result.dropLastCharacters(1);
      if (result.endsWithChar('.'))
        result = result.dropLastCharacters(1);
    }
    return result;
  }

  String formatHz(float frequency) {
    frequency = std::max(0.0f, frequency);
    const int decimals = frequency < 10.0f ? 2 : (frequency < 100.0f ? 1 : 0);
    return compactFloat(frequency, decimals) + " Hz";
  }

  bool parseNoteNameToMidi(const String& text, float& midi_note) {
    String note = text.trim().removeCharacters(" ");
    if (note.isEmpty())
      return false;

    note = note.toLowerCase();
    if (note.endsWith("hz"))
      return false;

    const juce_wchar root = note[0];
    int semitone = 0;
    switch (root) {
      case 'c': semitone = 0; break;
      case 'd': semitone = 2; break;
      case 'e': semitone = 4; break;
      case 'f': semitone = 5; break;
      case 'g': semitone = 7; break;
      case 'a': semitone = 9; break;
      case 'b': semitone = 11; break;
      default: return false;
    }

    int index = 1;
    if (note.substring(index).startsWith("sharp")) {
      semitone += 1;
      index += 5;
    }
    else if (note.substring(index).startsWith("flat")) {
      semitone -= 1;
      index += 4;
    }
    else if (index < note.length() && note[index] == '#') {
      semitone += 1;
      ++index;
    }
    else if (index < note.length() && note[index] == 'b') {
      semitone -= 1;
      ++index;
    }

    const String octave_text = note.substring(index);
    if (octave_text.isEmpty())
      return false;

    bool valid_octave = true;
    for (int i = 0; i < octave_text.length(); ++i) {
      const juce_wchar c = octave_text[i];
      if (!((c >= '0' && c <= '9') || (i == 0 && c == '-'))) {
        valid_octave = false;
        break;
      }
    }
    if (!valid_octave)
      return false;

    const int octave = octave_text.getIntValue();
    while (semitone < 0)
      semitone += 12;
    semitone %= 12;
    midi_note = static_cast<float>((octave + 1) * 12 + semitone);
    return true;
  }

  bool isSemitoneFrequencyParameter(const String& parameter_id, const vital::ValueDetails& details) {
    if (details.string_lookup != nullptr || details.value_scale != vital::ValueDetails::kLinear)
      return false;

    const String id = parameter_id.toLowerCase();
    const String display = String(details.display_name).toLowerCase();
    const bool frequency_name = id.contains("cutoff") || display.contains("cutoff") ||
                                display.contains("low cut") || display.contains("high cut") ||
                                id.endsWith("_center") || display.endsWith(" center");
    if (!frequency_name)
      return false;

    return details.min >= 0.0f && details.max >= 120.0f && details.max <= 140.0f;
  }

  int oscillatorIndexFromDistortionParameter(const String& parameter_id) {
    if (!parameter_id.startsWith("osc_") || !parameter_id.endsWith("_distortion_type"))
      return -1;

    const String number_text = parameter_id.substring(4).upToFirstOccurrenceOf("_", false, false);
    const int oscillator_index = number_text.getIntValue() - 1;
    return isPositiveAndBelow(oscillator_index, vital::kNumOscillators) ? oscillator_index : -1;
  }

  String oscillatorDistortionValueText(const ValueBridge& bridge, double normalized_value) {
    const int oscillator_index = oscillatorIndexFromDistortionParameter(bridge.getParameterId());
    if (oscillator_index < 0)
      return "";

    const int distortion_type = static_cast<int>(std::round(bridge.convertToEngineValue(
        static_cast<float>(normalized_value))));
    auto oscillator_source_text = [](int source_index) {
      return "Oscillator " + String(source_index + 1);
    };

    switch (distortion_type) {
      case vital::SynthOscillator::kFmOscillatorA:
        return "FM from " + oscillator_source_text(vital::ProducersModule::getFirstModulationIndex(oscillator_index));
      case vital::SynthOscillator::kFmOscillatorB:
        return "FM from " + oscillator_source_text(vital::ProducersModule::getSecondModulationIndex(oscillator_index));
      case vital::SynthOscillator::kFmOscillatorC:
        return "FM from " + oscillator_source_text(vital::ProducersModule::getThirdModulationIndex(oscillator_index));
      case vital::SynthOscillator::kFmSample:
        return "FM from Sample";
      case vital::SynthOscillator::kFmGranular:
        return "FM from Granular";
      case vital::SynthOscillator::kRmOscillatorA:
        return "Ring Mod from " + oscillator_source_text(vital::ProducersModule::getFirstModulationIndex(oscillator_index));
      case vital::SynthOscillator::kRmOscillatorB:
        return "Ring Mod from " + oscillator_source_text(vital::ProducersModule::getSecondModulationIndex(oscillator_index));
      case vital::SynthOscillator::kRmOscillatorC:
        return "Ring Mod from " + oscillator_source_text(vital::ProducersModule::getThirdModulationIndex(oscillator_index));
      case vital::SynthOscillator::kRmSample:
        return "Ring Mod from Sample";
      case vital::SynthOscillator::kRmGranular:
        return "Ring Mod from Granular";
      default:
        return "";
    }
  }

  bool isSampleLoopPointParameter(const String& id) {
    return id == "sample_loop_start" || id == "sample_loop_end";
  }

  String accessibleParameterText(const ValueBridge& bridge, double normalized_value) {
    const String distortion_text = oscillatorDistortionValueText(bridge, normalized_value);
    if (distortion_text.isNotEmpty())
      return distortion_text;

    const auto& settings = accessibilitySpeechSettings();
    if (settings.frequency_values_in_hz &&
        isSemitoneFrequencyParameter(bridge.getParameterId(), bridge.getDetails())) {
      const float midi_note = bridge.convertToEngineValue(static_cast<float>(normalized_value));
      return formatHz(vital::utils::midiNoteToFrequency(midi_note));
    }

    return bridge.getText(static_cast<float>(normalized_value), 128);
  }

  double accessibleParameterValueForText(const ValueBridge& bridge, const String& text) {
    const auto& settings = accessibilitySpeechSettings();
    if (isSemitoneFrequencyParameter(bridge.getParameterId(), bridge.getDetails())) {
      const String trimmed = text.trim();
      if (trimmed.isNotEmpty()) {
        float midi_note = 0.0f;
        if (parseNoteNameToMidi(trimmed, midi_note)) {
          const auto& details = bridge.getDetails();
          return bridge.convertToPluginValue(jlimit(details.min, details.max, midi_note));
        }
        if (settings.frequency_values_in_hz) {
          const float frequency = trimmed.removeCharacters(" ").toLowerCase()
                                         .removeCharacters("hz")
                                         .getFloatValue();
          if (frequency > 0.0f) {
            midi_note = vital::utils::frequencyToMidiNote(frequency);
            const auto& details = bridge.getDetails();
            return bridge.convertToPluginValue(jlimit(details.min, details.max, midi_note));
          }
        }
      }
    }

    return static_cast<double>(bridge.getValueForText(text));
  }

  bool shouldPostPluginAnnouncement(const String& message) {
    const auto& settings = accessibilitySpeechSettings();
    if (!settings.speech_feedback)
      return false;

    const String lower = message.toLowerCase();
    if (containsAny(lower, { "preset", "bank" }))
      return settings.presets;
    if (containsAny(lower, { "lfo", "mseg", "point", "curve", "grid", "shape" }))
      return settings.lfo_editor;
    if (containsAny(lower, { "wavetable", "harmonic", "fundamental", "wave frame", "frame" }) ||
        (lower.startsWith("loaded ") && lower.contains(" in oscillator")) ||
        (lower.startsWith("normalized ") && lower.contains(" frame")))
      return settings.wavetable_editor;
    if (containsAny(lower, { "sample", "granular" }))
      return settings.sample_browser;
    if (containsAny(lower, { "modulation", "midi learn", "assigned", "destination" }))
      return settings.modulation;
    if (containsAny(lower, { "type value", "reset to", "macro" }))
      return settings.parameters;
    if (containsAny(lower, { "oscillator", "mixer", "routing", "filter", "envelope", "effects", "global", "zones" }))
      return settings.navigation;
    return settings.other;
  }

  void postPluginAnnouncement(const String& message, AccessibilityHandler::AnnouncementPriority priority) {
    if (shouldPostPluginAnnouncement(message))
      AccessibilityHandler::postAnnouncement(message, priority);
  }

  void postLfoAnnouncement(const String& message) {
    const auto& settings = accessibilitySpeechSettings();
    if (settings.speech_feedback && settings.lfo_editor)
      AccessibilityHandler::postAnnouncement(message, AccessibilityHandler::AnnouncementPriority::high);
  }

  struct MsegTimeDivision {
    const char* name;
    float quarter_notes;
  };

  const std::vector<MsegTimeDivision>& msegTimeDivisions() {
    static const std::vector<MsegTimeDivision> divisions {
      { "8 Bars", 32.0f }, { "4 Bars", 16.0f }, { "2 Bars", 8.0f }, { "1 Bar", 4.0f },
      { "1/2 Dotted", 3.0f }, { "1/2", 2.0f }, { "1/4 Dotted", 1.5f }, { "1/2 Triplet", 4.0f / 3.0f },
      { "1/4", 1.0f }, { "1/8 Dotted", 0.75f }, { "1/4 Triplet", 2.0f / 3.0f }, { "1/8", 0.5f },
      { "1/16 Dotted", 0.375f }, { "1/8 Triplet", 1.0f / 3.0f }, { "1/16", 0.25f },
      { "1/32 Dotted", 0.1875f }, { "1/16 Triplet", 1.0f / 6.0f }, { "1/32", 0.125f },
      { "1/64 Dotted", 0.09375f }, { "1/32 Triplet", 1.0f / 12.0f }, { "1/64", 0.0625f },
      { "1/128 Dotted", 0.046875f }, { "1/64 Triplet", 1.0f / 24.0f }, { "1/128", 0.03125f }
    };
    return divisions;
  }

  StringArray msegTimeDivisionChoices() {
    StringArray choices;
    for (const auto& division : msegTimeDivisions())
      choices.add(division.name);
    return choices;
  }

  String wavetableAudioLoadStyleName(int audio_load_style) {
    switch (audio_load_style) {
      case WavetableCreator::kVocoded:
        return "vocode";
      case WavetableCreator::kPitched:
        return "pitched";
      case WavetableCreator::kWavetableSplice:
      default:
        return "wavetable splice";
    }
  }

  WaveSource* editableWaveSourceFor(WavetableCreator* creator) {
    if (creator == nullptr || creator->numGroups() == 0)
      return nullptr;

    auto* group = creator->getGroup(0);
    if (group == nullptr || group->numComponents() == 0)
      return nullptr;

    return dynamic_cast<WaveSource*>(group->getComponent(0));
  }

  WaveSource* ensureEditableWaveSource(WavetableCreator* creator) {
    if (auto* existing = editableWaveSourceFor(creator))
      return existing;
    if (creator == nullptr || creator->getWavetable() == nullptr)
      return nullptr;

    auto* wavetable = creator->getWavetable();
    const auto* data = wavetable->getAllData();
    if (data == nullptr || data->num_frames <= 0)
      return nullptr;

    const std::string name = creator->getName();
    const std::string author = creator->getAuthor();
    auto* group = new WavetableGroup();
    auto* source = new WaveSource();
    const int frames = jlimit(1, vital::kNumOscillatorWaveFrames, data->num_frames);

    for (int frame = 0; frame < frames; ++frame) {
      source->insertNewKeyframe(frame);
      auto* wave_frame = source->getKeyframe(frame)->wave_frame();
      std::memcpy(wave_frame->time_domain, data->wave_data[frame],
                  sizeof(vital::mono_float) * vital::WaveFrame::kWaveformSize);
      wave_frame->index = frame;
      wave_frame->setFrequencyRatio(data->frequency_ratio);
      wave_frame->setSampleRate(data->sample_rate);
      wave_frame->toFrequencyDomain();
    }

    source->setInterpolationStyle(WavetableComponent::kLinear);
    group->addComponent(source);
    creator->clear();
    creator->setName(name);
    creator->setAuthor(author);
    creator->addGroup(group);
    creator->render();
    return source;
  }

  int closestVitalTempoIndex(float quarter_notes) {
    if (quarter_notes <= 0.0f)
      return 8;

    const float target = 1.0f / quarter_notes;
    int best = 8;
    float best_distance = std::numeric_limits<float>::max();
    for (int i = 1; i < vital::constants::kNumSyncedFrequencyRatios; ++i) {
      const float distance = std::abs(std::log2(jmax(0.000001f, vital::constants::kSyncedFrequencyRatios[i] / target)));
      if (distance < best_distance) {
        best_distance = distance;
        best = i;
      }
    }
    return best;
  }

  float engineValueFor(AudioProcessorParameter* parameter) {
    if (auto* bridge = dynamic_cast<ValueBridge*>(parameter))
      return bridge->convertToEngineValue(bridge->getValue());
    return parameter ? parameter->getValue() : 0.0f;
  }

  int filterIndexForSection(const String& section_name) {
    if (section_name == "Filter 1")
      return 1;
    if (section_name == "Filter 2")
      return 2;
    if (section_name == "Filter")
      return 3;
    return 0;
  }

  String filterPrefixForSection(const String& section_name) {
    if (section_name.startsWith("Bus ") && section_name.endsWith(" - Filter")) {
      const int bus = section_name.fromFirstOccurrenceOf("Bus ", false, false)
                                  .upToFirstOccurrenceOf(" - Filter", false, false)
                                  .getIntValue();
      if (isPositiveAndBelow(bus - 1, vital::kNumBuses))
        return "bus_" + String(bus) + "_filter_fx_";
    }

    const int index = filterIndexForSection(section_name);
    if (index == 3)
      return "filter_fx_";
    return index > 0 ? "filter_" + String(index) + "_" : String();
  }

  String filterSuffixForParameter(const String& section_name, const String& parameter_id) {
    const String prefix = filterPrefixForSection(section_name);
    if (prefix.isEmpty() || !parameter_id.startsWith(prefix))
      return {};
    return parameter_id.fromFirstOccurrenceOf(prefix, false, false);
  }

  bool isFilterParameterSuffix(const String& suffix) {
    static const std::array<const char*, 22> allowed_suffixes = {{
      "mix",
      "cutoff",
      "resonance",
      "drive",
      "blend",
      "style",
      "model",
      "on",
      "blend_transpose",
      "keytrack",
      "formant_x",
      "formant_y",
      "formant_transpose",
      "formant_resonance",
      "formant_spread",
      "osc1_input",
      "osc2_input",
      "osc3_input",
      "osc4_input",
      "sample_input",
      "granular_input",
      "filter_input",
    }};
    for (const auto* allowed : allowed_suffixes) {
      if (suffix == allowed)
        return true;
    }
    return false;
  }

  int numFilterStylesForModel(int model) {
    switch (static_cast<vital::constants::FilterModel>(model)) {
      case vital::constants::kAnalog:
      case vital::constants::kDirty:
      case vital::constants::kLadder:
      case vital::constants::kDigital:
        return 5;
      case vital::constants::kDiode:
      case vital::constants::kFormant:
      case vital::constants::kPhase:
        return 2;
      case vital::constants::kComb:
        return 6;
      default:
        return 1;
    }
  }

  String filterStyleNameForModel(int model, int style) {
    style = jlimit(0, jmax(0, numFilterStylesForModel(model) - 1), style);
    switch (static_cast<vital::constants::FilterModel>(model)) {
      case vital::constants::kAnalog:
      case vital::constants::kDirty:
      case vital::constants::kLadder:
      case vital::constants::kDigital:
        return strings::kFilterStyleNames[style];
      case vital::constants::kDiode:
        return strings::kDiodeStyleNames[style];
      case vital::constants::kFormant:
        if (style == vital::FormantFilter::kVocalTract)
          return "The Mouth";
        if (style == vital::FormantFilter::kAIUO)
          return "AIUO";
        return "AOIE";
      case vital::constants::kComb:
        return strings::kCombStyleNames[style];
      case vital::constants::kPhase:
        return style ? "Negative" : "Positive";
      default:
        return {};
    }
  }

  String filterBlendText(int model, double normalized_value) {
    const double engine_value = jlimit(0.0, 1.0, normalized_value) * 2.0;
    if (model == vital::constants::kComb) {
      if (engine_value <= 0.05)
        return "Low comb";
      if (engine_value >= 1.95)
        return "High comb";
      return "Low to high comb, " + String(engine_value, 2);
    }
    if (model == vital::constants::kPhase)
      return "Phaser blend, " + String(engine_value, 2);

    if (engine_value <= 0.05)
      return "Low pass";
    if (std::abs(engine_value - 1.0) <= 0.05)
      return "Band pass";
    if (engine_value >= 1.95)
      return "High pass";
    if (engine_value < 1.0)
      return "Between low pass and band pass, " + String(engine_value, 2);
    return "Between band pass and high pass, " + String(engine_value, 2);
  }

  String last_section_name;
  String last_preset_path;
  String last_preset_search;
  String last_preset_library = kAllLibraries;
  String last_preset_bank = kAllBanks;
  String last_preset_category = kAllCategories;
  bool last_preset_preview = false;

  force_inline int chunkNameToData(const char* chunk_name) {
    return static_cast<int>(ByteOrder::littleEndianInt(chunk_name));
  }

  String getWavetableDataString(InputStream& input_stream) {
    input_stream.setPosition(0);
    int first_chunk = input_stream.readInt();
    if (first_chunk != chunkNameToData("RIFF"))
      return "";

    int length = input_stream.readInt();
    int data_end = static_cast<int>(input_stream.getPosition()) + length;

    if (input_stream.readInt() != chunkNameToData("WAVE"))
      return "";

    while (!input_stream.isExhausted() && input_stream.getPosition() < data_end) {
      int chunk_label = input_stream.readInt();
      int chunk_length = input_stream.readInt();

      if (chunk_label == chunkNameToData("clm ")) {
        MemoryBlock memory_block;
        input_stream.readIntoMemoryBlock(memory_block, chunk_length);
        return memory_block.toString();
      }

      input_stream.setPosition(input_stream.getPosition() + chunk_length);
    }

    return "";
  }

  FileSource::FadeStyle getFadeStyleFromWavetableString(String data) {
    if (data.isEmpty())
      return FileSource::kFreqInterpolate;

    if (data.substring(0, 3) == "<!>") {
      StringArray tokens;
      tokens.addTokens(data.substring(3), " ", "");
      if (tokens.size() < 2 || tokens[1].isEmpty())
        return FileSource::kFreqInterpolate;

      char fade_character = tokens[1][0];
      if (fade_character == '0')
        return FileSource::kNoInterpolate;
      if (fade_character == '1')
        return FileSource::kTimeInterpolate;
    }

    return FileSource::kFreqInterpolate;
  }

  String getAuthorFromWavetableString(String data) {
    if (data.substring(0, 3) == "<!>") {
      int start = data.indexOf("[");
      int end = data.indexOf("]");
      if (start < end && start >= 0)
        return data.substring(start + 1, end);
    }

    return "";
  }

  int loadAudioFile(AudioSampleBuffer& destination, InputStream* audio_stream) {
    AudioFormatManager format_manager;
    format_manager.registerBasicFormats();

    audio_stream->setPosition(0);
    std::unique_ptr<AudioFormatReader> format_reader(
        format_manager.createReaderFor(std::unique_ptr<InputStream>(audio_stream)));
    if (format_reader == nullptr)
      return 0;

    int num_samples = static_cast<int>(format_reader->lengthInSamples);
    destination.setSize(format_reader->numChannels, num_samples);
    format_reader->read(&destination, 0, num_samples, 0, true, true);
    return static_cast<int>(format_reader->sampleRate);
  }

  struct BrowserNode {
    std::map<String, std::unique_ptr<BrowserNode>> folders;
    Array<File> files;
  };

  String browserRootName(const File& root, const String& folder_name) {
    const File data_folder = LoadSave::getDataDirectory().getChildFile(folder_name);
    if (root == data_folder)
      return "Factory";

    const File user_folder = LoadSave::getUserDirectory().getChildFile(folder_name);
    if (root == user_folder)
      return "User";

    return root.getFileName().isNotEmpty() ? root.getFileName() : root.getFullPathName();
  }

  void addBrowserFile(BrowserNode& root, const File& base, const File& file) {
    StringArray parts;
    parts.addTokens(file.getRelativePathFrom(base), File::getSeparatorString(), "");
    if (parts.isEmpty())
      return;

    BrowserNode* node = &root;
    for (int i = 0; i < parts.size() - 1; ++i) {
      const String folder = parts[i];
      if (!node->folders.count(folder))
        node->folders[folder] = std::make_unique<BrowserNode>();
      node = node->folders[folder].get();
    }
    node->files.add(file);
  }

  void sortBrowserNode(BrowserNode& node) {
    node.files.sort();
    for (auto& folder : node.folders)
      sortBrowserNode(*folder.second);
  }

  bool addBrowserNodeToMenu(PopupMenu& menu, const BrowserNode& node,
                            const std::shared_ptr<std::vector<File>>& choices, int& item_id) {
    bool has_items = false;
    for (const auto& folder : node.folders) {
      PopupMenu sub_menu;
      if (addBrowserNodeToMenu(sub_menu, *folder.second, choices, item_id)) {
        menu.addSubMenu(folder.first, sub_menu, true);
        has_items = true;
      }
    }

    for (const auto& file : node.files) {
      choices->push_back(file);
      menu.addItem(item_id++, file.getFileNameWithoutExtension());
      has_items = true;
    }

    return has_items;
  }

  bool browserFileBelongsToRoot(const File& file, const File& root) {
    const String root_path = root.getFullPathName();
    const String file_path = file.getFullPathName();
    return file_path == root_path || file_path.startsWith(root_path + File::getSeparatorString());
  }

  void refreshBrowserFileCache(std::vector<File>& cache, const std::vector<File>& roots, const String& wildcard) {
    cache.clear();
    for (const auto& root : roots) {
      if (!root.isDirectory())
        continue;

      Array<File> files;
      root.findChildFiles(files, File::findFiles, true, wildcard);
      cache.reserve(cache.size() + static_cast<size_t>(files.size()));
      for (const auto& file : files)
        cache.push_back(file);
    }
  }

  std::vector<File> browserRootsWithAdditionalFolders(const String& folder_name,
                                                      const std::string& additional_folders_name) {
    auto roots = LoadSave::getDirectories(folder_name);
    for (const auto& path : LoadSave::getAdditionalFolders(additional_folders_name))
      roots.push_back(File(path));
    return roots;
  }

  File shiftedBrowserFile(const std::vector<File>& browser_files, const File& current_file, int shift) {
    if (browser_files.empty())
      return File();

    Array<File> sorted_files;
    for (const auto& file : browser_files)
      sorted_files.add(file);

    LoadSave::FileSorterAscending file_sorter;
    sorted_files.sort(file_sorter);

    const int index = sorted_files.indexOf(current_file);
    if (index < 0)
      return sorted_files[0];
    return sorted_files[(index + shift + sorted_files.size()) % sorted_files.size()];
  }

  PopupMenu createFileBrowserMenu(const std::vector<File>& roots, const String& folder_name,
                                  const std::vector<File>& browser_files,
                                  const std::shared_ptr<std::vector<File>>& choices) {
    PopupMenu menu;
    int item_id = 1;

    for (const auto& root : roots) {
      if (!root.isDirectory())
        continue;

      BrowserNode root_node;
      for (const auto& file : browser_files) {
        if (browserFileBelongsToRoot(file, root))
          addBrowserFile(root_node, root, file);
      }
      sortBrowserNode(root_node);

      PopupMenu root_menu;
      if (addBrowserNodeToMenu(root_menu, root_node, choices, item_id))
        menu.addSubMenu(browserRootName(root, folder_name), root_menu, true);
    }

    if (choices->empty())
      menu.addItem(1, "No files found", false);
    return menu;
  }

  int indexOfPathPart(const StringArray& parts, const String& name) {
    for (int i = 0; i < parts.size(); ++i) {
      if (parts[i] == name)
        return i;
    }
    return -1;
  }

  StringArray presetPathParts(const File& preset) {
    StringArray parts;
    parts.addTokens(preset.getRelativePathFrom(LoadSave::getDataDirectory()), File::getSeparatorString(), "");
    return parts;
  }

  String sanitizePresetPathPart(String text, const String& fallback) {
    text = text.trim().removeCharacters("\\/:*?\"<>|");
    while (text.contains(".."))
      text = text.replace("..", ".");
    if (text.isEmpty())
      text = fallback;
    return text;
  }

  String presetLibraryName(const File& preset) {
    const auto parts = presetPathParts(preset);
    if (parts.size() > 0) {
      if (parts[0] == kFactoryLibrary)
        return kFactoryLibrary;
      if (parts[0] == kUserLibrary)
        return kUserLibrary;
    }
    return kOtherLibrary;
  }

  String presetBankName(const File& preset) {
    const auto parts = presetPathParts(preset);
    const int preset_folder = indexOfPathPart(parts, LoadSave::kPresetFolderName);
    if (preset_folder >= 0 && preset_folder + 1 < parts.size() - 1 &&
        preset_folder == 1 && (parts[0] == kFactoryLibrary || parts[0] == kUserLibrary)) {
      return parts[preset_folder + 1];
    }
    if (preset_folder == 0 && preset_folder + 1 < parts.size() - 1)
      return parts[preset_folder + 1];
    if (preset_folder > 0)
      return parts[preset_folder - 1];
    return preset.getParentDirectory().getFileName();
  }

  String presetPathCategoryName(const File& preset) {
    const auto parts = presetPathParts(preset);
    const int preset_folder = indexOfPathPart(parts, LoadSave::kPresetFolderName);
    int category_start = preset_folder + 1;
    if (preset_folder >= 0 && preset_folder == 1 && (parts[0] == kFactoryLibrary || parts[0] == kUserLibrary))
      category_start = preset_folder + 2;
    else if (preset_folder == 0)
      category_start = preset_folder + 2;

    if (preset_folder >= 0 && category_start < parts.size() - 1) {
      StringArray category_parts;
      for (int i = category_start; i < parts.size() - 1; ++i)
        category_parts.add(parts[i]);
      return category_parts.joinIntoString(" / ");
    }

    return {};
  }

  String presetCategoryName(const File& preset) {
    const String path_category = presetPathCategoryName(preset);
    if (path_category.isNotEmpty())
      return path_category;

    const String style = LoadSave::getStyleFromFile(preset);
    if (style.isNotEmpty())
      return style;

    return "Uncategorized";
  }

  StringArray presetTagNames(const File& preset) {
    StringArray tags;
    tags.addTokens(LoadSave::getTagsFromFile(preset), ",", "");
    for (int i = tags.size() - 1; i >= 0; --i) {
      tags.set(i, tags[i].trim());
      if (tags[i].isEmpty())
        tags.remove(i);
    }
    tags.removeDuplicates(false);
    return tags;
  }

  PresetBrowserItem buildPresetBrowserItem(const File& preset, const File& data_directory) {
    PresetBrowserItem item;
    item.file = preset;
    item.file_size = preset.getSize();
    item.modified_time = preset.getLastModificationTime().toMilliseconds();
    item.library = presetLibraryName(preset);
    item.bank = presetBankName(preset);
    item.style = LoadSave::getStyleFromFile(preset);
    const String path_category = presetPathCategoryName(preset);
    item.category = path_category.isNotEmpty() ? path_category
                                               : item.style.isNotEmpty() ? item.style : "Uncategorized";
    item.tags = presetTagNames(preset);
    item.author = LoadSave::getAuthorFromFile(preset);
    item.label = preset.getFileNameWithoutExtension() + " - " + item.library + " - " + item.bank;
    if (item.category.isNotEmpty() && item.category != "Uncategorized")
      item.label += " - " + item.category;
    item.searchable = (preset.getFileNameWithoutExtension() + " " +
                       item.library + " " + item.bank + " " + item.category + " " +
                       item.tags.joinIntoString(" ") + " " +
                       preset.getRelativePathFrom(data_directory) + " " +
                       item.author + " " + item.style).toLowerCase();
    return item;
  }

  constexpr int kPresetIndexSchemaVersion = 1;

  File presetIndexDatabaseFile() {
    return LoadSave::getDataDirectory().getChildFile("AtlasPresetIndex.db");
  }

  String presetPathKey(const File& file) {
    return file.getFullPathName();
  }

  bool presetFileStampMatches(const PresetBrowserItem& item, const File& file) {
    return item.file_size == file.getSize() &&
           item.modified_time == file.getLastModificationTime().toMilliseconds();
  }

  json stringArrayToJson(const StringArray& values) {
    json result = json::array();
    for (const auto& value : values)
      result.push_back(value.toStdString());
    return result;
  }

  StringArray stringArrayFromJson(const json& data) {
    StringArray result;
    if (!data.is_array())
      return result;
    for (const auto& value : data) {
      if (value.is_string())
        result.add(String(value.get<std::string>()));
    }
    result.removeDuplicates(false);
    return result;
  }

  json presetBrowserItemToJson(const PresetBrowserItem& item) {
    json data;
    data["path"] = item.file.getFullPathName().toStdString();
    data["size"] = item.file_size;
    data["modified"] = item.modified_time;
    data["library"] = item.library.toStdString();
    data["bank"] = item.bank.toStdString();
    data["category"] = item.category.toStdString();
    data["tags"] = stringArrayToJson(item.tags);
    data["author"] = item.author.toStdString();
    data["style"] = item.style.toStdString();
    data["label"] = item.label.toStdString();
    data["searchable"] = item.searchable.toStdString();
    return data;
  }

  bool presetBrowserItemFromJson(const json& data, PresetBrowserItem& item) {
    if (!data.is_object() || !data.count("path") || !data["path"].is_string())
      return false;

    item.file = File(String(data["path"].get<std::string>()));
    item.file_size = data.value("size", static_cast<int64>(0));
    item.modified_time = data.value("modified", static_cast<int64>(0));
    item.library = String(data.value("library", std::string()));
    item.bank = String(data.value("bank", std::string()));
    item.category = String(data.value("category", std::string("Uncategorized")));
    item.tags = data.count("tags") ? stringArrayFromJson(data["tags"]) : StringArray();
    item.author = String(data.value("author", std::string()));
    item.style = String(data.value("style", std::string()));
    item.label = String(data.value("label", std::string()));
    item.searchable = String(data.value("searchable", std::string()));

    if (item.label.isEmpty()) {
      item.label = item.file.getFileNameWithoutExtension() + " - " + item.library + " - " + item.bank;
      if (item.category.isNotEmpty() && item.category != "Uncategorized")
        item.label += " - " + item.category;
    }
    if (item.searchable.isEmpty()) {
      item.searchable = (item.file.getFileNameWithoutExtension() + " " +
                         item.library + " " + item.bank + " " + item.category + " " +
                         item.tags.joinIntoString(" ") + " " + item.author + " " + item.style).toLowerCase();
    }
    return true;
  }

  std::map<std::string, PresetBrowserItem> loadPresetIndexDatabase() {
    std::map<std::string, PresetBrowserItem> items;
    const File database = presetIndexDatabaseFile();
    if (!database.existsAsFile())
      return items;

    try {
      const json data = json::parse(database.loadFileAsString().toStdString(), nullptr, false);
      if (data.is_discarded() || !data.is_object() ||
          data.value("schema", 0) != kPresetIndexSchemaVersion ||
          !data.count("presets") || !data["presets"].is_array())
        return items;

      for (const auto& entry : data["presets"]) {
        PresetBrowserItem item;
        if (presetBrowserItemFromJson(entry, item))
          items[presetPathKey(item.file).toStdString()] = item;
      }
    }
    catch (const json::exception&) {
      return {};
    }
    return items;
  }

  void savePresetIndexDatabase(const std::vector<PresetBrowserItem>& items) {
    File database = presetIndexDatabaseFile();
    database.getParentDirectory().createDirectory();

    json data;
    data["schema"] = kPresetIndexSchemaVersion;
    data["data_directory"] = LoadSave::getDataDirectory().getFullPathName().toStdString();
    data["updated"] = Time::getCurrentTime().toMilliseconds();
    data["presets"] = json::array();
    for (const auto& item : items)
      data["presets"].push_back(presetBrowserItemToJson(item));

    database.replaceWithText(String(data.dump(2)));
  }

  CriticalSection& presetBrowserCacheLock() {
    static CriticalSection lock;
    return lock;
  }

  std::vector<PresetBrowserItem>& cachedPresetBrowserItems() {
    static std::vector<PresetBrowserItem> items;
    return items;
  }

  bool& cachedPresetBrowserItemsValid() {
    static bool valid = false;
    return valid;
  }

  bool isRoutingParameter(const String& id) {
    if (id == "effect_chain_order" || id == "post_effect_order" ||
        id == "filter_1_destination" || id == "filter_2_destination")
      return true;
    if (id.startsWith("effect_chain_slot_"))
      return true;
    if (id.startsWith("bus_") && id.endsWith("_destination"))
      return true;
    if (id.startsWith("bus_") && (id.endsWith("effect_chain_order") || id.endsWith("post_effect_order")))
      return true;
    if (id.startsWith("bus_") && id.contains("_effect_chain_slot_"))
      return true;
    if (id.startsWith("filter_") &&
        (id.endsWith("_osc1_input") || id.endsWith("_osc2_input") ||
         id.endsWith("_osc3_input") || id.endsWith("_osc4_input") || id.endsWith("_sample_input") ||
         id.endsWith("_filter_input")))
      return true;
    return false;
  }

  int sectionRank(const String& section) {
    static const std::array<const char*, 54> order = {{
      "Master and global",
      kPresetSection,
      "Voice and performance",
      kSignalRoutingSection,
      "Oscillator 1",
      "Oscillator 2",
      "Oscillator 3",
      "Oscillator 4",
      "Sample",
      "Granular",
      "Filter 1",
      "Filter 2",
      kEffectsChainSection,
      "Distortion",
      "Filter",
      "Chorus",
      "Flanger",
      "Phaser",
      "Delay",
      "Reverb",
      "Utility",
      "Phase Shift",
      "Equalizer",
      "Compressor",
      "Limiter",
      "Frequency Shifter",
      "Dimension Expander",
      "Envelope 1",
      "Envelope 2",
      "Envelope 3",
      "Envelope 4",
      "Envelope 5",
      "Envelope 6",
      "LFO 1",
      "LFO 2",
      "LFO 3",
      "LFO 4",
      "LFO 5",
      "LFO 6",
      "LFO 7",
      "LFO 8",
      "Random 1",
      "Random 2",
      "Random 3",
      "Random 4",
      "Macros",
      "Modulation routing",
      "Zones - Oscillator 1",
      "Zones - Oscillator 2",
      "Zones - Oscillator 3",
      "Zones - Oscillator 4",
      "Zones - Sample",
      "Zones - Granular",
      "Other"
    }};

    for (int i = 0; i < static_cast<int>(order.size()); ++i) {
      if (section == order[i])
        return i;
    }
    if (section.startsWith("Bus 1 - "))
      return 220 + sectionRank(section.fromFirstOccurrenceOf("Bus 1 - ", false, false));
    if (section.startsWith("Bus 2 - "))
      return 250 + sectionRank(section.fromFirstOccurrenceOf("Bus 2 - ", false, false));
    if (section.startsWith("Bus 3 - "))
      return 280 + sectionRank(section.fromFirstOccurrenceOf("Bus 3 - ", false, false));
    return static_cast<int>(order.size());
  }

  String groupForSection(const String& section) {
    if (section == kPresetSection)
      return "Presets";
    if (section.startsWith("Oscillator") || section == "Sample" || section == "Granular")
      return "Oscillators";
    if (section == "Voice and performance" || section == kSignalRoutingSection)
      return "Mixer and routing";
    if (section.startsWith("Filter "))
      return "Filters";
    if (section.startsWith("Bus 1 - "))
      return "Bus 1";
    if (section.startsWith("Bus 2 - "))
      return "Bus 2";
    if (section.startsWith("Bus 3 - "))
      return "Bus 3";
    if (section.startsWith("Envelope "))
      return "Envelopes";
    if (section.startsWith("LFO "))
      return "LFOs";
    if (section.startsWith("Random "))
      return "Random modulation";
    if (section == "Macros")
      return "Macros";
    if (section.startsWith("Zones - "))
      return "Zones";
    if (section == kEffectsChainSection || section == "Distortion" || section == "Filter" ||
        section == "Chorus" || section == "Flanger" || section == "Phaser" ||
        section == "Delay" || section == "Reverb" || section == "Utility" || section == "Phase Shift" ||
        section == "Equalizer" || section == "Compressor" || section == "Limiter" ||
        section == "Frequency Shifter" || section == "Dimension Expander")
      return "Effects";
    if (section == "Modulation routing")
      return "Modulation";
    return "Global";
  }

  int groupRank(const String& group) {
    static const std::array<const char*, 15> order = {{
      "Presets",
      "Oscillators",
      "Mixer and routing",
      "Filters",
      "Bus 1",
      "Bus 2",
      "Bus 3",
      "Envelopes",
      "LFOs",
      "Random modulation",
      "Macros",
      "Effects",
      "Modulation",
      "Zones",
      "Global"
    }};

    for (int i = 0; i < static_cast<int>(order.size()); ++i) {
      if (group == order[i])
        return i;
    }
    return static_cast<int>(order.size());
  }

  int modulationSlotForParameterId(const String& id);
  int modulationParameterSuffixRank(const String& id);
  int macroIndexForControlId(const String& id);
  bool isMacroBipolarParameterId(const String& id);
  String macroBipolarParameterId(int macroIndex);

  int parameterRank(const String& id) {
    String rank_id = id;
    for (int bus = 1; bus <= vital::kNumBuses; ++bus) {
      const String prefix = "bus_" + String(bus) + "_";
      if (rank_id.startsWith(prefix)) {
        rank_id = rank_id.fromFirstOccurrenceOf(prefix, false, false);
        break;
      }
    }

    auto oscillator_suffix_rank = [](const String& suffix) {
      static const std::array<const char*, 35> oscillator_order = {{
        "on",
        "wave_frame",
        "transpose",
        "tune",
        "level",
        "pan",
        "destination",
        "unison_voices",
        "unison_detune",
        "unison_blend",
        "stack_style",
        "detune_power",
        "detune_range",
        "spectral_unison",
        "frame_spread",
        "stereo_spread",
        "phase",
        "random_phase",
        "midi_track",
        "smooth_interpolation",
        "distortion_type",
        "distortion_amount",
        "distortion_spread",
        "spectral_morph_type",
        "spectral_morph_amount",
        "spectral_morph_spread",
        "transpose_quantize_key",
        "transpose_quantize_scale",
        "transpose_quantize_mode",
        "transpose_quantize",
        "key_zone_start",
        "key_zone_end",
        "velocity_zone_start",
        "velocity_zone_end",
        "view_2d"
      }};
      for (int i = 0; i < static_cast<int>(oscillator_order.size()); ++i) {
        if (suffix == oscillator_order[i])
          return 100 + i;
      }
      return 500;
    };

    for (int osc = 1; osc <= vital::kNumOscillators; ++osc) {
      const String prefix = "osc_" + String(osc) + "_";
      if (id.startsWith(prefix))
        return osc * 1000 + oscillator_suffix_rank(id.fromFirstOccurrenceOf(prefix, false, false));
    }

    if (id.startsWith("sample_")) {
      const String suffix = id.fromFirstOccurrenceOf("sample_", false, false);
      static const std::array<const char*, 27> sample_order = {{
        "on",
        "destination",
        "playback_mode",
        "loop",
        "bounce",
        "random_phase",
        "keytrack",
        "root_key",
        "start",
        "end",
        "loop_start",
        "loop_end",
        "loop_crossfade",
        "low_cutoff",
        "high_cutoff",
        "transpose",
        "tune",
        "transpose_quantize_key",
        "transpose_quantize_scale",
        "transpose_quantize_mode",
        "transpose_quantize",
        "key_zone_start",
        "key_zone_end",
        "velocity_zone_start",
        "velocity_zone_end",
        "level",
        "pan"
      }};
      for (int i = 0; i < static_cast<int>(sample_order.size()); ++i) {
        if (suffix == sample_order[i])
          return 4500 + i;
      }
      if (suffix == "level")
        return 4525;
      if (suffix == "pan")
        return 4526;
      return 4590;
    }

    if (id.startsWith("granular_")) {
      const String suffix = id.fromFirstOccurrenceOf("granular_", false, false);
      static const std::array<const char*, 36> granular_order = {{
        "on",
        "destination",
        "mode",
        "keytrack",
        "root_key",
        "grain_count",
        "density",
        "midi_density",
        "grain_size",
        "speed",
        "position",
        "position_mod",
        "position_mod_rate",
        "random_position",
        "random_volume",
        "random_pan",
        "random_pitch",
        "interval",
        "interval_chance",
        "direction",
        "start",
        "end",
        "low_cutoff",
        "high_cutoff",
        "transpose",
        "tune",
        "transpose_quantize_key",
        "transpose_quantize_scale",
        "transpose_quantize_mode",
        "transpose_quantize",
        "key_zone_start",
        "key_zone_end",
        "velocity_zone_start",
        "velocity_zone_end",
        "level",
        "pan"
      }};
      for (int i = 0; i < static_cast<int>(granular_order.size()); ++i) {
        if (suffix == granular_order[i])
          return 4600 + i;
      }
      return 4690;
    }

    for (int random = 1; random <= vital::kNumRandomLfos; ++random) {
      const String prefix = "random_" + String(random) + "_";
      if (!id.startsWith(prefix))
        continue;

      const String suffix = id.fromFirstOccurrenceOf(prefix, false, false);
      static const std::array<const char*, 8> random_order = {{
        "style",
        "frequency",
        "rate_x10",
        "sync",
        "tempo",
        "sync_type",
        "stereo",
        "keytrack_transpose",
      }};
      for (int i = 0; i < static_cast<int>(random_order.size()); ++i) {
        if (suffix == random_order[i])
          return 7000 + random * 100 + i;
      }
      if (suffix == "keytrack_tune")
        return 7000 + random * 100 + 8;
      return 7000 + random * 100 + 90;
    }

    const int modulation_slot = modulationSlotForParameterId(id);
    if (modulation_slot > 0)
      return 200000 + modulation_slot * 10 + modulationParameterSuffixRank(id);

    const int macro_index = macroIndexForControlId(id);
    if (macro_index >= 0)
      return 10000 + macro_index;
    if (isMacroBipolarParameterId(id))
      return 11000 + id.fromFirstOccurrenceOf("macro_bipolar_", false, false).getIntValue();

    static const std::array<const char*, 49> priority = {{
      "volume",
      "polyphony",
      "oversampling",
      "osc_1_on",
      "osc_1_destination",
      "osc_2_on",
      "osc_2_destination",
      "osc_3_on",
      "osc_3_destination",
      "osc_4_on",
      "osc_4_destination",
      "sample_on",
      "sample_destination",
      "granular_on",
      "granular_destination",
      "filter_1_on",
      "filter_1_filter_input",
      "filter_1_osc1_input",
      "filter_1_osc2_input",
      "filter_1_osc3_input",
      "filter_1_osc4_input",
      "filter_1_sample_input",
      "filter_2_on",
      "filter_2_filter_input",
      "filter_2_osc1_input",
      "filter_2_osc2_input",
      "filter_2_osc3_input",
      "filter_2_osc4_input",
      "filter_2_sample_input",
      "effect_chain_order",
      "post_effect_order",
      "destination",
      "distortion_on",
      "filter_fx_on",
      "chorus_on",
      "flanger_on",
      "phaser_on",
      "delay_on",
      "reverb_on",
      "utility_on",
      "phase_shift_on",
      "eq_on",
      "compressor_on",
      "limiter_on",
      "frequency_shifter_on",
      "dimension_expander_on",
      "filter_1_model",
      "filter_1_style",
      "filter_2_model"
    }};

    for (int i = 0; i < static_cast<int>(priority.size()); ++i) {
      if (rank_id == priority[i])
        return i;
    }
    return 1000;
  }

  // Tiebreak key used when ordering a section's accessible rows. Parameters with the
  // same rank fall back to this; mapping zone_crossfade onto a key that begins with
  // "portamento_glide_zones" keeps it directly after the glide zone crossing toggle.
  String sectionSortId(const String& id) {
    if (id == "zone_crossfade")
      return "portamento_glide_zones_crossfade";
    return id;
  }

  String readableId(String id) {
    return id.replaceCharacter('_', ' ').toLowerCase().substring(0, 1).toUpperCase()
           + id.replaceCharacter('_', ' ').toLowerCase().substring(1);
  }

  String effectName(int effect) {
    if (isPositiveAndBelow(effect, vital::constants::kNumEffects))
      return readableId(strings::kEffectOrder[effect]);
    return {};
  }

  String effectNameForParameterPrefix(const String& prefix) {
    if (prefix == "filter_fx")
      return "Filter";
    return readableId(prefix);
  }

  String effectSectionForPrefixedId(const String& id, const String& section_prefix) {
    const String rest = id.fromFirstOccurrenceOf(section_prefix, false, false);
    if (rest == "destination")
      return "Chain and routing";
    if (rest == "effect_chain_order" || rest == "post_effect_order" || rest.startsWith("effect_chain_slot_"))
      return "Chain and routing";
    if (rest.startsWith("chorus_")) return "Chorus";
    if (rest.startsWith("compressor_")) return "Compressor";
    if (rest.startsWith("delay_")) return "Delay";
    if (rest.startsWith("dimension_expander_")) return "Dimension Expander";
    if (rest.startsWith("distortion_")) return "Distortion";
    if (rest.startsWith("eq_")) return "Equalizer";
    if (rest.startsWith("flanger_")) return "Flanger";
    if (rest.startsWith("filter_fx_")) return "Filter";
    if (rest.startsWith("frequency_shifter_")) return "Frequency Shifter";
    if (rest.startsWith("limiter_")) return "Limiter";
    if (rest.startsWith("phase_shift_")) return "Phase Shift";
    if (rest.startsWith("phaser_")) return "Phaser";
    if (rest.startsWith("reverb_")) return "Reverb";
    if (rest.startsWith("utility_")) return "Utility";
    return {};
  }

  String busEffectSectionForParameter(const String& id) {
    for (int bus = 1; bus <= vital::kNumBuses; ++bus) {
      const String prefix = "bus_" + String(bus) + "_";
      if (!id.startsWith(prefix))
        continue;

      const String effect_section = effectSectionForPrefixedId(id, prefix);
      if (effect_section.isNotEmpty())
        return "Bus " + String(bus) + " - " + effect_section;
    }
    return {};
  }

  bool isEffectChainSection(const String& section) {
    if (section == kEffectsChainSection)
      return true;
    for (int bus = 1; bus <= vital::kNumBuses; ++bus) {
      if (section == "Bus " + String(bus) + " - Chain and routing")
        return true;
    }
    return false;
  }

  String effectChainPrefixForSection(const String& section) {
    for (int bus = 1; bus <= vital::kNumBuses; ++bus) {
      const String prefix = "Bus " + String(bus) + " - ";
      if (section.startsWith(prefix))
        return "bus_" + String(bus) + "_";
    }
    return {};
  }

  String accessibleSectionTitle(const String& section) {
    for (int bus = 1; bus <= vital::kNumBuses; ++bus) {
      const String prefix = "Bus " + String(bus) + " - ";
      if (section.startsWith(prefix))
        return section.fromFirstOccurrenceOf(prefix, false, false);
    }
    return section;
  }

  String accessibleParameterTitle(const String& section, String name) {
    for (int bus = 1; bus <= vital::kNumBuses; ++bus) {
      const String section_prefix = "Bus " + String(bus) + " - ";
      const String name_prefix = "Bus " + String(bus) + " ";
      if (section.startsWith(section_prefix) && name.startsWith(name_prefix))
        return name.fromFirstOccurrenceOf(name_prefix, false, false);
    }
    return name;
  }

  String effectIdForSection(const String& section) {
    const String effect_section = accessibleSectionTitle(section);
    for (int i = 0; i < vital::constants::kNumEffects; ++i) {
      const String effect_id = strings::kEffectOrder[i];
      if (effectSectionForPrefixedId(effect_id + "_on", {}) == effect_section)
        return effect_id;
    }
    return {};
  }

  String effectPresetDisplayName(const String& effect_id) {
    return effectSectionForPrefixedId(effect_id + "_on", {});
  }

  String bankRelativePathForFile(const File& file, const File& source_folder, const String& folder_name) {
    StringArray parts;
    parts.addTokens(file.getRelativePathFrom(source_folder), File::getSeparatorString(), "");
    const int folder_index = indexOfPathPart(parts, folder_name);
    if (folder_index >= 0 && folder_index + 1 < parts.size()) {
      StringArray stripped;
      for (int i = folder_index + 1; i < parts.size(); ++i)
        stripped.add(parts[i]);
      return stripped.joinIntoString("/");
    }
    return parts.joinIntoString("/");
  }

  bool pathContainsFolderName(const File& file, const File& source_folder, const String& folder_name) {
    StringArray parts;
    parts.addTokens(file.getRelativePathFrom(source_folder), File::getSeparatorString(), "");
    return indexOfPathPart(parts, folder_name) >= 0;
  }

  String sectionForParameter(const String& id) {
    const String bus_section = busEffectSectionForParameter(id);
    if (bus_section.isNotEmpty())
      return bus_section;
    for (int osc = 1; osc <= vital::kNumOscillators; ++osc) {
      const String prefix = "osc_" + String(osc) + "_";
      if (id.startsWith(prefix) &&
          (id.endsWith("_key_zone_start") || id.endsWith("_key_zone_end") ||
           id.endsWith("_velocity_zone_start") || id.endsWith("_velocity_zone_end"))) {
        return "Zones - Oscillator " + String(osc);
      }
    }
    if (id == "sample_key_zone_start" || id == "sample_key_zone_end" ||
        id == "sample_velocity_zone_start" || id == "sample_velocity_zone_end")
      return "Zones - Sample";
    if (id == "granular_key_zone_start" || id == "granular_key_zone_end" ||
        id == "granular_velocity_zone_start" || id == "granular_velocity_zone_end")
      return "Zones - Granular";
    if (isRoutingParameter(id))
      return (id == "effect_chain_order" || id == "post_effect_order" || id.startsWith("effect_chain_slot_") ||
              id.endsWith("effect_chain_order") || id.endsWith("post_effect_order") ||
              id.contains("_effect_chain_slot_")) ?
             kEffectsChainSection : kSignalRoutingSection;
    for (int osc = 1; osc <= vital::kNumOscillators; ++osc) {
      if (id.startsWith("osc_" + String(osc)))
        return "Oscillator " + String(osc);
    }
    if (id.startsWith("sample_")) return "Sample";
    if (id.startsWith("granular_")) return "Granular";
    if (id.startsWith("filter_1")) return "Filter 1";
    if (id.startsWith("filter_2")) return "Filter 2";
    if (id.startsWith("env_1")) return "Envelope 1";
    if (id.startsWith("env_2")) return "Envelope 2";
    if (id.startsWith("env_3")) return "Envelope 3";
    if (id.startsWith("env_4")) return "Envelope 4";
    if (id.startsWith("env_5")) return "Envelope 5";
    if (id.startsWith("env_6")) return "Envelope 6";
    if (id.startsWith("lfo_")) {
      const String number = id.fromFirstOccurrenceOf("lfo_", false, false)
                              .upToFirstOccurrenceOf("_", false, false);
      const int lfo = number.getIntValue();
      if (lfo > 0)
        return "LFO " + String(lfo);
      return "LFO 1";
    }
    if (id.startsWith("random_")) {
      const String number = id.fromFirstOccurrenceOf("random_", false, false)
                              .upToFirstOccurrenceOf("_", false, false);
      const int random = number.getIntValue();
      if (random > 0)
        return "Random " + String(random);
      return "Random 1";
    }
    if (id.startsWith("modulation_")) return "Modulation routing";
    if (id.startsWith("chorus_")) return "Chorus";
    if (id.startsWith("compressor_")) return "Compressor";
    if (id.startsWith("delay_")) return "Delay";
    if (id.startsWith("dimension_expander_")) return "Dimension Expander";
    if (id.startsWith("distortion_")) return "Distortion";
    if (id.startsWith("eq_")) return "Equalizer";
    if (id.startsWith("flanger_")) return "Flanger";
    if (id.startsWith("filter_fx_")) return "Filter";
    if (id.startsWith("frequency_shifter_")) return "Frequency Shifter";
    if (id.startsWith("limiter_")) return "Limiter";
    if (id.startsWith("phase_shift_")) return "Phase Shift";
    if (id.startsWith("phaser_")) return "Phaser";
    if (id.startsWith("reverb_")) return "Reverb";
    if (id.startsWith("utility_")) return "Utility";
    if (id.startsWith("macro_")) return "Macros";
    if (id.contains("voice") || id.startsWith("polyphony") || id.startsWith("portamento") ||
        id.startsWith("legato") || id.startsWith("zone_")) return "Voice and performance";
    return "Master and global";
  }

  String modulationDestinationGroupForId(const String& id) {
    const String section = sectionForParameter(id);
    if (section == kSignalRoutingSection)
      return "Signal routing";
    if (section == kEffectsChainSection)
      return "Effects chain";
    return section;
  }

  String modulationDestinationLabelForId(const String& id) {
    if (vital::Parameters::isParameter(id.toStdString()))
      return vital::Parameters::getDisplayName(id.toStdString());
    return readableId(id);
  }

  String midiControlName(int midi_id) {
    switch (midi_id) {
      case 1:   return "mod wheel";
      case 2:   return "breath controller";
      case 4:   return "foot controller";
      case 5:   return "portamento time";
      case 7:   return "channel volume";
      case 8:   return "balance";
      case 10:  return "pan";
      case 11:  return "expression";
      case 64:  return "sustain pedal";
      case 65:  return "portamento";
      case 66:  return "sostenuto pedal";
      case 67:  return "soft pedal";
      case 71:  return "resonance";
      case 74:  return "cutoff";
      case 91:  return "reverb depth";
      case 93:  return "chorus depth";
      default:  return "CC " + String(midi_id);
    }
  }

  String modulationSourceLabelForId(const String& id) {
    if (id == "mod_wheel") return "Mod wheel";
    if (id == "pitch_wheel") return "Pitch bend";
    if (id == "note_in_octave") return "Note in octave";
    if (id == "lift") return "Release velocity";
    if (id == "slide") return "MPE slide";
    if (id == "aftertouch") return "Aftertouch";
    if (id == "velocity") return "Velocity";
    if (id == "note") return "Note";
    if (id == "stereo") return "Stereo voice split";
    return readableId(id);
  }

  bool isInvalidModulationPair(const String& source_id, const String& destination_id) {
    if (source_id.isEmpty() || destination_id.isEmpty())
      return false;

    const StringArray self_modulating_prefixes { "lfo_", "env_", "random_" };
    for (const auto& prefix : self_modulating_prefixes) {
      if (!source_id.startsWith(prefix))
        continue;

      const String source_number = source_id.fromFirstOccurrenceOf(prefix, false, false)
                                       .upToFirstOccurrenceOf("_", false, false);
      if (source_number.isEmpty())
        continue;

      const String destination_prefix = prefix + source_number + "_";
      if (destination_id.startsWith(destination_prefix))
        return true;
    }
    return false;
  }

  int modulationSlotForParameterId(const String& id) {
    if (!id.startsWith("modulation_"))
      return -1;

    const String slot = id.fromFirstOccurrenceOf("modulation_", false, false)
                          .upToFirstOccurrenceOf("_", false, false);
    return slot.getIntValue();
  }

  int modulationParameterSuffixRank(const String& id) {
    static const std::array<const char*, 5> order = {{ "amount", "power", "bipolar", "stereo", "bypass" }};
    const String suffix = id.fromLastOccurrenceOf("_", false, false);
    for (int i = 0; i < static_cast<int>(order.size()); ++i)
      if (suffix == order[i])
        return i;
    return static_cast<int>(order.size());
  }

  String descriptionForCommonSuffix(const String& suffix, const String& subject) {
    if (suffix == "on")
      return "Enable or mute " + subject + ".";
    if (suffix == "level")
      return "Set the output level of " + subject + ".";
    if (suffix == "pan")
      return "Place " + subject + " between the left and right channels.";
    if (suffix == "destination")
      return "Choose where " + subject + " is routed next.";
    if (suffix == "transpose")
      return "Transpose " + subject + " in semitones.";
    if (suffix == "tune")
      return "Fine tune " + subject + " in cents.";
    if (suffix == "keytrack")
      return "Control how much " + subject + " follows the played MIDI note.";
    if (suffix == "root_key")
      return "Set the note that plays the sample at its original pitch.";
    if (suffix == "low_cutoff")
      return "Remove low frequencies from " + subject + ".";
    if (suffix == "high_cutoff")
      return "Remove high frequencies from " + subject + ".";
    if (suffix == "key_zone_start")
      return "Set the lowest MIDI note that can trigger " + subject + ".";
    if (suffix == "key_zone_end")
      return "Set the highest MIDI note that can trigger " + subject + ".";
    if (suffix == "velocity_zone_start")
      return "Set the lowest velocity that can trigger " + subject + ".";
    if (suffix == "velocity_zone_end")
      return "Set the highest velocity that can trigger " + subject + ".";
    if (suffix == "transpose_quantize_key")
      return "Choose the root key used when pitch modulation is quantized.";
    if (suffix == "transpose_quantize_scale")
      return "Choose the scale used when pitch modulation is quantized.";
    if (suffix == "transpose_quantize_mode")
      return "Choose whether pitch quantize uses this source's scale or the global scale.";
    if (suffix == "transpose_quantize")
      return "Snap pitch modulation to the selected key and scale.";
    return {};
  }

  String accessibleParameterHelpText(const String& parameter_id, const String& accessible_name, bool toggle) {
    if (parameter_id.isEmpty())
      return toggle ? "Enable or disable " + accessible_name + "." : "Adjust " + accessible_name + ".";

    const String id = parameter_id.toLowerCase();
    auto suffixAfter = [&id](const String& prefix) {
      return id.fromFirstOccurrenceOf(prefix, false, false);
    };

    if (id == "volume")
      return "Set Atlas's final output level.";
    if (id == "bypass")
      return "Bypass the whole instrument output.";
    if (id == "polyphony")
      return "Set the maximum number of voices Atlas can play at once.";
    if (id == "legato")
      return "When enabled, overlapping mono notes glide without retriggering the amp envelope.";
    if (id == "portamento_time")
      return "Set how long pitch glide takes between notes.";
    if (id == "portamento_slope")
      return "Shape the curve of the portamento glide.";
    if (id == "portamento_force")
      return "Force portamento even when notes are not overlapping.";
    if (id == "portamento_glide_zones")
      return "When enabled, the glide crosses zone boundaries and each zone's pitch is clamped to its key range.";
    if (id == "zone_crossfade")
      return "Set how long zones fade in and out as a glide crosses their boundaries.";
    if (id == "voice_transpose")
      return "Transpose the whole synth in semitones.";
    if (id == "voice_tune")
      return "Fine tune the whole synth in cents.";
    if (id == "voice_amplitude")
      return "Set the base voice amplitude before the final output stage.";
    if (id == "velocity_track")
      return "Control how much note velocity changes voice level.";
    if (id == "pitch_bend_range")
      return "Set the pitch bend wheel range in semitones.";
    if (id == "stereo_routing")
      return "Set how strongly the final signal is widened or rotated in stereo.";
    if (id == "stereo_mode")
      return "Choose Spread for width or Rotate for left-right rotation.";
    if (id == "mpe_enabled")
      return "Enable MPE input for per-note expression.";

    for (int osc = 1; osc <= vital::kNumOscillators; ++osc) {
      const String prefix = "osc_" + String(osc) + "_";
      if (!id.startsWith(prefix))
        continue;
      const String suffix = suffixAfter(prefix);
      const String subject = "oscillator " + String(osc);
      if (const String common = descriptionForCommonSuffix(suffix, subject); common.isNotEmpty())
        return common;
      if (suffix == "wave_frame") return "Move through the frames of the oscillator wavetable.";
      if (suffix == "unison_voices") return "Set how many detuned copies this oscillator plays.";
      if (suffix == "unison_detune") return "Set the pitch spread between unison voices.";
      if (suffix == "unison_blend") return "Balance the main voice against the unison voices.";
      if (suffix == "stack_style") return "Choose how unison voices are pitch-stacked.";
      if (suffix == "detune_power") return "Shape how detune is distributed across unison voices.";
      if (suffix == "detune_range") return "Limit the pitch range used by unison detune.";
      if (suffix == "spectral_unison") return "Add extra spectral copies inside the oscillator warp engine.";
      if (suffix == "frame_spread") return "Spread unison voices across different wavetable frames.";
      if (suffix == "stereo_spread") return "Spread unison voices across the stereo field.";
      if (suffix == "phase") return "Set the oscillator start phase.";
      if (suffix == "random_phase") return "Randomize oscillator start phase on each triggered note.";
      if (suffix == "midi_track") return "Choose how strongly this oscillator follows MIDI note pitch.";
      if (suffix == "smooth_interpolation") return "Smooth movement between wavetable frames.";
      if (suffix == "distortion_type") return "Choose the oscillator distortion or ring modulation mode.";
      if (suffix == "distortion_amount") return "Set how strongly the oscillator distortion is applied.";
      if (suffix == "distortion_spread") return "Offset oscillator distortion amount across stereo or unison voices.";
      if (suffix == "spectral_morph_type") return "Choose the oscillator frequency morph mode.";
      if (suffix == "spectral_morph_amount") return "Set how strongly the frequency morph is applied.";
      if (suffix == "spectral_morph_spread") return "Offset frequency morph amount across stereo or unison voices.";
    }

    if (id.startsWith("sample_")) {
      const String suffix = suffixAfter("sample_");
      if (const String common = descriptionForCommonSuffix(suffix, "the sample oscillator"); common.isNotEmpty())
        return common;
      if (suffix == "playback_mode") return "Choose whether notes retrigger the sample or only open the amp envelope.";
      if (suffix == "loop") return "Loop the selected sample range while notes are held.";
      if (suffix == "bounce") return "Play the sample loop forward then backward.";
      if (suffix == "random_phase") return "Randomize the sample start point on each triggered note.";
      if (suffix == "start") return "Set where sample playback begins.";
      if (suffix == "end") return "Set where sample playback stops.";
      if (suffix == "loop_start") return "Set the start point of the sample loop.";
      if (suffix == "loop_end") return "Set the end point of the sample loop.";
      if (suffix == "loop_crossfade") return "Crossfade the sample loop boundary to reduce clicks.";
    }

    if (id.startsWith("granular_")) {
      const String suffix = suffixAfter("granular_");
      if (const String common = descriptionForCommonSuffix(suffix, "the granular oscillator"); common.isNotEmpty())
        return common;
      if (suffix == "mode") return "Choose play-through or manual position mode for grains.";
      if (suffix == "grain_count") return "Set how many grains can overlap at once.";
      if (suffix == "density") return "Set how often new grains are created.";
      if (suffix == "midi_density") return "Let MIDI note rate control grain density.";
      if (suffix == "grain_size") return "Set the length of each grain.";
      if (suffix == "speed") return "Set the grain playback speed through the sample.";
      if (suffix == "position") return "Choose the sample position used for new grains.";
      if (suffix == "position_mod") return "Set how far grain position moves automatically.";
      if (suffix == "position_mod_rate") return "Set the speed of automatic position movement.";
      if (suffix == "random_position") return "Randomize the start position of each grain.";
      if (suffix == "random_volume") return "Randomize the level of each grain.";
      if (suffix == "random_pan") return "Randomize the stereo position of each grain.";
      if (suffix == "random_pitch") return "Randomize the pitch of each grain.";
      if (suffix == "interval") return "Set the pitch interval used by random pitch jumps.";
      if (suffix == "interval_chance") return "Set how often the granular pitch interval is applied.";
      if (suffix == "direction") return "Choose forward, reverse, or mixed grain direction.";
      if (suffix == "start") return "Set the start of the sample region used by the granular oscillator.";
      if (suffix == "end") return "Set the end of the sample region used by the granular oscillator.";
    }

    if (id.startsWith("filter_") || id.startsWith("filter_fx_") || id.contains("_filter_fx_")) {
      String suffix = id;
      if (id.startsWith("filter_1_")) suffix = suffixAfter("filter_1_");
      else if (id.startsWith("filter_2_")) suffix = suffixAfter("filter_2_");
      else if (id.startsWith("filter_fx_")) suffix = suffixAfter("filter_fx_");
      else if (id.contains("_filter_fx_")) suffix = id.fromFirstOccurrenceOf("_filter_fx_", false, false);

      if (suffix == "on") return "Enable or bypass this filter.";
      if (suffix == "mix") return "Blend between the dry signal and this filter.";
      if (suffix == "model") return "Choose the filter model.";
      if (suffix == "style") return "Choose the filter style or slope inside the selected model.";
      if (suffix == "cutoff") return "Set the main filter cutoff frequency.";
      if (suffix == "resonance") return "Boost frequencies around the cutoff point.";
      if (suffix == "drive") return "Drive the filter input for more saturation.";
      if (suffix == "blend") return "Morph between the available responses of the selected filter model.";
      if (suffix == "blend_transpose") return "Tune the comb filter blend pitch.";
      if (suffix == "keytrack") return "Control how much filter cutoff follows the played note.";
      if (suffix == "formant_x") return "Move the formant filter along its X vowel axis.";
      if (suffix == "formant_y") return "Move the formant filter along its Y vowel axis.";
      if (suffix == "formant_transpose") return "Transpose the formant filter.";
      if (suffix == "formant_resonance") return "Control the sharpness of the formant peaks.";
      if (suffix == "formant_spread") return "Spread the formant peaks apart.";
      if (suffix.endsWith("_input")) return "Choose whether this source feeds the filter.";
      if (suffix == "destination") return "Choose where this filter output is routed.";
    }

    if (id.startsWith("env_")) {
      const String suffix = id.fromFirstOccurrenceOf("_", false, false).fromFirstOccurrenceOf("_", false, false);
      if (suffix == "delay") return "Wait before this envelope begins after a note starts.";
      if (suffix == "attack") return "Set how long the envelope takes to rise.";
      if (suffix == "hold") return "Hold the envelope at full level before decay begins.";
      if (suffix == "decay") return "Set how long the envelope takes to fall to sustain.";
      if (suffix == "sustain") return "Set the level held while the note is held.";
      if (suffix == "release") return "Set how long the envelope takes to fade after note release.";
      if (suffix == "attack_power" || suffix == "decay_power" || suffix == "release_power")
        return "Shape the curve of this envelope stage.";
    }

    if (id.startsWith("lfo_")) {
      const String suffix = id.fromFirstOccurrenceOf("_", false, false).fromFirstOccurrenceOf("_", false, false);
      if (suffix == "frequency") return "Set the free-running LFO rate.";
      if (suffix == "tempo") return "Choose the tempo-synced LFO rate.";
      if (suffix == "sync") return "Choose seconds, tempo, dotted, triplet, or keytracked timing.";
      if (suffix == "sync_type") return "Choose whether the LFO follows host transport or retriggers freely.";
      if (suffix == "phase") return "Set where the LFO cycle starts.";
      if (suffix == "delay") return "Delay the LFO after note start.";
      if (suffix == "fade") return "Fade the LFO in after note start.";
      if (suffix == "smooth_mode") return "Choose how LFO smoothing is applied.";
      if (suffix == "smooth_time") return "Set the amount of smoothing applied to LFO movement.";
      if (suffix == "stereo") return "Offset the LFO phase between left and right channels.";
      if (suffix == "rate_x10") return "Multiply this LFO rate by ten.";
      if (suffix == "keytrack_transpose") return "Transpose the keytracked LFO rate.";
      if (suffix == "keytrack_tune") return "Fine tune the keytracked LFO rate.";
    }

    if (id.startsWith("random_")) {
      const String suffix = id.fromFirstOccurrenceOf("_", false, false).fromFirstOccurrenceOf("_", false, false);
      if (suffix == "style") return "Choose the random modulation shape.";
      if (suffix == "frequency") return "Set the free-running random modulation rate.";
      if (suffix == "rate_x10") return "Multiply this random modulation rate by ten.";
      if (suffix == "sync") return "Choose seconds, tempo, dotted, triplet, or keytracked timing.";
      if (suffix == "tempo") return "Choose the tempo-synced random modulation rate.";
      if (suffix == "sync_type") return "Choose whether random modulation follows host transport.";
      if (suffix == "stereo") return "Choose whether left and right channels share or split random values.";
      if (suffix == "keytrack_transpose") return "Transpose the keytracked random rate.";
      if (suffix == "keytrack_tune") return "Fine tune the keytracked random rate.";
    }

    if (id.startsWith("modulation_")) {
      if (id.endsWith("_amount")) return "Set the depth of this modulation route.";
      if (id.endsWith("_power")) return "Shape the curve of this modulation route.";
      if (id.endsWith("_bipolar")) return "Use the modulation source in both positive and negative directions.";
      if (id.endsWith("_stereo")) return "Invert this modulation amount between left and right channels.";
      if (id.endsWith("_bypass")) return "Temporarily disable this modulation route.";
    }

    if (id.startsWith("macro_control_"))
      return "Control this macro value for modulation assignments.";
    if (id.startsWith("macro_bipolar_"))
      return "Choose whether this macro ranges from zero to one hundred or minus one hundred to one hundred.";

    auto effectSuffix = [&id](const String& prefix) -> String {
      if (id.startsWith(prefix))
        return id.fromFirstOccurrenceOf(prefix, false, false);
      if (id.contains("_" + prefix))
        return id.fromFirstOccurrenceOf("_" + prefix, false, false);
      return {};
    };
    String effect_suffix;
    const std::array<String, 13> effect_prefixes {{
      "chorus_", "compressor_", "delay_", "dimension_expander_", "distortion_", "eq_", "flanger_",
      "frequency_shifter_", "limiter_", "phase_shift_", "phaser_", "reverb_", "utility_"
    }};
    for (const auto& prefix : effect_prefixes) {
      effect_suffix = effectSuffix(prefix);
      if (effect_suffix.isNotEmpty())
        break;
    }
    if (effect_suffix.isNotEmpty()) {
      if (effect_suffix == "on") return "Enable or bypass this effect.";
      if (effect_suffix == "mix" || effect_suffix == "dry_wet") return "Blend between the dry signal and this effect.";
      if (effect_suffix == "feedback") return "Feed effect output back into its input.";
      if (effect_suffix == "frequency" || effect_suffix == "cutoff" || effect_suffix.endsWith("_cutoff") ||
          effect_suffix == "low_cut" || effect_suffix == "high_cut")
        return "Set the filter frequency used by this effect.";
      if (effect_suffix == "tempo") return "Choose this effect's tempo-synced rate.";
      if (effect_suffix == "sync") return "Choose free or tempo-synced timing for this effect.";
      if (effect_suffix == "type" || effect_suffix == "style" || effect_suffix.endsWith("_mode"))
        return "Choose the operating mode for this effect.";
      if (effect_suffix == "drive") return "Set how hard the signal drives this effect.";
      if (effect_suffix == "gain") return "Set this effect's output gain.";
      if (effect_suffix == "width") return "Control the stereo width of this effect.";
      if (effect_suffix == "threshold") return "Set the level where this dynamics effect begins working.";
      if (effect_suffix == "attack") return "Set how quickly this dynamics effect reacts to rising level.";
      if (effect_suffix == "release") return "Set how quickly this dynamics effect relaxes.";
      if (effect_suffix == "ratio") return "Set the amount of compression above the threshold.";
      if (effect_suffix == "size") return "Set the size or length of this effect.";
      if (effect_suffix == "decay") return "Set how long this effect rings out.";
    }

    return toggle ? "Enable or disable " + accessible_name + "." : "Adjust " + accessible_name + ".";
  }

  String numberWord(int number) {
    static const std::array<const char*, 20> words = {{
      "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten",
      "eleven", "twelve", "thirteen", "fourteen", "fifteen", "sixteen", "seventeen",
      "eighteen", "nineteen", "twenty"
    }};
    if (number >= 1 && number <= static_cast<int>(words.size()))
      return words[number - 1];
    return String(number);
  }

  bool isModulationMenuKey(const KeyPress& key) {
    return key.getModifiers().isShiftDown() &&
           CharacterFunctions::toLowerCase(key.getTextCharacter()) == 'm';
  }

  bool isMidiLearnKey(const KeyPress& key) {
    return key.getModifiers().isShiftDown() &&
           CharacterFunctions::toLowerCase(key.getTextCharacter()) == 'l';
  }

  bool isMidiClearKey(const KeyPress& key) {
    return key.getModifiers().isShiftDown() &&
           CharacterFunctions::toLowerCase(key.getTextCharacter()) == 'c';
  }

  // The right bracket key acts as a "context menu" modifier on every platform so
  // that Windows screen reader users (and anyone without VoiceOver) can open the
  // same menu that VoiceOver exposes through VO+Shift+M (the showMenu action).
  bool isContextMenuKey(const KeyPress& key) {
    return !key.getModifiers().isAnyModifierKeyDown() &&
           (key.getKeyCode() == ']' || key.getTextCharacter() == ']');
  }

  bool isEffectMoveEarlierKey(const KeyPress& key) {
    const ModifierKeys modifiers = key.getModifiers();
    const bool allowed_modifiers = !modifiers.isAnyModifierKeyDown() ||
                                   (modifiers.isShiftDown() && !modifiers.isCommandDown() &&
                                    !modifiers.isCtrlDown() && !modifiers.isAltDown());
    const juce_wchar character = key.getTextCharacter();
    return allowed_modifiers &&
           (key.getKeyCode() == '[' || character == '[' || character == '{');
  }

  bool isEffectMoveLaterKey(const KeyPress& key) {
    const ModifierKeys modifiers = key.getModifiers();
    const bool allowed_modifiers = !modifiers.isAnyModifierKeyDown() ||
                                   (modifiers.isShiftDown() && !modifiers.isCommandDown() &&
                                    !modifiers.isCtrlDown() && !modifiers.isAltDown());
    const juce_wchar character = key.getTextCharacter();
    return allowed_modifiers &&
           (key.getKeyCode() == ']' || character == ']' || character == '}');
  }

  int macroIndexForControlId(const String& id) {
    if (!id.startsWith("macro_control_"))
      return -1;

    const int macro = id.fromFirstOccurrenceOf("macro_control_", false, false).getIntValue() - 1;
    return isPositiveAndBelow(macro, vital::kNumMacros) ? macro : -1;
  }

  bool isMacroBipolarParameterId(const String& id) {
    if (!id.startsWith("macro_bipolar_"))
      return false;

    const int macro = id.fromFirstOccurrenceOf("macro_bipolar_", false, false).getIntValue() - 1;
    return isPositiveAndBelow(macro, vital::kNumMacros);
  }

  String macroBipolarParameterId(int macroIndex) {
    return "macro_bipolar_" + String(macroIndex + 1);
  }

  int modulationSourceSortRank(const String& id) {
    const int macro = macroIndexForControlId(id);
    if (macro >= 0)
      return 4000 + macro;

    if (id.startsWith("lfo_"))
      return 1000 + id.fromFirstOccurrenceOf("lfo_", false, false).getIntValue();
    if (id.startsWith("env_"))
      return 2000 + id.fromFirstOccurrenceOf("env_", false, false).getIntValue();
    if (id.startsWith("random_"))
      return 3000 + id.fromFirstOccurrenceOf("random_", false, false).getIntValue();
    if (id == "velocity") return 5000;
    if (id == "aftertouch") return 5001;
    if (id == "mod_wheel") return 5002;
    if (id == "pitch_wheel") return 5003;
    if (id == "lift") return 5004;
    if (id == "slide") return 5005;
    if (id == "note") return 5006;
    if (id == "note_in_octave") return 5007;
    if (id == "stereo") return 5008;
    return 9000;
  }

  int modulationDestinationSortRank(const String& id) {
    return sectionRank(modulationDestinationGroupForId(id)) * 10000 + parameterRank(id);
  }

  String routingDestinationText(int destination) {
    switch (destination) {
      case vital::constants::kFilter1:
        return "Filter 1";
      case vital::constants::kFilter2:
        return "Filter 2";
      case vital::constants::kDualFilters:
        return "Filter 1 and 2";
      case vital::constants::kEffects:
        return "Main effects";
      case vital::constants::kBus1:
        return "Bus 1";
      case vital::constants::kBus2:
        return "Bus 2";
      case vital::constants::kDirectOut:
        return "Direct out";
      case vital::constants::kBus3:
        return "Bus 3";
      default:
        return "Main effects";
    }
  }

  String busRoutingDestinationText(int bus_index, int destination) {
    if (destination == vital::constants::kBusDestinationDirectOut)
      return "Direct out";
    if (destination == vital::constants::kBusDestinationMain)
      return "Main effects";
    if (destination >= vital::constants::kBusDestinationBus1 &&
        destination <= vital::constants::kBusDestinationBus3) {
      const int target_bus = destination - vital::constants::kBusDestinationBus1;
      if (target_bus > bus_index)
        return "Bus " + String(target_bus + 1);
    }
    return "Main effects";
  }

  String percentString(float value) {
    return String(roundToInt(jlimit(0.0f, 1.0f, value) * 100.0f)) + "%";
  }

  String midiRootKeyText(double value) {
    static const std::array<const char*, 12> names = {{
      "C", "C sharp", "D", "E flat", "E", "F", "F sharp", "G", "A flat", "A", "B flat", "B"
    }};
    const int midi_note = jlimit(0, 127, roundToInt(value));
    return String(names[midi_note % vital::kNotesPerOctave]) + ", MIDI " + String(midi_note);
  }

  int shiftedDigitIndex(const KeyPress& key) {
    if (!key.getModifiers().isShiftDown())
      return -1;

    switch (key.getTextCharacter()) {
      case '!': return 0;
      case '@': return 1;
      case '#': return 2;
      case '$': return 3;
      case '%': return 4;
      case '^': return 5;
      case '&': return 6;
      case '*': return 7;
      case '(': return 8;
      case ')': return 9;
      default: break;
    }

    const int key_code = key.getKeyCode();
    if (key_code >= '1' && key_code <= '9')
      return key_code - '1';
    if (key_code == '0')
      return 9;
    return -1;
  }

  bool isTransposeQuantizeParameter(const String& id) {
    return id.endsWith("transpose_quantize");
  }

  bool isTransposeQuantizeModeParameter(const String& id) {
    return id.endsWith("_transpose_quantize_mode");
  }

  bool isEffectChoiceParameter(const String& id, const String& suffix) {
    return id == suffix || id.endsWith("_" + suffix);
  }

  String transposeQuantizeModeTitleForParameter(const String& id) {
    if (id.startsWith("osc_")) {
      const String number = id.fromFirstOccurrenceOf("osc_", false, false)
                            .upToFirstOccurrenceOf("_", false, false);
      return "Oscillator " + number + " transpose quantize mode";
    }
    if (id == "sample_transpose_quantize_mode")
      return "Sample transpose quantize mode";
    if (id == "granular_transpose_quantize_mode")
      return "Granular transpose quantize mode";
    return "Transpose quantize mode";
  }

  struct QuantizeScaleChoice {
    const char* name;
    int mask;
  };

  const std::vector<QuantizeScaleChoice>& transposeQuantizeScales() {
    static const std::vector<QuantizeScaleChoice> scales {
      { "Off", 0 },
      { "Chromatic", (1 << 12) - 1 },
      { "Major", (1 << 0) | (1 << 2) | (1 << 4) | (1 << 5) | (1 << 7) | (1 << 9) | (1 << 11) },
      { "Natural minor", (1 << 0) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 8) | (1 << 10) },
      { "Harmonic minor", (1 << 0) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 8) | (1 << 11) },
      { "Melodic minor", (1 << 0) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 9) | (1 << 11) },
      { "Major pentatonic", (1 << 0) | (1 << 2) | (1 << 4) | (1 << 7) | (1 << 9) },
      { "Minor pentatonic", (1 << 0) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 10) },
      { "Blues", (1 << 0) | (1 << 3) | (1 << 5) | (1 << 6) | (1 << 7) | (1 << 10) },
      { "Dorian", (1 << 0) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 9) | (1 << 10) },
      { "Phrygian", (1 << 0) | (1 << 1) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 8) | (1 << 10) },
      { "Lydian", (1 << 0) | (1 << 2) | (1 << 4) | (1 << 6) | (1 << 7) | (1 << 9) | (1 << 11) },
      { "Mixolydian", (1 << 0) | (1 << 2) | (1 << 4) | (1 << 5) | (1 << 7) | (1 << 9) | (1 << 10) },
      { "Locrian", (1 << 0) | (1 << 1) | (1 << 3) | (1 << 5) | (1 << 6) | (1 << 8) | (1 << 10) },
      { "Custom", -1 },
    };
    return scales;
  }

  int rotateTransposeMask(int mask, int key) {
    int result = 0;
    for (int i = 0; i < vital::kNotesPerOctave; ++i)
      if ((mask >> i) & 1)
        result |= 1 << ((i + key + vital::kNotesPerOctave) % vital::kNotesPerOctave);
    return result;
  }

  String transposeKeyName(int key) {
    static const std::array<const char*, 12> names = {{
      "C", "C sharp", "D", "E flat", "E", "F", "F sharp", "G", "A flat", "A", "B flat", "B"
    }};
    return names[jlimit(0, vital::kNotesPerOctave - 1, key)];
  }

  int scaleChoiceForMask(int pitch_mask, int& key) {
    const auto& scales = transposeQuantizeScales();
    if (pitch_mask == 0) {
      key = 0;
      return 0;
    }

    for (int scale = 1; scale < static_cast<int>(scales.size()) - 1; ++scale) {
      for (int candidate_key = 0; candidate_key < vital::kNotesPerOctave; ++candidate_key) {
        if (rotateTransposeMask(scales[scale].mask, candidate_key) == pitch_mask) {
          key = candidate_key;
          return scale;
        }
      }
    }

    key = 0;
    return static_cast<int>(scales.size()) - 1;
  }

  String transposeQuantizeDescription(int value) {
    constexpr int kPitchMask = (1 << vital::kNotesPerOctave) - 1;
    const int pitch_mask = value & kPitchMask;
    const bool global = vital::utils::isTransposeQuantizeGlobal(value);

    if (pitch_mask == 0)
      return "Off";
    if (pitch_mask == kPitchMask)
      return String(global ? "Global " : "Local ") + "chromatic";

    StringArray notes;
    for (int i = 0; i < vital::kNotesPerOctave; ++i)
      if ((pitch_mask >> i) & 1)
        notes.add(transposeKeyName(i));

    return String(global ? "Global scale, " : "Local scale, ") + notes.joinIntoString(", ");
  }

  int noteIndexForToken(String token) {
    token = token.trim().toLowerCase()
                 .replace("sharp", "#")
                 .replace("flat", "b")
                 .replaceCharacter(juce_wchar(0x266f), '#')
                 .replaceCharacter(juce_wchar(0x266d), 'b');
    if (token == "c") return 0;
    if (token == "c#" || token == "db") return 1;
    if (token == "d") return 2;
    if (token == "d#" || token == "eb") return 3;
    if (token == "e") return 4;
    if (token == "f") return 5;
    if (token == "f#" || token == "gb") return 6;
    if (token == "g") return 7;
    if (token == "g#" || token == "ab") return 8;
    if (token == "a") return 9;
    if (token == "a#" || token == "bb") return 10;
    if (token == "b") return 11;
    return -1;
  }

  int scaleMaskForName(String text) {
    text = text.toLowerCase().trim();
    if (text == "off" || text == "none")
      return 0;
    if (text == "minor")
      text = "natural minor";
    for (const auto& scale : transposeQuantizeScales())
      if (text == String(scale.name).toLowerCase())
        return scale.mask;
    return -1;
  }

  bool parseTransposeQuantizeText(String text, int current_value, int& result) {
    text = text.trim();
    if (text.isEmpty())
      return false;

    if (text.containsOnly("0123456789")) {
      result = jlimit(0, 8191, text.getIntValue());
      return true;
    }

    String lower = text.toLowerCase();
    bool global = vital::utils::isTransposeQuantizeGlobal(current_value);
    if (lower.contains("global")) {
      global = true;
      lower = lower.replace("global", "");
    }
    if (lower.contains("local")) {
      global = false;
      lower = lower.replace("local", "");
    }
    lower = lower.replace("snap", "").replace("scale", "").trim();

    int mask = scaleMaskForName(lower);
    if (mask < 0) {
      mask = 0;
      StringArray tokens;
      tokens.addTokens(lower.replace(",", " ").replace(";", " "), " \t\r\n", "");
      for (const auto& token : tokens) {
        const int note = noteIndexForToken(token);
        if (note >= 0)
          mask |= 1 << note;
      }
      if (mask == 0)
        return false;
    }

    result = mask | (global ? (1 << vital::kNotesPerOctave) : 0);
    return true;
  }

  float pointOutputValue(LineGenerator* generator, int index) {
    if (generator == nullptr || !isPositiveAndBelow(index, generator->getNumPoints()))
      return 0.0f;
    return 1.0f - generator->getPoint(index).second;
  }

  enum AccessibleLfoCurve {
    kCurveHold,
    kCurveLinear,
    kCurveEaseIn,
    kCurveEaseOut,
    kCurveSmooth,
    kCurveSCurve,
    kNumAccessibleLfoCurves
  };

  int curveIndexForPower(float power, bool smooth) {
    if (LineGenerator::isHoldPower(power))
      return kCurveHold;
    if (smooth)
      return std::abs(power) <= 0.5f ? kCurveSCurve : kCurveSmooth;
    if (power > 0.5f)
      return kCurveEaseIn;
    if (power < -0.5f)
      return kCurveEaseOut;
    return kCurveLinear;
  }

  float powerForCurveIndex(int index) {
    static const std::array<float, kNumAccessibleLfoCurves> powers = {{
      20.0f, 0.0f, 3.0f, -3.0f, 0.75f, 0.0f
    }};
    return powers[jlimit(0, static_cast<int>(powers.size()) - 1, index)];
  }

  bool smoothForCurveIndex(int index) {
    return index == kCurveSmooth || index == kCurveSCurve;
  }

  String curveNameForIndex(int index) {
    static const std::array<const char*, kNumAccessibleLfoCurves> names = {{
      "Hold", "Linear", "Ease In", "Ease Out", "Smooth", "S Curve"
    }};
    return names[jlimit(0, static_cast<int>(names.size()) - 1, index)];
  }

  class OrderedFocusTraverser : public ComponentTraverser {
    public:
      OrderedFocusTraverser(SynthEditor& editor, bool keyboardTraversal) :
          editor_(editor), keyboard_traversal_(keyboardTraversal) { }

      Component* getDefaultComponent(Component*) override {
        const auto components = getAllComponents(nullptr);
        return components.empty() ? nullptr : components.front();
      }

      Component* getNextComponent(Component* current) override {
        return adjacent(current, 1);
      }

      Component* getPreviousComponent(Component* current) override {
        return adjacent(current, -1);
      }

      std::vector<Component*> getAllComponents(Component*) override {
        std::vector<Component*> result;
        const auto& order = keyboard_traversal_ ? editor_.getKeyboardFocusOrder()
                                                : editor_.getAccessibleFocusOrder();
        for (auto* component : order) {
          if (component != nullptr && component->isShowing() && component->getWantsKeyboardFocus())
            result.push_back(component);
        }
        return result;
      }

    private:
      Component* adjacent(Component* current, int direction) {
        const auto components = getAllComponents(nullptr);
        const auto found = std::find(components.begin(), components.end(), current);
        if (found == components.end() || components.empty())
          return nullptr;

        if (direction > 0 && std::next(found) != components.end())
          return withVisibleComponent(*std::next(found));
        if (direction < 0 && found != components.begin())
          return withVisibleComponent(*std::prev(found));
        return nullptr;
      }

      Component* withVisibleComponent(Component* component) {
        editor_.ensureComponentVisible(component);
        return component;
      }

      SynthEditor& editor_;
      bool keyboard_traversal_ = false;
  };

  class ChildListTraverser : public ComponentTraverser {
    public:
      explicit ChildListTraverser(const std::vector<Component*>& children) : children_(children) { }

      Component* getDefaultComponent(Component*) override {
        const auto components = getAllComponents(nullptr);
        return components.empty() ? nullptr : components.front();
      }

      Component* getNextComponent(Component* current) override {
        return adjacent(current, 1);
      }

      Component* getPreviousComponent(Component* current) override {
        return adjacent(current, -1);
      }

      std::vector<Component*> getAllComponents(Component*) override {
        std::vector<Component*> result;
        for (auto* component : children_) {
          if (component != nullptr && component->isShowing() && component->getWantsKeyboardFocus())
            result.push_back(component);
        }
        return result;
      }

    private:
      Component* adjacent(Component* current, int direction) {
        const auto components = getAllComponents(nullptr);
        const auto found = std::find(components.begin(), components.end(), current);
        if (found == components.end() || components.empty())
          return nullptr;

        if (direction > 0 && std::next(found) != components.end())
          return *std::next(found);
        if (direction < 0 && found != components.begin())
          return *std::prev(found);
        return nullptr;
      }

      const std::vector<Component*>& children_;
  };

  // Implemented by the offscreen controls so that the accessibility handlers
  // (VoiceOver VO+Shift+M -> showMenu) and the key handlers (the right bracket
  // key) can trigger the control's context menu through a common entry point.
  struct AccessibleContextMenuTarget {
    virtual ~AccessibleContextMenuTarget() = default;
    virtual void showAccessibleContextMenu() = 0;
  };

  class OffscreenSliderAccessibilityHandler final : public AccessibilityHandler {
    public:
      explicit OffscreenSliderAccessibilityHandler(Slider& sliderToWrap) :
          AccessibilityHandler(sliderToWrap, AccessibilityRole::slider, contextMenuActions(sliderToWrap),
                               AccessibilityHandler::Interfaces{std::make_unique<ValueInterface>(sliderToWrap)}),
          slider_(sliderToWrap) { }

      AccessibleState getCurrentState() const override {
        return AccessibilityHandler::getCurrentState().withAccessibleOffscreen();
      }

      String getDescription() const override { return {}; }
      String getHelp() const override { return {}; }

    private:
      static AccessibilityActions contextMenuActions(Slider& slider) {
        return AccessibilityActions().addAction(AccessibilityActionType::showMenu, [&slider] {
          if (auto* menu_target = dynamic_cast<AccessibleContextMenuTarget*>(&slider))
            menu_target->showAccessibleContextMenu();
        });
      }

      class ValueInterface final : public AccessibilityValueInterface {
        public:
          explicit ValueInterface(Slider& sliderToWrap) :
              slider_(sliderToWrap), use_max_value_(sliderToWrap.isTwoValue()) { }

          bool isReadOnly() const override { return false; }

          double getCurrentValue() const override {
            return use_max_value_ ? slider_.getMaximum() : slider_.getValue();
          }

          void setValue(double newValue) override {
            Slider::ScopedDragNotification drag(slider_);
            if (use_max_value_)
              slider_.setMaxValue(newValue, sendNotificationSync);
            else
              slider_.setValue(newValue, sendNotificationSync);
          }

          String getCurrentValueAsString() const override {
            return slider_.getTextFromValue(getCurrentValue());
          }

          void setValueAsString(const String& newValue) override {
            setValue(slider_.getValueFromText(newValue));
          }

          AccessibleValueRange getRange() const override {
            return { { slider_.getMinimum(), slider_.getMaximum() }, slider_.getInterval() };
          }

        private:
          Slider& slider_;
          bool use_max_value_ = false;
      };

      Slider& slider_;
  };

  class OffscreenButtonAccessibilityHandler final : public AccessibilityHandler {
    public:
      explicit OffscreenButtonAccessibilityHandler(Button& buttonToWrap, AccessibilityRole role) :
          AccessibilityHandler(buttonToWrap, role, buttonActions(buttonToWrap), buttonInterfaces(buttonToWrap)),
          button_(buttonToWrap) { }

      AccessibleState getCurrentState() const override {
        auto state = AccessibilityHandler::getCurrentState().withAccessibleOffscreen();
        if (button_.isToggleable()) {
          state = state.withCheckable();
          if (button_.getToggleState())
            state = state.withChecked();
        }
        return state;
      }

      String getTitle() const override {
        const auto title = AccessibilityHandler::getTitle();
        return title.isEmpty() ? button_.getButtonText() : title;
      }

      String getDescription() const override { return {}; }
      String getHelp() const override { return {}; }

    private:
      class ValueInterface final : public AccessibilityTextValueInterface {
        public:
          explicit ValueInterface(Button& buttonToWrap) : button_(buttonToWrap) { }

          bool isReadOnly() const override { return true; }
          String getCurrentValueAsString() const override {
            const char* key = button_.getToggleState() ? "accessibleOnText" : "accessibleOffText";
            const auto custom_text = button_.getProperties()[key].toString();
            return custom_text.isNotEmpty() ? custom_text : (button_.getToggleState() ? "On" : "Off");
          }
          void setValueAsString(const String&) override { }

        private:
          Button& button_;
      };

      static AccessibilityActions buttonActions(Button& button) {
        auto actions = AccessibilityActions().addAction(AccessibilityActionType::press,
                                                        [&button] { button.triggerClick(); });
        if (button.isToggleable()) {
          actions = actions.addAction(AccessibilityActionType::toggle, [&button] {
            if (button.isEnabled())
              button.setToggleState(!button.getToggleState(), sendNotification);
          });
        }
        if (dynamic_cast<AccessibleContextMenuTarget*>(&button)) {
          actions = actions.addAction(AccessibilityActionType::showMenu, [&button] {
            if (auto* menu_target = dynamic_cast<AccessibleContextMenuTarget*>(&button))
              menu_target->showAccessibleContextMenu();
          });
        }
        return actions;
      }

      static AccessibilityHandler::Interfaces buttonInterfaces(Button& button) {
        if (button.isToggleable())
          return AccessibilityHandler::Interfaces{std::make_unique<ValueInterface>(button)};
        return {};
      }

      Button& button_;
  };

  class OffscreenTextButton : public TextButton {
    public:
      using TextButton::TextButton;

      std::unique_ptr<AccessibilityHandler> createAccessibilityHandler() override {
        return std::make_unique<OffscreenButtonAccessibilityHandler>(*this, AccessibilityRole::button);
      }
  };

  class OffscreenComboBox : public ComboBox {
    public:
      std::function<void()> onFocusGained;

      void focusGained(FocusChangeType cause) override {
        ComboBox::focusGained(cause);
        if (onFocusGained)
          onFocusGained();
      }

      std::unique_ptr<AccessibilityHandler> createAccessibilityHandler() override {
        class Handler final : public AccessibilityHandler {
          public:
            explicit Handler(ComboBox& comboBoxToWrap) :
                AccessibilityHandler(comboBoxToWrap, AccessibilityRole::comboBox,
                                     AccessibilityActions()
                                         .addAction(AccessibilityActionType::press,
                                                    [&comboBoxToWrap] { comboBoxToWrap.showPopup(); })
                                         .addAction(AccessibilityActionType::showMenu,
                                                    [&comboBoxToWrap] { comboBoxToWrap.showPopup(); }),
                                     AccessibilityHandler::Interfaces{
                                         std::make_unique<ValueInterface>(comboBoxToWrap) }),
                combo_box_(comboBoxToWrap) { }

            AccessibleState getCurrentState() const override {
              auto state = AccessibilityHandler::getCurrentState().withAccessibleOffscreen().withExpandable();
              return combo_box_.isPopupActive() ? state.withExpanded() : state.withCollapsed();
            }

            String getDescription() const override { return {}; }
            String getHelp() const override { return {}; }

          private:
            class ValueInterface final : public AccessibilityTextValueInterface {
              public:
                explicit ValueInterface(ComboBox& comboBoxToWrap) : combo_box_(comboBoxToWrap) { }

                bool isReadOnly() const override { return true; }
                String getCurrentValueAsString() const override { return combo_box_.getText(); }
                void setValueAsString(const String&) override { }

              private:
                ComboBox& combo_box_;
            };

            ComboBox& combo_box_;
        };

        return std::make_unique<Handler>(*this);
      }
  };

  class OffscreenSlider : public Slider, public AccessibleContextMenuTarget {
    public:
      std::function<bool(const KeyPress&, Component&)> onAccessibleCommand;
      std::function<void(Component&)> onTextEntryCommand;
      std::function<void()> onDefaultCommand;
      std::function<void()> onContextMenuCommand;

      void showAccessibleContextMenu() override {
        if (onContextMenuCommand)
          onContextMenuCommand();
      }

      bool keyPressed(const KeyPress& key) override {
        if (onAccessibleCommand && onAccessibleCommand(key, *this))
          return true;
        if (onContextMenuCommand && isContextMenuKey(key)) {
          onContextMenuCommand();
          return true;
        }
        if (key.getKeyCode() == KeyPress::returnKey) {
          if (onTextEntryCommand)
            onTextEntryCommand(*this);
          return true;
        }
        if (key.getKeyCode() == KeyPress::backspaceKey || key.getKeyCode() == KeyPress::deleteKey) {
          if (onDefaultCommand)
            onDefaultCommand();
          if (auto* handler = getAccessibilityHandler())
            handler->notifyAccessibilityEvent(AccessibilityEvent::valueChanged);
          return true;
        }

        if (key.getKeyCode() == KeyPress::homeKey) {
          Slider::ScopedDragNotification drag(*this);
          setValue(getMaximum(), sendNotificationSync);
          if (auto* handler = getAccessibilityHandler())
            handler->notifyAccessibilityEvent(AccessibilityEvent::valueChanged);
          return true;
        }
        if (key.getKeyCode() == KeyPress::endKey) {
          Slider::ScopedDragNotification drag(*this);
          setValue(getMinimum(), sendNotificationSync);
          if (auto* handler = getAccessibilityHandler())
            handler->notifyAccessibilityEvent(AccessibilityEvent::valueChanged);
          return true;
        }

        const bool increase = key.getKeyCode() == KeyPress::upKey || key.getKeyCode() == KeyPress::rightKey ||
                              key.getKeyCode() == KeyPress::pageUpKey;
        const bool decrease = key.getKeyCode() == KeyPress::downKey || key.getKeyCode() == KeyPress::leftKey ||
                              key.getKeyCode() == KeyPress::pageDownKey;
        if (!increase && !decrease)
          return Slider::keyPressed(key);

        const double range = getMaximum() - getMinimum();
        const bool continuous = getInterval() <= 0.0;
        const double interval = continuous ? range / 100.0 : getInterval();
        const bool page = key.getKeyCode() == KeyPress::pageUpKey ||
                          key.getKeyCode() == KeyPress::pageDownKey;
        // Mirror of SynthSlider::kSlowDragMultiplier: Shift gives a 1/10 step. Only meaningful on
        // continuous params, where the interval is an arbitrary fallback rather than a real step.
        constexpr double kFineMultiplier = 0.1;
        double multiplier = 1.0;
        if (page)
          multiplier = 10.0;
        else if (continuous && key.getModifiers().isShiftDown())
          multiplier = kFineMultiplier;
        const double delta = interval * multiplier * (increase ? 1.0 : -1.0);
        Slider::ScopedDragNotification drag(*this);
        setValue(jlimit(getMinimum(), getMaximum(), getValue() + delta), sendNotificationSync);
        if (auto* handler = getAccessibilityHandler())
          handler->notifyAccessibilityEvent(AccessibilityEvent::valueChanged);
        return true;
      }

      std::unique_ptr<AccessibilityHandler> createAccessibilityHandler() override {
        return std::make_unique<OffscreenSliderAccessibilityHandler>(*this);
      }
  };

  class OffscreenToggleButton : public ToggleButton, public AccessibleContextMenuTarget {
    public:
      std::function<bool(const KeyPress&, Component&)> onAccessibleCommand;
      std::function<void()> onContextMenuCommand;

      void showAccessibleContextMenu() override {
        if (onContextMenuCommand)
          onContextMenuCommand();
      }

      bool keyPressed(const KeyPress& key) override {
        if (onAccessibleCommand && onAccessibleCommand(key, *this))
          return true;
        if (onContextMenuCommand && isContextMenuKey(key)) {
          onContextMenuCommand();
          return true;
        }
        return ToggleButton::keyPressed(key);
      }

      std::unique_ptr<AccessibilityHandler> createAccessibilityHandler() override {
        return std::make_unique<OffscreenButtonAccessibilityHandler>(*this, AccessibilityRole::toggleButton);
      }
  };
}

std::unique_ptr<AccessibilityHandler> AccessibleTextButton::createAccessibilityHandler() {
  class Handler final : public AccessibilityHandler {
    public:
      explicit Handler(Button& buttonToWrap) :
          AccessibilityHandler(buttonToWrap, AccessibilityRole::button,
                               AccessibilityActions().addAction(AccessibilityActionType::press,
                                                                 [&buttonToWrap] { buttonToWrap.triggerClick(); })),
          button_(buttonToWrap) { }

      AccessibleState getCurrentState() const override {
        return AccessibilityHandler::getCurrentState().withAccessibleOffscreen();
      }

      String getTitle() const override {
        const auto title = AccessibilityHandler::getTitle();
        return title.isEmpty() ? button_.getButtonText() : title;
      }

      String getDescription() const override { return {}; }
      String getHelp() const override { return {}; }

    private:
      Button& button_;
  };

  return std::make_unique<Handler>(*this);
}

std::unique_ptr<AccessibilityHandler> AccessibleComboBox::createAccessibilityHandler() {
  class Handler final : public AccessibilityHandler {
    public:
      explicit Handler(ComboBox& comboBoxToWrap) :
          AccessibilityHandler(comboBoxToWrap, AccessibilityRole::comboBox,
                               AccessibilityActions()
                                   .addAction(AccessibilityActionType::press,
                                              [&comboBoxToWrap] { comboBoxToWrap.showPopup(); })
                                   .addAction(AccessibilityActionType::showMenu,
                                              [&comboBoxToWrap] { comboBoxToWrap.showPopup(); }),
                               AccessibilityHandler::Interfaces{
                                   std::make_unique<ValueInterface>(comboBoxToWrap) }),
          combo_box_(comboBoxToWrap) { }

      AccessibleState getCurrentState() const override {
        auto state = AccessibilityHandler::getCurrentState().withAccessibleOffscreen().withExpandable();
        return combo_box_.isPopupActive() ? state.withExpanded() : state.withCollapsed();
      }

      String getDescription() const override { return {}; }
      String getHelp() const override { return {}; }

    private:
      class ValueInterface final : public AccessibilityTextValueInterface {
        public:
          explicit ValueInterface(ComboBox& comboBoxToWrap) : combo_box_(comboBoxToWrap) { }

          bool isReadOnly() const override { return true; }
          String getCurrentValueAsString() const override { return combo_box_.getText(); }
          void setValueAsString(const String&) override { }

        private:
          ComboBox& combo_box_;
      };

      ComboBox& combo_box_;
  };

  return std::make_unique<Handler>(*this);
}

class AccessibleParameterRow : public Component {
  public:
    explicit AccessibleParameterRow(AudioProcessorParameter& parameter, const String& accessibleName = {},
                                    std::function<String(double)> textFromValue = {},
                                    double maxNormalizedValue = 1.0,
                                    double minNormalizedValue = 0.0,
                                    const String& accessibleOffText = {},
                                    const String& accessibleOnText = {},
                                    const String& accessibleDescription = {},
                                    bool forceSliderControl = false) :
        parameter_(parameter), text_from_value_(std::move(textFromValue)),
        min_normalized_value_(jlimit(0.0, 1.0, minNormalizedValue)),
        max_normalized_value_(jlimit(min_normalized_value_, 1.0, maxNormalizedValue)) {
      use_toggle_control_ = parameter_.isBoolean() && !forceSliderControl;
      const auto* bridge = dynamic_cast<ValueBridge*>(&parameter_);
      const String parameter_id = bridge != nullptr ? bridge->getParameterId() : String();
      const auto name = parameter_.getName(128);
      const bool random_rate = parameter_id.startsWith("random_") && parameter_id.endsWith("_frequency");
      const auto accessible_name = accessibleName.isNotEmpty() ? accessibleName : (random_rate ? String("Rate") : name);
      const String parameter_description = accessibleDescription.isNotEmpty()
          ? accessibleDescription
          : accessibleParameterHelpText(parameter_id, accessible_name, use_toggle_control_);
      label_.setText(accessible_name, dontSendNotification);
      label_.setTitle(accessible_name + " label");
      label_.setColour(Label::textColourId, Colours::white);
      addAndMakeVisible(label_);

      if (use_toggle_control_) {
        toggle_.setButtonText(accessible_name);
        toggle_.setTitle(accessible_name);
        toggle_.setDescription(parameter_description);
        toggle_.setTooltip(parameter_description);
        if (accessibleOffText.isNotEmpty())
          toggle_.getProperties().set("accessibleOffText", accessibleOffText);
        if (accessibleOnText.isNotEmpty())
          toggle_.getProperties().set("accessibleOnText", accessibleOnText);
        toggle_.setHelpText("Press Space to toggle. Press Shift M, the right bracket key, or VoiceOver Shift M for the context menu. Press Shift L for MIDI learn, or Shift C to clear MIDI learn.");
        toggle_.setWantsKeyboardFocus(true);
        toggle_.onClick = [this] {
          parameter_.beginChangeGesture();
          parameter_.setValueNotifyingHost(toggle_.getToggleState() ? 1.0f : 0.0f);
          parameter_.endChangeGesture();
        };
        toggle_.onContextMenuCommand = [this] { showContextMenu(toggle_); };
        addAndMakeVisible(toggle_);
      }
      else {
        slider_.setSliderStyle(Slider::LinearHorizontal);
        slider_.setTextBoxStyle(Slider::TextBoxRight, false, 150, 28);
        const int steps = parameter_.getNumSteps();
        slider_.setRange(min_normalized_value_, max_normalized_value_,
                         steps > 1 && steps < 10000 ? 1.0 / (steps - 1) : 0.0);
        slider_.setTitle(accessible_name);
        slider_.setDescription(parameter_description);
        slider_.setTooltip(parameter_description);
        slider_.setHelpText(random_rate ? "In free mode this controls how long each random cycle lasts. In synced mode use Tempo."
                                        : "Use arrows for changes, hold Shift with the arrows for finer changes, Page Up and Page Down for larger changes, Enter to type a value, Backspace to reset to default. Press Shift M, the right bracket key, or VoiceOver Shift M for the context menu. Press Shift L for MIDI learn, and Shift C to clear MIDI learn.");
        slider_.setWantsKeyboardFocus(true);
        slider_.textFromValueFunction = [this](double value) {
          if (text_from_value_)
            return text_from_value_(value);
          if (auto* bridge = dynamic_cast<ValueBridge*>(&parameter_))
            return accessibleParameterText(*bridge, value);
          return parameter_.getText(static_cast<float>(value), 128);
        };
        slider_.valueFromTextFunction = [this](const String& text) {
          if (auto* bridge = dynamic_cast<ValueBridge*>(&parameter_))
            return accessibleParameterValueForText(*bridge, text);
          return static_cast<double>(parameter_.getValueForText(text));
        };
        slider_.onDragStart = [this] { parameter_.beginChangeGesture(); };
        slider_.onDragEnd = [this] { parameter_.endChangeGesture(); };
        slider_.onValueChange = [this] {
          if (!updating_)
            parameter_.setValueNotifyingHost(static_cast<float>(slider_.getValue()));
        };
        slider_.onDefaultCommand = [this] { resetToDefaultValue(); };
        slider_.onContextMenuCommand = [this] { showContextMenu(slider_); };
        addAndMakeVisible(slider_);
      }
      refresh();
    }

    void setModulationSourceSubmenuCallback(
        std::function<PopupMenu(const String&, std::map<int, String>&, int)> callback) {
      modulation_source_submenu_callback_ = std::move(callback);
      updateAccessibleCommand();
    }

    void setModulationAssignCallback(std::function<void(const String&, const String&, Component&)> callback) {
      modulation_assign_callback_ = std::move(callback);
      updateAccessibleCommand();
    }

    void setModulationEditCallback(std::function<void(const String&, const String&, Component&)> callback) {
      modulation_edit_callback_ = std::move(callback);
      updateAccessibleCommand();
    }

    void setMidiLearnCallback(std::function<void(const String&, Component&, bool)> callback) {
      midi_learn_callback_ = std::move(callback);
      updateAccessibleCommand();
    }

    void setModulationDestinationPredicate(std::function<bool(const String&)> predicate) {
      is_modulation_destination_ = std::move(predicate);
      updateAccessibleCommand();
    }

    void setModulationRemovalCallbacks(std::function<std::vector<std::pair<String, String>>(const String&)> sources,
                                       std::function<void(const String&, const String&, Component&)> remove) {
      modulation_removal_sources_callback_ = std::move(sources);
      modulation_remove_callback_ = std::move(remove);
      updateAccessibleCommand();
    }

    void setValueEntryCallback(std::function<void(const String&, Component&)> callback) {
      auto* bridge = dynamic_cast<ValueBridge*>(&parameter_);
      const String parameter_id = bridge != nullptr ? bridge->getParameterId() : String();
      slider_.onTextEntryCommand = [parameter_id, callback = std::move(callback)](Component& target) {
        if (parameter_id.isNotEmpty() && callback)
          callback(parameter_id, target);
      };
    }

    void setExtraCommandCallback(std::function<bool(const String&, const KeyPress&, Component&)> callback) {
      extra_command_callback_ = std::move(callback);
      updateAccessibleCommand();
    }

    void setFineTuneCallbacks(std::function<bool()> getState, std::function<void()> toggle) {
      fine_tune_state_ = std::move(getState);
      fine_tune_toggle_ = std::move(toggle);
    }

    void notifyDisplayChanged() {
      if (auto* handler = focusableControl()->getAccessibilityHandler())
        handler->notifyAccessibilityEvent(AccessibilityEvent::valueChanged);
    }

    void refresh() {
      ScopedValueSetter<bool> guard(updating_, true);
      if (use_toggle_control_)
        toggle_.setToggleState(parameter_.getValue() >= 0.5f, dontSendNotification);
      else
        slider_.setValue(jlimit(min_normalized_value_, max_normalized_value_,
                                static_cast<double>(parameter_.getValue())), dontSendNotification);
    }

    void setAccessibleName(const String& name, const String& description = {}) {
      if (name.isEmpty())
        return;

      if (focusableControl()->getTitle() == name)
        return;

      label_.setText(name, dontSendNotification);
      label_.setTitle(name + " label");
      focusableControl()->setTitle(name);
      if (description.isNotEmpty()) {
        focusableControl()->setDescription(description);
        if (auto* tooltip = dynamic_cast<SettableTooltipClient*>(focusableControl()))
          tooltip->setTooltip(description);
      }
      if (auto* handler = focusableControl()->getAccessibilityHandler())
        handler->notifyAccessibilityEvent(AccessibilityEvent::titleChanged);
      repaint();
    }

    Component* focusableControl() {
      return use_toggle_control_ ? static_cast<Component*>(&toggle_) : static_cast<Component*>(&slider_);
    }

    String parameterId() const {
      if (auto* bridge = dynamic_cast<ValueBridge*>(&parameter_))
        return bridge->getParameterId();
      return {};
    }

    void setControlFocusOrder(int order) {
      focusableControl()->setExplicitFocusOrder(order);
    }

    void resized() override {
      auto bounds = getLocalBounds().reduced(8, 4);
      label_.setBounds(bounds.removeFromLeft(jmin(240, bounds.getWidth() / 3)));
      if (use_toggle_control_)
        toggle_.setBounds(bounds);
      else
        slider_.setBounds(bounds);
    }

  private:
    enum ContextMenuAction {
      kContextTypeValue = 1,
      kContextResetDefault,
      kContextMidiLearn,
      kContextClearMidi,
      kContextRenameMacro,
      kContextToggleBipolar,
      kContextFineTune,
      kContextAddModulationBase = 1000,
      kContextEditModulationBase = 2000,
      kContextRemoveModulationBase = 3000,
    };

    bool canAddModulation(const String& parameter_id) const {
      if (parameter_id.isEmpty())
        return false;
      if (!(modulation_source_submenu_callback_ && modulation_assign_callback_))
        return false;
      return !is_modulation_destination_ || is_modulation_destination_(parameter_id);
    }

    void showContextMenu(Component& target) {
      auto* bridge = dynamic_cast<ValueBridge*>(&parameter_);
      const String parameter_id = bridge != nullptr ? bridge->getParameterId() : String();
      if (parameter_id.isEmpty())
        return;

      const bool is_toggle = parameter_.isBoolean();
      const int macro_index = macroIndexForControlId(parameter_id);
      const auto removable_modulations = modulation_removal_sources_callback_
          ? modulation_removal_sources_callback_(parameter_id)
          : std::vector<std::pair<String, String>>();

      PopupMenu menu;
      std::map<int, String> add_modulation_choices;
      menu.addSectionHeader(parameter_.getName(128));
      if (!is_toggle && slider_.onTextEntryCommand)
        menu.addItem(kContextTypeValue, "Type a value...");
      menu.addItem(kContextResetDefault, "Reset to default");
      if (fine_tune_toggle_)
        menu.addItem(kContextFineTune, "Fine tune", true, fine_tune_state_ && fine_tune_state_());
      if (canAddModulation(parameter_id)) {
        PopupMenu add_menu = modulation_source_submenu_callback_(parameter_id, add_modulation_choices,
                                                                 kContextAddModulationBase);
        menu.addSubMenu("Add modulation source", add_menu, !add_modulation_choices.empty());
      }
      for (int i = 0; i < static_cast<int>(removable_modulations.size()); ++i)
        menu.addItem(kContextEditModulationBase + i,
                     "Edit modulation from " + removable_modulations[static_cast<size_t>(i)].second);
      for (int i = 0; i < static_cast<int>(removable_modulations.size()); ++i)
        menu.addItem(kContextRemoveModulationBase + i,
                     "Remove modulation from " + removable_modulations[static_cast<size_t>(i)].second);
      if (midi_learn_callback_) {
        menu.addItem(kContextMidiLearn, "MIDI learn");
        menu.addItem(kContextClearMidi, "Clear MIDI learn");
      }
      if (macro_index >= 0) {
        menu.addItem(kContextRenameMacro, "Rename macro\xe2\x80\xa6");
        menu.addItem(kContextToggleBipolar, "Toggle bipolar range");
      }

      Component::SafePointer<Component> safe_target(&target);
      menu.showMenuAsync(PopupMenu::Options().withTargetComponent(&target),
                         [this, parameter_id, removable_modulations, add_modulation_choices, safe_target](int result) {
        Component* invoked = safe_target.getComponent();
        if (invoked == nullptr)
          return;

        const auto add_found = add_modulation_choices.find(result);
        if (add_found != add_modulation_choices.end()) {
          if (modulation_assign_callback_)
            modulation_assign_callback_(add_found->second, parameter_id, *invoked);
          return;
        }

        if (result >= kContextEditModulationBase &&
            result < kContextEditModulationBase + static_cast<int>(removable_modulations.size())) {
          const int index = result - kContextEditModulationBase;
          if (modulation_edit_callback_ &&
              isPositiveAndBelow(index, static_cast<int>(removable_modulations.size())))
            modulation_edit_callback_(removable_modulations[static_cast<size_t>(index)].first,
                                      parameter_id, *invoked);
          return;
        }

        if (result >= kContextRemoveModulationBase &&
            result < kContextRemoveModulationBase + static_cast<int>(removable_modulations.size())) {
          const int index = result - kContextRemoveModulationBase;
          if (modulation_remove_callback_ &&
              isPositiveAndBelow(index, static_cast<int>(removable_modulations.size())))
            modulation_remove_callback_(removable_modulations[static_cast<size_t>(index)].first,
                                        parameter_id, *invoked);
          return;
        }

        switch (result) {
          case kContextTypeValue:
            if (slider_.onTextEntryCommand)
              slider_.onTextEntryCommand(*invoked);
            break;
          case kContextResetDefault:
            resetToDefaultValue();
            break;
          case kContextMidiLearn:
            if (midi_learn_callback_)
              midi_learn_callback_(parameter_id, *invoked, false);
            break;
          case kContextClearMidi:
            if (midi_learn_callback_)
              midi_learn_callback_(parameter_id, *invoked, true);
            break;
          case kContextRenameMacro:
            if (extra_command_callback_)
              extra_command_callback_(parameter_id,
                                      KeyPress(KeyPress::returnKey, ModifierKeys::shiftModifier, 0), *invoked);
            break;
          case kContextToggleBipolar:
            if (extra_command_callback_)
              extra_command_callback_(parameter_id, KeyPress('b', ModifierKeys(), 'b'), *invoked);
            break;
          case kContextFineTune:
            if (fine_tune_toggle_)
              fine_tune_toggle_();
            break;
          default:
            break;
        }
      });
    }

    void resetToDefaultValue() {
      const float default_value = jlimit(static_cast<float>(min_normalized_value_),
                                        static_cast<float>(max_normalized_value_),
                                        parameter_.getDefaultValue());
      parameter_.beginChangeGesture();
      parameter_.setValueNotifyingHost(default_value);
      parameter_.endChangeGesture();
      refresh();
      String value_text = parameter_.getText(parameter_.getValue(), 128);
      if (auto* bridge = dynamic_cast<ValueBridge*>(&parameter_))
        value_text = accessibleParameterText(*bridge, parameter_.getValue());
      postPluginAnnouncement(parameter_.getName(128) + " reset to " + value_text,
                             AccessibilityHandler::AnnouncementPriority::high);
    }

    AudioProcessorParameter& parameter_;
    Label label_;
    OffscreenSlider slider_;
    OffscreenToggleButton toggle_;
    std::function<String(double)> text_from_value_;
    std::function<PopupMenu(const String&, std::map<int, String>&, int)> modulation_source_submenu_callback_;
    std::function<void(const String&, const String&, Component&)> modulation_assign_callback_;
    std::function<void(const String&, const String&, Component&)> modulation_edit_callback_;
    std::function<bool(const String&)> is_modulation_destination_;
    std::function<std::vector<std::pair<String, String>>(const String&)> modulation_removal_sources_callback_;
    std::function<void(const String&, const String&, Component&)> modulation_remove_callback_;
    std::function<void(const String&, Component&, bool)> midi_learn_callback_;
    std::function<bool(const String&, const KeyPress&, Component&)> extra_command_callback_;
    std::function<bool()> fine_tune_state_;
    std::function<void()> fine_tune_toggle_;
    double min_normalized_value_ = 0.0;
    double max_normalized_value_ = 1.0;
    bool use_toggle_control_ = false;
    bool updating_ = false;

    void updateAccessibleCommand() {
      auto* bridge = dynamic_cast<ValueBridge*>(&parameter_);
      const String parameter_id = bridge != nullptr ? bridge->getParameterId() : String();
      auto command = [this, parameter_id](const KeyPress& key, Component& target) {
        if (parameter_id.isEmpty())
          return false;

        if (extra_command_callback_ && extra_command_callback_(parameter_id, key, target))
          return true;

        if (isModulationMenuKey(key)) {
          showContextMenu(target);
          return true;
        }

        if (isMidiLearnKey(key) && midi_learn_callback_) {
          midi_learn_callback_(parameter_id, target, false);
          return true;
        }

        if (isMidiClearKey(key) && midi_learn_callback_) {
          midi_learn_callback_(parameter_id, target, true);
          return true;
        }

        return false;
      };

      slider_.onAccessibleCommand = command;
      toggle_.onAccessibleCommand = std::move(command);
    }
};

class AccessibleSectionHeader : public Component {
  public:
    std::function<bool(const KeyPress&, Component&)> onKeyCommand;

    AccessibleSectionHeader(const String& title, int groupRank, bool topLevel, const String& sectionKey = {}) :
        title_(title), top_level_(topLevel) {
      setTitle(title_);
      setDescription(top_level_ ? "Control group " + title_ : "Control section " + title_);
      setHelpText(top_level_ ? "Group heading" : "Section heading");
      setAccessible(true);
      setWantsKeyboardFocus(true);
      setFocusContainerType(Component::FocusContainerType::focusContainer);
      getProperties().set("ControlGroup", groupRank);
      if (sectionKey.isNotEmpty())
        getProperties().set("SectionName", sectionKey);
    }

    void paint(Graphics& g) override {
      const auto bounds = getLocalBounds().toFloat().reduced(2.0f);
      g.setColour(top_level_ ? Colour(0xff28313d) : Colour(0xff20262f));
      g.fillRoundedRectangle(bounds, 5.0f);
      g.setColour(Colours::white);
      g.setFont(FontOptions(top_level_ ? 18.0f : 15.0f, top_level_ ? Font::bold : Font::plain));
      g.drawText(title_, getLocalBounds().reduced(top_level_ ? 12 : 22, 0),
                 Justification::centredLeft, true);
    }

    bool keyPressed(const KeyPress& key) override {
      if (onKeyCommand && onKeyCommand(key, *this))
        return true;
      return Component::keyPressed(key);
    }

    std::unique_ptr<AccessibilityHandler> createAccessibilityHandler() override {
      class HeaderAccessibilityHandler final : public AccessibilityHandler {
        public:
          explicit HeaderAccessibilityHandler(AccessibleSectionHeader& header) :
              AccessibilityHandler(header, AccessibilityRole::group) { }

          AccessibleState getCurrentState() const override {
            return AccessibilityHandler::getCurrentState().withAccessibleOffscreen();
          }
      };
      return std::make_unique<HeaderAccessibilityHandler>(*this);
    }

    std::unique_ptr<ComponentTraverser> createFocusTraverser() override {
      return std::make_unique<ChildListTraverser>(child_focus_order_);
    }

    void addAccessibleChild(Component* component) {
      if (component != nullptr)
        child_focus_order_.push_back(component);
    }

    void setHeaderTitle(const String& title) {
      if (title.isEmpty() || title == title_)
        return;

      title_ = title;
      setTitle(title_);
      setDescription(top_level_ ? "Control group " + title_ : "Control section " + title_);
      if (auto* handler = getAccessibilityHandler())
        handler->notifyAccessibilityEvent(AccessibilityEvent::titleChanged);
      repaint();
    }

    int headerHeight() const { return top_level_ ? 42 : 34; }

  private:
    String title_;
    bool top_level_ = false;
    std::vector<Component*> child_focus_order_;
};

SynthEditor::SynthEditor(SynthPlugin& synth) :
    AudioProcessorEditor(&synth), SynthGuiInterface(&synth, false), synth_(synth) {
  setTitle("Atlas");
  setDescription("Keyboard and VoiceOver accessible editor for Atlas");
  setHelpText("Use Group to choose the editor area, Element to choose the oscillator, filter, envelope, LFO, effect, or routing page, then Tab through visible controls. Press comma or period to move between elements.");
  setWantsKeyboardFocus(true);
  setFocusContainerType(Component::FocusContainerType::keyboardFocusContainer);

  title_.setText("Atlas", dontSendNotification);
  title_.setFont(FontOptions(24.0f, Font::bold));
  title_.setTitle("Atlas");
  title_.setColour(Label::textColourId, Colours::white);
  addAndMakeVisible(title_);

  instructions_.setText("Choose a group and element, then use Tab and arrow keys. Comma and period move between elements. On a control, press Shift M, the right bracket key, or VoiceOver Shift M for its context menu.",
                        dontSendNotification);
  instructions_.setTitle("Keyboard instructions");
  instructions_.setColour(Label::textColourId, Colours::white);
  addAndMakeVisible(instructions_);

  menu_button_.setTitle("Editor menu");
  menu_button_.setDescription("Open patch and accessibility settings");
  menu_button_.setHelpText("Press Enter to save the current patch, initialize the patch, or change accessibility settings");
  menu_button_.setWantsKeyboardFocus(true);
  menu_button_.onClick = [this] { showNavigationMenu(); };
  addAndMakeVisible(menu_button_);

  group_selector_.setTitle("Editor group");
  group_selector_.setDescription("Choose a top-level control group such as oscillators, filters, effects, LFOs, or modulation");
  group_selector_.setHelpText("Use Up and Down Arrow to choose a group. The Element picker changes to the controls inside this group.");
  group_selector_.setWantsKeyboardFocus(true);
  group_selector_.onChange = [this] {
    if (updating_navigation_)
      return;
    const String group = group_selector_.getText();
    populateElementSelectorForGroup(group);
    const auto found = group_sections_.find(group);
    if (found != group_sections_.end() && found->second.size() > 0)
      selectSectionByName(found->second[0], true);
  };
  addAndMakeVisible(group_selector_);

  section_selector_.setTitle("Editor element");
  section_selector_.setDescription("Choose the specific element inside the selected group");
  section_selector_.setHelpText("Use Up and Down Arrow to choose an element, then press Tab to reach its visible controls");
  section_selector_.setWantsKeyboardFocus(true);
  section_selector_.onChange = [this] {
    if (updating_navigation_)
      return;
    const String group = group_selector_.getText();
    const int element_index = section_selector_.getSelectedItemIndex();
    const auto found = group_sections_.find(group);
    if (found != group_sections_.end() && isPositiveAndBelow(element_index, found->second.size()))
      selectSectionByName(found->second[element_index], true);
  };
  addAndMakeVisible(section_selector_);

  preset_summary_.setTitle("Preset summary");
  preset_summary_.setDescription("Current preset and Atlas resources path");
  preset_summary_.setColour(Label::textColourId, Colours::white);
  preset_summary_.setJustificationType(Justification::centredLeft);
  addChildComponent(preset_summary_);

  preset_menu_.setTitle("Preset menu");
  preset_menu_.setDescription("Open preset browser actions including refresh, open preset file, import bank, and export bank");
  preset_menu_.setHelpText("Press Enter to open preset browser actions");
  preset_menu_.setWantsKeyboardFocus(true);
  preset_menu_.onClick = [this] { showPresetMenu(); };
  addChildComponent(preset_menu_);

  preset_library_.setTitle("Preset library");
  preset_library_.setDescription("Filter presets by factory, user, or other installed libraries");
  preset_library_.setHelpText("Choose all libraries, factory, user, or other preset locations");
  preset_library_.setWantsKeyboardFocus(true);
  preset_library_.onFocusGained = [this] { ensurePresetListLoaded(); };
  preset_library_.onChange = [this] {
    if (updating_preset_list_)
      return;
    if (!preset_list_loaded_) {
      ensurePresetListLoaded();
      return;
    }
    last_preset_library = preset_library_.getText();
    last_preset_bank = kAllBanks;
    last_preset_category = kAllCategories;
    schedulePresetFilterUpdate(true);
  };
  addChildComponent(preset_library_);

  preset_bank_.setTitle("Preset bank");
  preset_bank_.setDescription("Filter presets by bank or sound pack");
  preset_bank_.setHelpText("Choose all banks or one installed bank");
  preset_bank_.setWantsKeyboardFocus(true);
  preset_bank_.onFocusGained = [this] { ensurePresetListLoaded(); };
  preset_bank_.onChange = [this] {
    if (updating_preset_list_)
      return;
    if (!preset_list_loaded_) {
      ensurePresetListLoaded();
      return;
    }
    last_preset_bank = preset_bank_.getText();
    last_preset_category = kAllCategories;
    schedulePresetFilterUpdate(true);
  };
  addChildComponent(preset_bank_);

  preset_category_.setTitle("Preset category");
  preset_category_.setDescription("Filter presets by category when the bank provides one");
  preset_category_.setHelpText("Choose all categories or one category");
  preset_category_.setWantsKeyboardFocus(true);
  preset_category_.onFocusGained = [this] { ensurePresetListLoaded(); };
  preset_category_.onChange = [this] {
    if (updating_preset_list_)
      return;
    if (!preset_list_loaded_) {
      ensurePresetListLoaded();
      return;
    }
    last_preset_category = preset_category_.getText();
    schedulePresetFilterUpdate(false);
  };
  addChildComponent(preset_category_);

  preset_search_.setTitle("Preset search");
  preset_search_.setDescription("Filter presets by name, folder, author, category, or tags");
  preset_search_.setHelpText("Type text to narrow the preset list");
  preset_search_.setTextToShowWhenEmpty("Search presets", Colours::grey);
  preset_search_.setWantsKeyboardFocus(true);
  preset_search_.setText(last_preset_search, false);
  preset_search_.onTextChange = [this] {
    last_preset_search = preset_search_.getText();
    if (!preset_list_loaded_) {
      ensurePresetListLoaded();
      return;
    }
    schedulePresetFilterUpdate(false);
  };
  addChildComponent(preset_search_);

  preset_selector_.setTitle("Preset list");
  preset_selector_.setDescription("Factory and user presets from Atlas's resources folder. Press Enter to load.");
  preset_selector_.setHelpText("Choose a preset, then press Enter to load it. If autoload is enabled, changing selection loads immediately.");
  preset_selector_.setWantsKeyboardFocus(true);
  preset_selector_.onFocusGained = [this] { ensurePresetListLoaded(); };
  preset_selector_.onReturnKey = [this] { loadSelectedPreset(); };
  preset_selector_.onChange = [this] {
    if (updating_preset_list_)
      return;
    if (!preset_list_loaded_) {
      ensurePresetListLoaded();
      return;
    }
    const int index = preset_selector_.getSelectedItemIndex();
    if (isPositiveAndBelow(index, filtered_presets_.size()))
      last_preset_path = filtered_presets_[index].getFullPathName();
    if (preset_preview_.getToggleState() && isPositiveAndBelow(index, filtered_presets_.size()))
      loadPresetFile(filtered_presets_[index], true, false);
  };
  addChildComponent(preset_selector_);

  preset_preview_.setTitle("Autoload preset when scrolling");
  preset_preview_.setDescription("Automatically load presets as you move through the preset list");
  preset_preview_.setHelpText("Turn this on to load presets while scrolling. Leave it off to load only when pressing Enter.");
  preset_preview_.setWantsKeyboardFocus(true);
  preset_preview_.setToggleState(last_preset_preview, dontSendNotification);
  preset_preview_.onClick = [this] {
    last_preset_preview = preset_preview_.getToggleState();
    postPluginAnnouncement(last_preset_preview ? "Preset autoload enabled" : "Preset autoload disabled",
                                           AccessibilityHandler::AnnouncementPriority::medium);
  };
  addChildComponent(preset_preview_);

  preset_name_editor_.setTitle("Preset name");
  preset_name_editor_.setDescription("Name to suggest when saving the patch");
  preset_name_editor_.setHelpText("Type a preset name, then use Editor menu, Save patch as");
  preset_name_editor_.setTextToShowWhenEmpty("Preset name", Colours::grey);
  preset_name_editor_.setWantsKeyboardFocus(true);
  addChildComponent(preset_name_editor_);

  save_patch_prompt_.setTitle("Save patch");
  save_patch_prompt_.setDescription("Save the current Atlas patch");
  save_patch_prompt_.setColour(Label::textColourId, Colours::white);
  save_patch_prompt_.setJustificationType(Justification::centredLeft);
  addChildComponent(save_patch_prompt_);

  auto configureSaveEditor = [this](TextEditor& editor, const String& title, const String& empty_text) {
    editor.setTitle(title);
    editor.setDescription(title);
    editor.setHelpText("Type " + title.toLowerCase());
    editor.setTextToShowWhenEmpty(empty_text, Colours::grey);
    editor.setSelectAllWhenFocused(true);
    editor.setWantsKeyboardFocus(true);
    addChildComponent(editor);
  };
  configureSaveEditor(save_patch_name_, "Patch name", "Name");
  configureSaveEditor(save_patch_author_, "Patch author", "Author");
  configureSaveEditor(save_patch_bank_, "Patch bank", "Bank");
  configureSaveEditor(save_patch_category_, "Patch category", "Category");
  configureSaveEditor(save_patch_tags_, "Patch tags", "Tags separated by commas");
  save_patch_ok_.setTitle("Save patch");
  save_patch_ok_.setDescription("Save patch to the user library");
  save_patch_ok_.setHelpText("Press Enter to save the patch");
  save_patch_ok_.setWantsKeyboardFocus(true);
  save_patch_ok_.onClick = [this] { commitSavePresetDialog(); };
  addChildComponent(save_patch_ok_);
  save_patch_cancel_.setTitle("Cancel save patch");
  save_patch_cancel_.setDescription("Close the save patch form without saving");
  save_patch_cancel_.setHelpText("Press Enter to cancel");
  save_patch_cancel_.setWantsKeyboardFocus(true);
  save_patch_cancel_.onClick = [this] { hideSavePresetDialog(); };
  addChildComponent(save_patch_cancel_);

  wavetable_name_.setTitle("Current wavetable");
  wavetable_name_.setDescription("Name of the wavetable loaded in the selected oscillator");
  wavetable_name_.setColour(Label::textColourId, Colours::white);
  addChildComponent(wavetable_name_);

  load_wavetable_.setTitle("Load wavetable");
  load_wavetable_.setDescription("Load an Atlas wavetable or audio file into the selected oscillator");
  load_wavetable_.setHelpText("Press Enter, then choose a wavetable, WAV, or FLAC file");
  load_wavetable_.setWantsKeyboardFocus(true);
  load_wavetable_.onClick = [this] { chooseWavetableFile(); };
  addChildComponent(load_wavetable_);

  reset_wavetable_.setTitle("Reset wavetable");
  reset_wavetable_.setDescription("Restore the selected oscillator to the default wavetable");
  reset_wavetable_.setHelpText("Press Enter to restore the default wavetable");
  reset_wavetable_.setWantsKeyboardFocus(true);
  reset_wavetable_.onClick = [this] {
    if (auto* creator = synth_.getWavetableCreator(active_oscillator_)) {
      synth_.pauseProcessing(true);
      creator->loadDefaultCreator();
      creator->render();
      synth_.pauseProcessing(false);
      wavetable_name_.setText("Current wavetable: " + String(creator->getName()), dontSendNotification);
      postPluginAnnouncement("Default wavetable loaded in oscillator " +
                                               String(active_oscillator_ + 1),
                                               AccessibilityHandler::AnnouncementPriority::high);
    }
  };
  addChildComponent(reset_wavetable_);

  oscillator_summary_.setTitle("Oscillator summary");
  oscillator_summary_.setDescription("Musical oscillator controls for wavetable position and tuning");
  oscillator_summary_.setColour(Label::textColourId, Colours::white);
  oscillator_summary_.setJustificationType(Justification::centredLeft);
  addChildComponent(oscillator_summary_);

  oscillator_octave_.setTitle("Octave");
  oscillator_octave_.setDescription("Transpose the selected oscillator in octaves");
  oscillator_octave_.setHelpText("Choose an octave offset from minus four to plus four");
  oscillator_octave_.setWantsKeyboardFocus(true);
  for (int octave = -4; octave <= 4; ++octave)
    oscillator_octave_.addItem((octave > 0 ? "+" : "") + String(octave), octave + 5);
  oscillator_octave_.onChange = [this] {
    if (!updating_oscillator_controls_)
      setOscillatorTransposeFromDirectControls();
  };
  addChildComponent(oscillator_octave_);

  oscillator_semitone_.setTitle("Semitone");
  oscillator_semitone_.setDescription("Transpose the selected oscillator in semitones inside the octave");
  oscillator_semitone_.setHelpText("Choose a semitone offset from minus eleven to plus eleven");
  oscillator_semitone_.setWantsKeyboardFocus(true);
  for (int semitone = -11; semitone <= 11; ++semitone)
    oscillator_semitone_.addItem((semitone > 0 ? "+" : "") + String(semitone), semitone + 12);
  oscillator_semitone_.onChange = [this] {
    if (!updating_oscillator_controls_)
      setOscillatorTransposeFromDirectControls();
  };
  addChildComponent(oscillator_semitone_);

  oscillator_fine_tune_.setSliderStyle(Slider::LinearHorizontal);
  oscillator_fine_tune_.setTextBoxStyle(Slider::TextBoxRight, false, 96, 28);
  oscillator_fine_tune_.setRange(-100.0, 100.0, 1.0);
  oscillator_fine_tune_.setTitle("Fine tune");
  oscillator_fine_tune_.setDescription("Fine tune the selected oscillator in cents");
  oscillator_fine_tune_.setHelpText("Use Left and Right Arrow to adjust fine tune");
  oscillator_fine_tune_.setTextValueSuffix(" cents");
  oscillator_fine_tune_.setWantsKeyboardFocus(true);
  oscillator_fine_tune_.onValueChange = [this] {
    if (!updating_oscillator_controls_ && active_oscillator_ >= 0)
      setParameterEngineValue("osc_" + String(active_oscillator_ + 1) + "_tune",
                              static_cast<float>(oscillator_fine_tune_.getValue() / 100.0));
  };
  addChildComponent(oscillator_fine_tune_);

  oscillator_wave_frame_.setSliderStyle(Slider::LinearHorizontal);
  oscillator_wave_frame_.setTextBoxStyle(Slider::TextBoxRight, false, 96, 28);
  oscillator_wave_frame_.setRange(0.0, vital::kNumOscillatorWaveFrames - 1, 1.0);
  oscillator_wave_frame_.setTitle("Wave frame");
  oscillator_wave_frame_.setDescription("Move through the selected oscillator wavetable frames");
  oscillator_wave_frame_.setHelpText("Use Left and Right Arrow to scan the wavetable position");
  oscillator_wave_frame_.setWantsKeyboardFocus(true);
  oscillator_wave_frame_.onValueChange = [this] {
    if (!updating_oscillator_controls_ && active_oscillator_ >= 0)
      setParameterEngineValue("osc_" + String(active_oscillator_ + 1) + "_wave_frame",
                              static_cast<float>(oscillator_wave_frame_.getValue()));
  };
  addChildComponent(oscillator_wave_frame_);

  oscillator_scale_key_.setTitle("Transpose quantize key");
  oscillator_scale_key_.setDescription("Choose the key used by oscillator transpose quantize");
  oscillator_scale_key_.setHelpText("Choose the root note for transpose quantize");
  oscillator_scale_key_.setWantsKeyboardFocus(true);
  for (int key = 0; key < vital::kNotesPerOctave; ++key)
    oscillator_scale_key_.addItem(transposeKeyName(key), key + 1);
  oscillator_scale_key_.onChange = [this] {
    if (!updating_oscillator_controls_)
      setOscillatorScaleFromDirectControls();
  };
  addChildComponent(oscillator_scale_key_);

  oscillator_scale_type_.setTitle("Transpose quantize scale");
  oscillator_scale_type_.setDescription("Choose the scale used by oscillator transpose quantize");
  oscillator_scale_type_.setHelpText("Choose Off, Chromatic, Major, Minor, Pentatonic, Blues, or a mode");
  oscillator_scale_type_.setWantsKeyboardFocus(true);
  const auto& scale_choices = transposeQuantizeScales();
  for (int i = 0; i < static_cast<int>(scale_choices.size()); ++i)
    oscillator_scale_type_.addItem(scale_choices[i].name, i + 1);
  oscillator_scale_type_.onChange = [this] {
    if (!updating_oscillator_controls_)
      setOscillatorScaleFromDirectControls();
  };
  addChildComponent(oscillator_scale_type_);

  oscillator_scale_mode_.setTitle("Transpose quantize snap mode");
  oscillator_scale_mode_.setDescription("Choose whether transpose quantize snaps relative to played notes or to global pitch classes");
  oscillator_scale_mode_.setHelpText("Local snaps oscillator transpose relative to the played note. Global snaps the final pitch to the selected key and scale.");
  oscillator_scale_mode_.setWantsKeyboardFocus(true);
  oscillator_scale_mode_.addItem("Local", 1);
  oscillator_scale_mode_.addItem("Global", 2);
  oscillator_scale_mode_.onChange = [this] {
    if (!updating_oscillator_controls_)
      setOscillatorScaleFromDirectControls();
  };
  addChildComponent(oscillator_scale_mode_);

  routing_summary_.setTitle("Routing summary");
  routing_summary_.setDescription("Summary of oscillator, sample, filter, and effects routing");
  routing_summary_.setColour(Label::textColourId, Colours::white);
  routing_summary_.setJustificationType(Justification::centredLeft);
  addChildComponent(routing_summary_);

  routing_mode_.setTitle("Filter routing mode");
  routing_mode_.setDescription("Choose how filter 1 and filter 2 are connected");
  routing_mode_.setHelpText("Choose parallel filters, filter 1 into filter 2, or filter 2 into filter 1");
  routing_mode_.setWantsKeyboardFocus(true);
  routing_mode_.addItemList({ "Parallel filters", "Filter 1 into filter 2", "Filter 2 into filter 1" }, 1);
  routing_mode_.onChange = [this] {
    if (!updating_routing_controls_)
      applyRoutingPreset(routing_mode_.getSelectedItemIndex());
  };
  addChildComponent(routing_mode_);

  routing_default_.setTitle("Default routing");
  routing_default_.setDescription("Set filters to parallel routing and restore the default oscillator and sample destinations");
  routing_default_.setHelpText("Press Enter to apply default routing");
  routing_default_.setWantsKeyboardFocus(true);
  routing_default_.onClick = [this] { applyRoutingPreset(3); };
  addChildComponent(routing_default_);

  routing_serial_forward_.setTitle("Serial filters 1 into 2");
  routing_serial_forward_.setDescription("Send filter 1 into filter 2");
  routing_serial_forward_.setHelpText("Press Enter to route filter 1 into filter 2");
  routing_serial_forward_.setWantsKeyboardFocus(true);
  routing_serial_forward_.onClick = [this] { applyRoutingPreset(1); };
  addChildComponent(routing_serial_forward_);

  routing_serial_backward_.setTitle("Serial filters 2 into 1");
  routing_serial_backward_.setDescription("Send filter 2 into filter 1");
  routing_serial_backward_.setHelpText("Press Enter to route filter 2 into filter 1");
  routing_serial_backward_.setWantsKeyboardFocus(true);
  routing_serial_backward_.onClick = [this] { applyRoutingPreset(2); };
  addChildComponent(routing_serial_backward_);

  effect_chain_summary_.setTitle("Effects chain order");
  effect_chain_summary_.setDescription("Current order of the effects chain");
  effect_chain_summary_.setColour(Label::textColourId, Colours::white);
  effect_chain_summary_.setJustificationType(Justification::centredLeft);
  addChildComponent(effect_chain_summary_);

  effect_chain_selector_.setTitle("Effect position");
  effect_chain_selector_.setDescription("Choose an effect in the current chain order");
  effect_chain_selector_.setHelpText("Choose an effect, then use the move buttons to reorder it in the chain");
  effect_chain_selector_.setWantsKeyboardFocus(true);
  addChildComponent(effect_chain_selector_);

  post_effect_order_selector_.setTitle("Post effect order");
  post_effect_order_selector_.setDescription("Choose the order of the two added post effects");
  post_effect_order_selector_.setHelpText("Choose Frequency Shifter then Limiter, or Limiter then Frequency Shifter");
  post_effect_order_selector_.setWantsKeyboardFocus(true);
  post_effect_order_selector_.addItem("Frequency Shifter then Limiter", 1);
  post_effect_order_selector_.addItem("Limiter then Frequency Shifter", 2);
  post_effect_order_selector_.onChange = [this] {
    const int selected = post_effect_order_selector_.getSelectedItemIndex();
    if (!isPositiveAndBelow(selected, 2))
      return;

    setParameterEngineValue(effectChainPrefixForSection(last_section_name) + "post_effect_order",
                            static_cast<float>(selected));
    refreshEffectChainControls();
    postPluginAnnouncement(post_effect_order_selector_.getText(),
                                           AccessibilityHandler::AnnouncementPriority::high);
  };
  addChildComponent(post_effect_order_selector_);

  effect_move_up_.setTitle("Move effect earlier");
  effect_move_up_.setDescription("Move the selected effect one position earlier in the effects chain");
  effect_move_up_.setHelpText("Press Enter to move the selected effect earlier");
  effect_move_up_.setWantsKeyboardFocus(true);
  effect_move_up_.onClick = [this] { moveSelectedEffect(-1); };
  addChildComponent(effect_move_up_);

  effect_move_down_.setTitle("Move effect later");
  effect_move_down_.setDescription("Move the selected effect one position later in the effects chain");
  effect_move_down_.setHelpText("Press Enter to move the selected effect later");
  effect_move_down_.setWantsKeyboardFocus(true);
  effect_move_down_.onClick = [this] { moveSelectedEffect(1); };
  addChildComponent(effect_move_down_);

  modulation_source_.setTitle("Modulation source");
  modulation_source_.setDescription("Choose an envelope, LFO, macro, MIDI source, or other modulation source");
  modulation_source_.setHelpText("Use Up and Down Arrow to choose a source");
  modulation_source_.setWantsKeyboardFocus(true);
  modulation_source_.onChange = [this] {
    if (!updating_modulation_destinations_)
      updateModulationDestinationList();
  };
  addChildComponent(modulation_source_);

  modulation_destination_group_.setTitle("Destination group");
  modulation_destination_group_.setDescription("Choose a category of modulation destinations");
  modulation_destination_group_.setHelpText("Choose a group first, then choose a destination inside that group");
  modulation_destination_group_.setWantsKeyboardFocus(true);
  modulation_destination_group_.onChange = [this] {
    if (!updating_modulation_destinations_)
      updateModulationDestinationList();
  };
  addChildComponent(modulation_destination_group_);

  modulation_destination_.setTitle("Modulation destination");
  modulation_destination_.setDescription("Choose the synthesizer parameter to modulate inside the selected group");
  modulation_destination_.setHelpText("Choose a destination. Change Destination group to shorten this list.");
  modulation_destination_.setWantsKeyboardFocus(true);
  addChildComponent(modulation_destination_);

  for (const auto& source : synth_.getEngine()->getModulationSources())
    modulation_source_ids_.add(source.first);
  std::vector<String> sorted_sources;
  for (const auto& id : modulation_source_ids_)
    sorted_sources.push_back(id);
  std::stable_sort(sorted_sources.begin(), sorted_sources.end(), [](const String& a, const String& b) {
    const int rank_a = modulationSourceSortRank(a);
    const int rank_b = modulationSourceSortRank(b);
    if (rank_a != rank_b)
      return rank_a < rank_b;
    return modulationSourceLabelForId(a).compareNatural(modulationSourceLabelForId(b)) < 0;
  });
  modulation_source_ids_.clear();
  for (const auto& id : sorted_sources)
    modulation_source_ids_.add(id);
  for (const auto& id : modulation_source_ids_) {
    const int macro_index = macroIndexForControlId(id);
    const String label = macro_index >= 0 ? synth_.getMacroName(macro_index)
                                          : modulationSourceLabelForId(id);
    modulation_source_.addItem(label, modulation_source_.getNumItems() + 1);
  }

  populateModulationDestinations();

  add_modulation_.setTitle("Connect modulation");
  add_modulation_.setDescription("Connect the selected source to the selected destination");
  add_modulation_.setHelpText("Choose a source and destination, then press Enter");
  add_modulation_.setWantsKeyboardFocus(true);
  add_modulation_.onClick = [this] {
    const int source = modulation_source_.getSelectedItemIndex();
    const int destination = modulation_destination_.getSelectedItemIndex();
    if (!isPositiveAndBelow(source, modulation_source_ids_.size()) ||
        !isPositiveAndBelow(destination, modulation_destination_ids_.size())) {
      postPluginAnnouncement("Choose both a modulation source and destination",
                                               AccessibilityHandler::AnnouncementPriority::high);
      return;
    }
    connectModulationAndPromptForAmount(modulation_source_ids_[source], modulation_destination_ids_[destination],
                                        add_modulation_);
  };
  addChildComponent(add_modulation_);

  remove_modulation_.setTitle("Remove selected modulation");
  remove_modulation_.setDescription("Disconnect the selected modulation route");
  remove_modulation_.setHelpText("Select a route in the list, then press Enter");
  remove_modulation_.setWantsKeyboardFocus(true);
  remove_modulation_.onClick = [this] {
    const int row = modulation_list_.getSelectedRow();
    if (isPositiveAndBelow(row, static_cast<int>(modulation_routes_.size()))) {
      synth_.disconnectModulation(modulation_routes_[row]);
      refreshModulationRoutes();
      postPluginAnnouncement("Modulation removed",
                                               AccessibilityHandler::AnnouncementPriority::high);
    }
  };
  addChildComponent(remove_modulation_);

  modulation_list_.setTitle("Connected modulations");
  modulation_list_.setDescription("List of modulation source and destination routes");
  modulation_list_.setHelpText("Use Up and Down Arrow to select a route. Its amount and options appear below.");
  modulation_list_.setWantsKeyboardFocus(true);
  modulation_list_.setRowHeight(30);
  addChildComponent(modulation_list_);

  modulation_amount_prompt_.setTitle("Initial modulation amount");
  modulation_amount_prompt_.setDescription("Type the maximum modulation amount from minus one to one");
  modulation_amount_prompt_.setColour(Label::textColourId, Colours::white);
  modulation_amount_prompt_.setJustificationType(Justification::centredLeft);
  addChildComponent(modulation_amount_prompt_);

  modulation_amount_editor_.setTitle("Maximum modulation amount");
  modulation_amount_editor_.setDescription("Maximum modulation amount from minus one to one");
  modulation_amount_editor_.setHelpText("Type a value from minus one to one, then press Return or OK");
  modulation_amount_editor_.setWantsKeyboardFocus(true);
  modulation_amount_editor_.onReturnKey = [this] { applyInlineTextPrompt(); };
  addChildComponent(modulation_amount_editor_);

  modulation_amount_ok_.setTitle("Set modulation amount");
  modulation_amount_ok_.setDescription("Apply the typed initial modulation amount");
  modulation_amount_ok_.setWantsKeyboardFocus(true);
  modulation_amount_ok_.onClick = [this] { applyInlineTextPrompt(); };
  addChildComponent(modulation_amount_ok_);

  modulation_amount_cancel_.setTitle("Cancel modulation amount");
  modulation_amount_cancel_.setDescription("Leave the modulation connected without changing the amount");
  modulation_amount_cancel_.setWantsKeyboardFocus(true);
  modulation_amount_cancel_.onClick = [this] { cancelInlineTextPrompt(); };
  addChildComponent(modulation_amount_cancel_);

  lfo_mseg_summary_.setTitle("Accessible LFO MSEG summary");
  lfo_mseg_summary_.setDescription("Current LFO shape and selected point");
  lfo_mseg_summary_.setColour(Label::textColourId, Colours::white);
  lfo_mseg_summary_.setJustificationType(Justification::centredLeft);
  addChildComponent(lfo_mseg_summary_);

  lfo_mseg_lfo_.setTitle("LFO to edit");
  lfo_mseg_lfo_.setDescription("Choose which Vital LFO shape to edit as an accessible MSEG");
  lfo_mseg_lfo_.setHelpText("Choose an LFO, then edit its points below");
  lfo_mseg_lfo_.setWantsKeyboardFocus(true);
  for (int i = 0; i < vital::kNumLfos; ++i)
    lfo_mseg_lfo_.addItem("LFO " + String(i + 1), i + 1);
  lfo_mseg_lfo_.setSelectedItemIndex(0, dontSendNotification);
  lfo_mseg_lfo_.onChange = [this] {
    if (updating_lfo_mseg_controls_)
      return;
    active_lfo_index_ = jlimit(0, vital::kNumLfos - 1, lfo_mseg_lfo_.getSelectedItemIndex());
    selectSectionByName("LFO " + String(active_lfo_index_ + 1), true);
    refreshLfoMsegControls();
    postLfoAnnouncement("Editing LFO " + String(active_lfo_index_ + 1));
  };
  addChildComponent(lfo_mseg_lfo_);

  lfo_mseg_mode_.setTitle("Mode");
  lfo_mseg_mode_.setDescription("LFO editor mode");
  lfo_mseg_mode_.setHelpText("Loop edits the normal looping Vital LFO. One shot and retrigger are editor labels for compatibility with ModWarp shortcuts.");
  lfo_mseg_mode_.setWantsKeyboardFocus(true);
  lfo_mseg_mode_.addItemList({ "Loop", "One shot", "Retrigger per MIDI note" }, 1);
  lfo_mseg_mode_.setSelectedItemIndex(0, dontSendNotification);
  lfo_mseg_mode_.onChange = [this] {
    if (!updating_lfo_mseg_controls_)
      applyLfoMode();
  };
  addChildComponent(lfo_mseg_mode_);

  lfo_mseg_cycle_.setTitle("Cycle Length");
  lfo_mseg_cycle_.setDescription("Length of the complete LFO shape");
  lfo_mseg_cycle_.setHelpText("Semicolon and apostrophe change cycle length from the keyboard editor");
  lfo_mseg_cycle_.setWantsKeyboardFocus(true);
  lfo_mseg_cycle_.addItemList(msegTimeDivisionChoices(), 1);
  lfo_mseg_cycle_.setSelectedItemIndex(lfo_cycle_index_, dontSendNotification);
  lfo_mseg_cycle_.onChange = [this] {
    if (updating_lfo_mseg_controls_)
      return;
    lfo_cycle_index_ = jlimit(0, static_cast<int>(msegTimeDivisions().size()) - 1,
                              lfo_mseg_cycle_.getSelectedItemIndex());
    applyLfoCycleLength();
    updateLfoMsegSummary();
  };
  addChildComponent(lfo_mseg_cycle_);

  lfo_mseg_grid_.setTitle("Editor Grid");
  lfo_mseg_grid_.setDescription("Keyboard movement size for the LFO editor");
  lfo_mseg_grid_.setHelpText("Minus makes the grid coarser. Equals makes it finer.");
  lfo_mseg_grid_.setWantsKeyboardFocus(true);
  lfo_mseg_grid_.addItemList(msegTimeDivisionChoices(), 1);
  lfo_mseg_grid_.setSelectedItemIndex(lfo_grid_index_, dontSendNotification);
  lfo_mseg_grid_.onChange = [this] {
    if (updating_lfo_mseg_controls_)
      return;
    lfo_grid_index_ = jlimit(0, static_cast<int>(msegTimeDivisions().size()) - 1,
                             lfo_mseg_grid_.getSelectedItemIndex());
    updateLfoMsegSummary();
    postLfoAnnouncement("Grid " + lfo_mseg_grid_.getText() + ". " + lfoMsegStatusText());
  };
  addChildComponent(lfo_mseg_grid_);

  lfo_mseg_shape_.setTitle("LFO shape preset");
  lfo_mseg_shape_.setDescription("Choose a standard LFO shape or MSEG starting point");
  lfo_mseg_shape_.setHelpText("Choose a shape, then press Apply shape");
  lfo_mseg_shape_.setWantsKeyboardFocus(true);
  lfo_mseg_shape_.addItemList({ "Custom", "Sine", "Triangle", "Square", "Saw up", "Saw down",
                                "Pulse", "Ramp hold", "Step climb", "Step fall",
                                "Flat 0", "Flat 50", "Flat 100" }, 1);
  lfo_mseg_shape_.setSelectedItemIndex(0, dontSendNotification);
  addChildComponent(lfo_mseg_shape_);

  lfo_mseg_apply_shape_.setTitle("Apply LFO shape");
  lfo_mseg_apply_shape_.setDescription("Replace the selected LFO shape with the selected preset");
  lfo_mseg_apply_shape_.setHelpText("Press Enter to apply the selected shape preset");
  lfo_mseg_apply_shape_.setWantsKeyboardFocus(true);
  lfo_mseg_apply_shape_.onClick = [this] { applyLfoShapePreset(); };
  addChildComponent(lfo_mseg_apply_shape_);

  lfo_mseg_point_.setTitle("LFO point");
  lfo_mseg_point_.setDescription("Choose a point in the selected LFO MSEG");
  lfo_mseg_point_.setHelpText("Choose a point, then use the edit buttons to move it or change its value and curve");
  lfo_mseg_point_.setWantsKeyboardFocus(true);
  lfo_mseg_point_.onChange = [this] {
    if (!updating_lfo_mseg_controls_) {
      if (auto* generator = activeLfoGenerator()) {
        const int index = selectedLfoPointIndex();
        if (isPositiveAndBelow(index, generator->getNumPoints())) {
          lfo_cursor_phase_ = generator->getPoint(index).first;
          if (!lfo_multi_selection_mode_) {
            selected_lfo_point_phases_.clear();
            selected_lfo_point_phases_.push_back(lfo_cursor_phase_);
          }
        }
      }
      updateLfoMsegSummary();
    }
  };
  addChildComponent(lfo_mseg_point_);

  auto configure_lfo_button = [this](TextButton& button, const String& title, const String& description) {
    button.setTitle(title);
    button.setDescription(description);
    button.setHelpText("Press Enter to " + description.toLowerCase());
    button.setWantsKeyboardFocus(true);
    addChildComponent(button);
  };
  configure_lfo_button(lfo_mseg_add_point_, "Add MSEG point", "Add a point in the largest gap of the current LFO shape");
  configure_lfo_button(lfo_mseg_delete_point_, "Delete MSEG point", "Delete the selected LFO point");
  configure_lfo_button(lfo_mseg_time_down_, "Point earlier", "Move the selected point earlier in time");
  configure_lfo_button(lfo_mseg_time_up_, "Point later", "Move the selected point later in time");
  configure_lfo_button(lfo_mseg_value_down_, "Value down", "Lower the selected point value");
  configure_lfo_button(lfo_mseg_value_up_, "Value up", "Raise the selected point value");
  lfo_mseg_add_point_.onClick = [this] { addLfoPoint(); };
  lfo_mseg_delete_point_.onClick = [this] { deleteLfoPoint(); };
  lfo_mseg_time_down_.onClick = [this] { moveLfoPointTime(-lfoGridAmount()); };
  lfo_mseg_time_up_.onClick = [this] { moveLfoPointTime(lfoGridAmount()); };
  lfo_mseg_value_down_.onClick = [this] { moveLfoPointValue(-lfoGridAmount()); };
  lfo_mseg_value_up_.onClick = [this] { moveLfoPointValue(lfoGridAmount()); };

  lfo_mseg_curve_.setTitle("Segment curve");
  lfo_mseg_curve_.setDescription("Choose the curve after the selected point");
  lfo_mseg_curve_.setHelpText("This changes how the selected point moves into the next point");
  lfo_mseg_curve_.setWantsKeyboardFocus(true);
  for (int i = 0; i < kNumAccessibleLfoCurves; ++i)
    lfo_mseg_curve_.addItem(curveNameForIndex(i), i + 1);
  lfo_mseg_curve_.setSelectedItemIndex(kCurveLinear, dontSendNotification);
  lfo_mseg_curve_.onChange = [this] {
    if (!updating_lfo_mseg_controls_)
      setLfoPointCurveFromCombo();
  };
  addChildComponent(lfo_mseg_curve_);

  lfo_mseg_smooth_.setTitle("Smooth LFO MSEG");
  lfo_mseg_smooth_.setDescription("Use smooth curves between LFO points");
  lfo_mseg_smooth_.setHelpText("Toggle smoothing for the whole selected LFO shape");
  lfo_mseg_smooth_.setWantsKeyboardFocus(true);
  lfo_mseg_smooth_.onClick = [this] { setLfoSmoothFromToggle(); };
  addChildComponent(lfo_mseg_smooth_);

  lfo_mseg_keyboard_.onKeyPressed = [this](const KeyPress& key) { return handleLfoMsegShortcut(key); };
  lfo_mseg_keyboard_.getStatusText = [this] { return lfoMsegStatusText(); };
  auto handle_lfo_combo_point_shortcut = [this](const KeyPress& key) {
    const juce_wchar character = CharacterFunctions::toLowerCase(key.getTextCharacter());
    if (character == ',' || character == '.')
      return handleLfoMsegShortcut(key);
    return false;
  };
  lfo_mseg_lfo_.onKeyPressed = handle_lfo_combo_point_shortcut;
  lfo_mseg_mode_.onKeyPressed = handle_lfo_combo_point_shortcut;
  lfo_mseg_cycle_.onKeyPressed = handle_lfo_combo_point_shortcut;
  lfo_mseg_grid_.onKeyPressed = handle_lfo_combo_point_shortcut;
  lfo_mseg_shape_.onKeyPressed = handle_lfo_combo_point_shortcut;
  lfo_mseg_point_.onKeyPressed = handle_lfo_combo_point_shortcut;
  lfo_mseg_curve_.onKeyPressed = handle_lfo_combo_point_shortcut;
  addChildComponent(lfo_mseg_keyboard_);

  viewport_.setTitle("Section controls");
  viewport_.setDescription("Controls for the selected synthesizer section");
  viewport_.setViewedComponent(&rows_container_, false);
  addAndMakeVisible(viewport_);

  buildSections();

  // Announce Downloads-folder imports once, only when this instance actually
  // copied new presets (not on every editor/FX-chain open).
  const int imported = synth_.takeDownloadedPresetsImported();
  if (imported > 0)
    postPluginAnnouncement("Imported " + String(imported) + " new presets from Downloads folder",
                           AccessibilityHandler::AnnouncementPriority::high);
  group_selector_.setVisible(!show_all_sections_);
  section_selector_.setVisible(!show_all_sections_);
  String initial_section = last_section_name;
  if (section_names_.indexOf(initial_section) < 0 && section_names_.size() > 0)
    initial_section = section_names_[0];
  if (show_all_sections_) {
    last_section_name = initial_section;
    showAllSections(false);
  }
  else {
    selectSectionByName(initial_section, false);
  }
  rebuildFocusOrder();
  setResizable(true, true);
  setResizeLimits(680, 480, 1400, 1000);
  setSize(900, 700);
  startTimerHz(20);
}

void SynthEditor::paint(Graphics& graphics) {
  graphics.fillAll(Colour(0xff17191d));
}

void SynthEditor::resized() {
  auto bounds = getLocalBounds().reduced(16);
  title_.setBounds(bounds.removeFromTop(40));
  instructions_.setBounds(bounds.removeFromTop(34));
  auto navigation = bounds.removeFromTop(38);
  menu_button_.setBounds(navigation.removeFromLeft(120).reduced(3));
  if (group_selector_.isVisible())
    group_selector_.setBounds(navigation.removeFromLeft(jmin(260, navigation.getWidth() / 2)).reduced(3));
  if (section_selector_.isVisible())
    section_selector_.setBounds(navigation.removeFromLeft(jmin(360, navigation.getWidth())).reduced(3));
  if (modulation_amount_prompt_visible_ || parameter_value_prompt_visible_) {
    bounds.removeFromTop(8);
    modulation_amount_prompt_.setBounds(bounds.removeFromTop(34));
    auto amount_row = bounds.removeFromTop(38);
    modulation_amount_editor_.setBounds(amount_row.removeFromLeft(jmax(180, amount_row.getWidth() - 220)).reduced(3));
    modulation_amount_ok_.setBounds(amount_row.removeFromLeft(100).reduced(3));
    modulation_amount_cancel_.setBounds(amount_row.removeFromLeft(120).reduced(3));
  }
  if (preset_controls_visible_ && !show_all_sections_) {
    bounds.removeFromTop(8);
    preset_summary_.setBounds(bounds.removeFromTop(54));
    auto menu_row = bounds.removeFromTop(38);
    preset_menu_.setBounds(menu_row.removeFromLeft(150).reduced(3));
    preset_library_.setBounds(menu_row.removeFromLeft(jmax(130, menu_row.getWidth() / 3)).reduced(3));
    preset_bank_.setBounds(menu_row.removeFromLeft(menu_row.getWidth() / 2).reduced(3));
    preset_category_.setBounds(menu_row.reduced(3));
    auto search_row = bounds.removeFromTop(38);
    preset_search_.setBounds(search_row.removeFromLeft(search_row.getWidth() / 2).reduced(3));
    preset_selector_.setBounds(search_row.reduced(3));
    auto save_row = bounds.removeFromTop(38);
    preset_preview_.setBounds(save_row.removeFromLeft(220).reduced(3));
    preset_name_editor_.setBounds(save_row.reduced(3));
  }
  if (save_patch_dialog_visible_) {
    bounds.removeFromTop(8);
    save_patch_prompt_.setBounds(bounds.removeFromTop(34));
    auto first_row = bounds.removeFromTop(38);
    save_patch_name_.setBounds(first_row.removeFromLeft(first_row.getWidth() / 2).reduced(3));
    save_patch_author_.setBounds(first_row.reduced(3));
    auto second_row = bounds.removeFromTop(38);
    save_patch_bank_.setBounds(second_row.removeFromLeft(second_row.getWidth() / 2).reduced(3));
    save_patch_category_.setBounds(second_row.reduced(3));
    auto third_row = bounds.removeFromTop(38);
    save_patch_tags_.setBounds(third_row.removeFromLeft(jmax(180, third_row.getWidth() - 220)).reduced(3));
    save_patch_ok_.setBounds(third_row.removeFromLeft(100).reduced(3));
    save_patch_cancel_.setBounds(third_row.removeFromLeft(120).reduced(3));
  }
  if (wavetable_name_.isVisible()) {
    bounds.removeFromTop(8);
    auto wavetable_bounds = bounds.removeFromTop(38);
    wavetable_name_.setBounds(wavetable_bounds.removeFromLeft(jmax(220, wavetable_bounds.getWidth() - 340)));
    load_wavetable_.setBounds(wavetable_bounds.removeFromLeft(165).reduced(3));
    reset_wavetable_.setBounds(wavetable_bounds.removeFromLeft(165).reduced(3));
    auto direct_summary = bounds.removeFromTop(38);
    oscillator_summary_.setBounds(direct_summary.reduced(3));
    auto direct_controls = bounds.removeFromTop(38);
    oscillator_octave_.setBounds(direct_controls.removeFromLeft(direct_controls.getWidth() / 4).reduced(3));
    oscillator_semitone_.setBounds(direct_controls.removeFromLeft(direct_controls.getWidth() / 3).reduced(3));
    oscillator_fine_tune_.setBounds(direct_controls.removeFromLeft(direct_controls.getWidth() / 2).reduced(3));
    oscillator_wave_frame_.setBounds(direct_controls.reduced(3));
    auto scale_controls = bounds.removeFromTop(38);
    oscillator_scale_key_.setBounds(scale_controls.removeFromLeft(scale_controls.getWidth() / 3).reduced(3));
    oscillator_scale_type_.setBounds(scale_controls.removeFromLeft(scale_controls.getWidth() / 2).reduced(3));
    oscillator_scale_mode_.setBounds(scale_controls.reduced(3));
  }
  if (routing_controls_visible_) {
    bounds.removeFromTop(8);
    if (routing_mode_.getParentComponent() == this) {
      routing_summary_.setBounds(bounds.removeFromTop(54));
      routing_mode_.setBounds(bounds.removeFromTop(38).reduced(3));
      auto buttons = bounds.removeFromTop(38);
      routing_default_.setBounds(buttons.removeFromLeft(buttons.getWidth() / 3).reduced(3));
      routing_serial_forward_.setBounds(buttons.removeFromLeft(buttons.getWidth() / 2).reduced(3));
      routing_serial_backward_.setBounds(buttons.reduced(3));
    }
  }
  if (effect_chain_controls_visible_) {
    bounds.removeFromTop(8);
    if (effect_chain_selector_.getParentComponent() == this) {
      effect_chain_summary_.setBounds(bounds.removeFromTop(54));
      auto controls = bounds.removeFromTop(38);
      effect_chain_selector_.setBounds(controls.removeFromLeft(jmax(220, controls.getWidth() - 360)).reduced(3));
      effect_move_up_.setBounds(controls.removeFromLeft(180).reduced(3));
      effect_move_down_.setBounds(controls.removeFromLeft(180).reduced(3));
    }
  }
  if (modulation_controls_visible_) {
    bounds.removeFromTop(8);
    auto selectors = bounds.removeFromTop(38);
    modulation_source_.setBounds(selectors.removeFromLeft(selectors.getWidth() / 3).reduced(3));
    modulation_destination_group_.setBounds(selectors.removeFromLeft(selectors.getWidth() / 2).reduced(3));
    modulation_destination_.setBounds(selectors.reduced(3));
    auto buttons = bounds.removeFromTop(38);
    add_modulation_.setBounds(buttons.removeFromLeft(buttons.getWidth() / 2).reduced(3));
    remove_modulation_.setBounds(buttons.reduced(3));
    modulation_list_.setBounds(bounds.removeFromTop(jmin(180, bounds.getHeight() / 3)).reduced(3));
  }
  if (lfo_mseg_controls_visible_) {
    bounds.removeFromTop(8);
    auto editor_area = bounds.removeFromTop(jmin(330, jmax(230, bounds.getHeight() / 2)));
    auto controls = editor_area.removeFromLeft(jmin(270, editor_area.getWidth() / 3)).reduced(0, 2);
    editor_area.removeFromLeft(14);
    lfo_mseg_keyboard_.setBounds(editor_area.reduced(3));

    const int row_height = 31;
    const int gap = 7;
    auto placeControl = [&](Component& component) {
      component.setBounds(controls.removeFromTop(row_height).reduced(3));
      controls.removeFromTop(gap);
    };
    placeControl(lfo_mseg_lfo_);
    placeControl(lfo_mseg_mode_);
    placeControl(lfo_mseg_cycle_);
    placeControl(lfo_mseg_grid_);
    placeControl(lfo_mseg_shape_);
    placeControl(lfo_mseg_apply_shape_);
    placeControl(lfo_mseg_point_);
    placeControl(lfo_mseg_curve_);
    placeControl(lfo_mseg_smooth_);

    lfo_mseg_summary_.setBounds(bounds.removeFromTop(44));
    auto edit_row = bounds.removeFromTop(38);
    lfo_mseg_add_point_.setBounds(edit_row.removeFromLeft(edit_row.getWidth() / 6).reduced(3));
    lfo_mseg_delete_point_.setBounds(edit_row.removeFromLeft(edit_row.getWidth() / 5).reduced(3));
    lfo_mseg_time_down_.setBounds(edit_row.removeFromLeft(edit_row.getWidth() / 4).reduced(3));
    lfo_mseg_time_up_.setBounds(edit_row.removeFromLeft(edit_row.getWidth() / 3).reduced(3));
    lfo_mseg_value_down_.setBounds(edit_row.removeFromLeft(edit_row.getWidth() / 2).reduced(3));
    lfo_mseg_value_up_.setBounds(edit_row.reduced(3));
  }
  bounds.removeFromTop(10);
  viewport_.setBounds(bounds);
  if (!show_all_sections_)
    rows_container_.setSize(jmax(620, viewport_.getMaximumVisibleWidth()),
                            static_cast<int>(rows_.size()) * 48);
}

void SynthEditor::buildSections() {
  sections_.clear();
  section_names_.clear();
  group_sections_.clear();
  group_names_.clear();
  section_selector_.clear(dontSendNotification);
  group_selector_.clear(dontSendNotification);

  sections_[kPresetSection];
  for (auto* parameter : synth_.getParameters()) {
    if (auto* bridge = dynamic_cast<ValueBridge*>(parameter)) {
      parameters_by_id_[bridge->getParameterId()] = bridge;
      sections_[sectionForParameter(bridge->getParameterId())].push_back(parameter);
    }
  }

  for (auto& section : sections_) {
    std::stable_sort(section.second.begin(), section.second.end(), [](auto* a, auto* b) {
      auto* bridge_a = dynamic_cast<ValueBridge*>(a);
      auto* bridge_b = dynamic_cast<ValueBridge*>(b);
      if (bridge_a == nullptr || bridge_b == nullptr)
        return a->getName(128) < b->getName(128);
      const int rank_a = parameterRank(bridge_a->getParameterId());
      const int rank_b = parameterRank(bridge_b->getParameterId());
      if (rank_a != rank_b)
        return rank_a < rank_b;
      return sectionSortId(bridge_a->getParameterId()) < sectionSortId(bridge_b->getParameterId());
    });
  }

  std::vector<String> names;
  for (const auto& section : sections_)
    names.push_back(section.first);
  auto dynamic_section_rank = [this](const String& section) {
    const String effect_id = effectIdForSection(section);
    if (effect_id.isEmpty())
      return sectionRank(section);

    int effect_index = -1;
    for (int i = 0; i < vital::constants::kNumEffects; ++i) {
      if (effect_id == String(strings::kEffectOrder[i])) {
        effect_index = i;
        break;
      }
    }
    if (effect_index < 0)
      return sectionRank(section);

    int order[vital::constants::kNumEffects];
    readEffectChainOrder(section, order);
    int position = vital::constants::kNumEffects;
    for (int i = 0; i < vital::constants::kNumEffects; ++i) {
      if (order[i] == effect_index) {
        position = i;
        break;
      }
    }

    if (section.startsWith("Bus 1 - "))
      return 220 + sectionRank(kEffectsChainSection) + 1 + position;
    if (section.startsWith("Bus 2 - "))
      return 250 + sectionRank(kEffectsChainSection) + 1 + position;
    if (section.startsWith("Bus 3 - "))
      return 280 + sectionRank(kEffectsChainSection) + 1 + position;
    return sectionRank(kEffectsChainSection) + 1 + position;
  };
  std::stable_sort(names.begin(), names.end(), [&](const String& a, const String& b) {
    const int rank_a = dynamic_section_rank(a);
    const int rank_b = dynamic_section_rank(b);
    if (rank_a != rank_b)
      return rank_a < rank_b;
    return a < b;
  });
  for (const auto& name : names)
    section_names_.add(name);
  buildGroups();
}

void SynthEditor::buildGroups() {
  group_sections_.clear();
  group_names_.clear();

  for (const auto& section : section_names_)
    group_sections_[groupForSection(section)].add(section);

  std::vector<String> names;
  for (const auto& group : group_sections_)
    names.push_back(group.first);
  std::stable_sort(names.begin(), names.end(), [](const String& a, const String& b) {
    const int rank_a = groupRank(a);
    const int rank_b = groupRank(b);
    if (rank_a != rank_b)
      return rank_a < rank_b;
    return a < b;
  });
  for (const auto& name : names)
    group_names_.add(name);

  populateGroupSelector();
}

void SynthEditor::populateGroupSelector(const String& preferredGroup) {
  ScopedValueSetter<bool> guard(updating_navigation_, true);
  group_selector_.clear(dontSendNotification);
  group_selector_.addItemList(group_names_, 1);
  int group_index = group_names_.indexOf(preferredGroup);
  if (group_index < 0 && group_names_.size() > 0)
    group_index = 0;
  if (group_index >= 0)
    group_selector_.setSelectedItemIndex(group_index, dontSendNotification);
}

void SynthEditor::populateElementSelectorForGroup(const String& group, const String& preferredSection) {
  ScopedValueSetter<bool> guard(updating_navigation_, true);
  section_selector_.clear(dontSendNotification);
  const auto found = group_sections_.find(group);
  if (found == group_sections_.end())
    return;

  for (int i = 0; i < found->second.size(); ++i)
    section_selector_.addItem(accessibleSectionTitle(found->second[i]), i + 1);
  int section_index = found->second.indexOf(preferredSection);
  if (section_index < 0 && found->second.size() > 0)
    section_index = 0;
  if (section_index >= 0)
    section_selector_.setSelectedItemIndex(section_index, dontSendNotification);
}

void SynthEditor::selectSectionByName(const String& section, bool announce) {
  const int section_index = section_names_.indexOf(section);
  if (section_index < 0)
    return;

  if (show_all_sections_) {
    last_section_name = section;
    if (row_focus_order_.empty())
      showAllSections(false);
    Component* target = nullptr;
    for (auto* component : row_focus_order_) {
      if (component != nullptr && component->getProperties()["SectionName"].toString() == section) {
        target = component;
        break;
      }
    }
    if (target == nullptr) {
      const String title = accessibleSectionTitle(section);
      for (auto* component : row_focus_order_) {
        if (component != nullptr && component->getTitle() == title) {
          target = component;
          break;
        }
      }
    }
    if (target == nullptr) {
      const String group = groupForSection(section);
      for (auto* component : row_focus_order_) {
        if (component != nullptr && component->getTitle() == group) {
          target = component;
          break;
        }
      }
    }
    if (target != nullptr) {
      ensureComponentVisible(target);
      target->grabKeyboardFocus();
    }
    if (announce)
      postPluginAnnouncement(accessibleSectionTitle(section),
                                             AccessibilityHandler::AnnouncementPriority::high);
    return;
  }

  const String group = groupForSection(section);
  {
    ScopedValueSetter<bool> guard(updating_navigation_, true);
    const int group_index = group_names_.indexOf(group);
    if (group_index >= 0 && group_selector_.getSelectedItemIndex() != group_index)
      group_selector_.setSelectedItemIndex(group_index, dontSendNotification);

    section_selector_.clear(dontSendNotification);
    const auto found = group_sections_.find(group);
    if (found != group_sections_.end()) {
      for (int i = 0; i < found->second.size(); ++i)
        section_selector_.addItem(accessibleSectionTitle(found->second[i]), i + 1);
      const int element_index = found->second.indexOf(section);
      if (element_index >= 0)
        section_selector_.setSelectedItemIndex(element_index, dontSendNotification);
    }
  }

  showSection(section_index, announce);
}

std::unique_ptr<AccessibleParameterRow> SynthEditor::createAccessibleParameterRow(AudioProcessorParameter& parameter,
                                                                                  const String& sectionName) const {
  auto* bridge = dynamic_cast<ValueBridge*>(&parameter);
  if (bridge == nullptr)
    return std::make_unique<AccessibleParameterRow>(parameter);

  const String parameter_id = bridge->getParameterId();
  const String suffix = filterSuffixForParameter(sectionName, parameter_id);
  if (parameter_id == "stereo_mode") {
    return std::make_unique<AccessibleParameterRow>(
        parameter, "Stereo mode",
        [](double value) { return value >= 0.5 ? "Rotate" : "Spread"; }, 1.0, 0.0,
        String(), String(),
        "Choose how the final stereo routing control treats the left and right channels", true);
  }

  if (isTransposeQuantizeParameter(parameter_id)) {
    return std::make_unique<AccessibleParameterRow>(parameter, "Transpose quantize",
                                                    [bridge](double value) {
                                                      const int quantize = static_cast<int>(std::round(
                                                          bridge->convertToEngineValue(static_cast<float>(value))));
                                                      return transposeQuantizeDescription(quantize);
                                                    });
  }

  if (isTransposeQuantizeModeParameter(parameter_id)) {
    return std::make_unique<AccessibleParameterRow>(
        parameter, transposeQuantizeModeTitleForParameter(parameter_id),
        [](double value) { return value >= 0.5 ? "Global scale" : "Local scale"; }, 1.0, 0.0,
        String(), String(),
        "Choose whether transpose quantize uses this source's scale or the global scale", true);
  }

  if (isEffectChoiceParameter(parameter_id, "eq_low_mode")) {
    return std::make_unique<AccessibleParameterRow>(
        parameter, "EQ low band mode",
        [](double value) { return value >= 0.5 ? "High pass" : "Shelf"; }, 1.0, 0.0,
        String(), String(),
        "Choose whether the low EQ band is a shelf or high pass filter", true);
  }

  if (isEffectChoiceParameter(parameter_id, "eq_band_mode")) {
    return std::make_unique<AccessibleParameterRow>(
        parameter, "EQ middle band mode",
        [](double value) { return value >= 0.5 ? "Notch" : "Shelf"; }, 1.0, 0.0,
        String(), String(),
        "Choose whether the middle EQ band is a shelf or notch filter", true);
  }

  if (isEffectChoiceParameter(parameter_id, "eq_high_mode")) {
    return std::make_unique<AccessibleParameterRow>(
        parameter, "EQ high band mode",
        [](double value) { return value >= 0.5 ? "Low pass" : "Shelf"; }, 1.0, 0.0,
        String(), String(),
        "Choose whether the high EQ band is a shelf or low pass filter", true);
  }

  if (isEffectChoiceParameter(parameter_id, "utility_filter_slope")) {
    return std::make_unique<AccessibleParameterRow>(
        parameter, "Utility filter slope",
        [](double value) { return value >= 0.5 ? "24 dB per octave" : "12 dB per octave"; }, 1.0, 0.0,
        String(), String(),
        "Choose the slope for the utility low cut and high cut filters", true);
  }

  if (parameter_id == "granular_mode") {
    return std::make_unique<AccessibleParameterRow>(
        parameter, "Granular playback mode",
        [](double value) { return value >= 0.5 ? "Manual position" : "Play-through"; }, 1.0, 0.0,
        String(), String(),
        "Choose whether the granular oscillator plays through the sample or uses the position control", true);
  }

  if (parameter_id == "granular_midi_density") {
    return std::make_unique<AccessibleParameterRow>(
        parameter, "Granular MIDI density control",
        [](double value) { return value >= 0.5 ? "MIDI note rate" : "Density knob"; }, 1.0, 0.0,
        String(), String(),
        "Choose whether grain density follows the density knob or the incoming MIDI note rate", true);
  }

  if (parameter_id == "sample_root_key" || parameter_id == "granular_root_key") {
    const String title = parameter_id == "sample_root_key" ? "Sample root key" : "Granular root key";
    return std::make_unique<AccessibleParameterRow>(
        parameter, title,
        [](double value) { return midiRootKeyText(value); }, 1.0, 0.0,
        String(), String(),
        "Set the MIDI note that plays the loaded sample at its original pitch", true);
  }

  if (isSampleLoopPointParameter(parameter_id)) {
    return std::make_unique<AccessibleParameterRow>(
        parameter, bridge->getName(128),
        [this, bridge](double value) -> String {
          const int length = sampleLoopPointLength();
          if (fine_tune_sample_loop_ && length > 0)
            return String(juce::roundToInt(value * length));
          return accessibleParameterText(*bridge, value);
        });
  }

  if (parameter_id.startsWith("random_")) {
    const String suffix = parameter_id.fromFirstOccurrenceOf("random_", false, false)
                                      .fromFirstOccurrenceOf("_", false, false);
    if (suffix == "style")
      return std::make_unique<AccessibleParameterRow>(parameter, "Style");
    if (suffix == "frequency")
      return std::make_unique<AccessibleParameterRow>(parameter, "Rate");
    if (suffix == "rate_x10")
      return std::make_unique<AccessibleParameterRow>(
          parameter, "10x rate",
          [bridge](double value) { return bridge->getText(static_cast<float>(value), 128); }, 1.0, 0.0,
          String(), String(),
          "Multiply this random LFO rate by ten", true);
    if (suffix == "sync")
      return std::make_unique<AccessibleParameterRow>(
          parameter, "Tempo sync type",
          [bridge](double value) { return bridge->getText(static_cast<float>(value), 128); }, 1.0, 0.0,
          String(), String(),
          "Choose seconds, tempo, dotted tempo, triplet tempo, or keytrack timing", true);
    if (suffix == "tempo")
      return std::make_unique<AccessibleParameterRow>(parameter, "Tempo");
    if (suffix == "sync_type")
      return std::make_unique<AccessibleParameterRow>(
          parameter, "Transport sync",
          [](double value) { return value >= 0.5 ? "Host synced" : "Free running"; }, 1.0, 0.0,
          String(), String(),
          "Choose whether the random LFO follows host transport sync or runs freely", true);
    if (suffix == "stereo")
      return std::make_unique<AccessibleParameterRow>(
          parameter, "Stereo mode",
          [](double value) { return value >= 0.5 ? "Independent stereo" : "Mono"; }, 1.0, 0.0,
          String(), String(),
          "Choose whether random values are shared or independent between stereo channels", true);
    if (suffix == "keytrack_transpose")
      return std::make_unique<AccessibleParameterRow>(parameter, "Keytrack transpose");
    if (suffix == "keytrack_tune")
      return std::make_unique<AccessibleParameterRow>(parameter, "Keytrack tune");
  }

  if (parameter_id == "filter_1_filter_input") {
    return std::make_unique<AccessibleParameterRow>(
        parameter, "Filter 1 input from Filter 2",
        [](double value) {
          return value >= 0.5 ? "Filter 2 feeds Filter 1" : "Filter 1 does not receive Filter 2";
        }, 1.0, 0.0,
        String(), String(),
        "Route Filter 2 into Filter 1 for serial filter routing", true);
  }

  if (parameter_id == "filter_2_filter_input") {
    return std::make_unique<AccessibleParameterRow>(
        parameter, "Filter 2 input from Filter 1",
        [](double value) {
          return value >= 0.5 ? "Filter 1 feeds Filter 2" : "Filter 2 does not receive Filter 1";
        }, 1.0, 0.0,
        String(), String(),
        "Route Filter 1 into Filter 2 for serial filter routing", true);
  }

  const int macro_index = macroIndexForControlId(parameter_id);
  if (macro_index >= 0) {
    const String macro_name = synth_.getMacroName(macro_index);
    return std::make_unique<AccessibleParameterRow>(parameter, macro_name,
                                                    [this, macro_index, bridge](double value) {
                                                      if (isMacroBipolar(macro_index)) {
                                                        const int percent = roundToInt((value * 2.0 - 1.0) * 100.0);
                                                        return String(percent) + "%";
                                                      }
                                                      return bridge->getText(static_cast<float>(value), 128);
                                                    });
  }

  if ((parameter_id.startsWith("osc_") && parameter_id.endsWith("_destination")) ||
      parameter_id == "sample_destination" || parameter_id == "granular_destination") {
    const double max_normalized = bridge->convertToPluginValue(static_cast<float>(vital::constants::kBus3));
    return std::make_unique<AccessibleParameterRow>(parameter, "Destination",
                                                    [bridge](double value) {
                                                      const int destination = static_cast<int>(std::round(
                                                          bridge->convertToEngineValue(static_cast<float>(value))));
                                                      return routingDestinationText(destination);
                                                    },
                                                    max_normalized);
  }

  if (parameter_id.startsWith("bus_") && parameter_id.endsWith("_destination")) {
    const String number = parameter_id.fromFirstOccurrenceOf("bus_", false, false)
                          .upToFirstOccurrenceOf("_", false, false);
    const int bus_index = jlimit(0, vital::kNumBuses - 1, number.getIntValue() - 1);
    return std::make_unique<AccessibleParameterRow>(parameter, "Destination",
                                                    [bridge, bus_index](double value) {
                                                      const int destination = static_cast<int>(std::round(
                                                          bridge->convertToEngineValue(static_cast<float>(value))));
                                                      return busRoutingDestinationText(bus_index, destination);
                                                    });
  }

  if (parameter_id == "filter_1_destination" || parameter_id == "filter_2_destination") {
    const double min_normalized = bridge->convertToPluginValue(static_cast<float>(vital::constants::kEffects));
    const double max_normalized = bridge->convertToPluginValue(static_cast<float>(vital::constants::kBus3));
    const String filter_name = parameter_id == "filter_1_destination" ? "Filter 1 destination"
                                                                      : "Filter 2 destination";
    return std::make_unique<AccessibleParameterRow>(parameter, filter_name,
                                                    [bridge](double value) {
                                                      const int destination = static_cast<int>(std::round(
                                                          bridge->convertToEngineValue(static_cast<float>(value))));
                                                      return routingDestinationText(destination);
                                                    },
                                                    max_normalized, min_normalized);
  }

  if (suffix.isEmpty())
    return std::make_unique<AccessibleParameterRow>(parameter,
                                                    accessibleParameterTitle(sectionName, parameter.getName(128)));

  const String prefix = filterPrefixForSection(sectionName);
  int model = 0;
  if (auto* model_bridge = parameterBridge(prefix + "model"))
    model = static_cast<int>(std::round(model_bridge->convertToEngineValue(model_bridge->getValue())));

  if (suffix == "model")
    return std::make_unique<AccessibleParameterRow>(parameter, "Filter model");

  if (suffix == "style") {
    const int max_style = jmax(0, numFilterStylesForModel(model) - 1);
    const double max_normalized = bridge->convertToPluginValue(static_cast<float>(max_style));
    return std::make_unique<AccessibleParameterRow>(parameter, "Filter style",
                                                    [bridge, model](double value) {
                                                      const int style = static_cast<int>(std::round(
                                                          bridge->convertToEngineValue(static_cast<float>(value))));
                                                      return filterStyleNameForModel(model, style);
                                                    },
                                                    max_normalized);
  }

  if (suffix == "blend") {
    const String name = model == vital::constants::kComb ? "Comb blend"
                                                        : "Filter blend, low pass to high pass";
    return std::make_unique<AccessibleParameterRow>(parameter, name,
                                                    [model](double value) { return filterBlendText(model, value); });
  }

  return std::make_unique<AccessibleParameterRow>(parameter,
                                                  accessibleParameterTitle(sectionName, parameter.getName(128)));
}

int SynthEditor::sampleLoopPointLength() const {
  auto* sample = synth_.getSample();
  return sample != nullptr ? sample->originalLength() : 0;
}

void SynthEditor::toggleSampleLoopFineTune() {
  fine_tune_sample_loop_ = !fine_tune_sample_loop_;
  for (auto& row : rows_) {
    if (row != nullptr && isSampleLoopPointParameter(row->parameterId()))
      row->notifyDisplayChanged();
  }
  postPluginAnnouncement(String("Sample loop fine tune ") +
                             (fine_tune_sample_loop_ ? "on, sample numbers" : "off, percent"),
                         AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::wireSampleLoopFineTune(AccessibleParameterRow& row) {
  if (!isSampleLoopPointParameter(row.parameterId()))
    return;
  row.setFineTuneCallbacks([this] { return fine_tune_sample_loop_; },
                           [this] { toggleSampleLoopFineTune(); });
}

bool SynthEditor::shouldShowParameterInSection(const String& sectionName, AudioProcessorParameter* parameter) const {
  auto* bridge = dynamic_cast<ValueBridge*>(parameter);
  if (bridge == nullptr)
    return true;

  const String parameter_id = bridge->getParameterId();
  if (parameter_id == "beats_per_minute" || parameter_id == "view_spectrogram" ||
      parameter_id.endsWith("_view_2d"))
    return false;

  // The zone crossfade only applies while glide zone crossing is enabled.
  if (parameter_id == "zone_crossfade") {
    if (auto* glide_bridge = parameterBridge("portamento_glide_zones"))
      return glide_bridge->convertToEngineValue(glide_bridge->getValue()) != 0.0f;
    return false;
  }

  if (isMacroBipolarParameterId(bridge->getParameterId()))
    return false;

  const String suffix = filterSuffixForParameter(sectionName, parameter_id);
  if (suffix.isEmpty())
    return true;
  if (!isFilterParameterSuffix(suffix))
    return false;

  const String prefix = filterPrefixForSection(sectionName);
  int model = 0;
  int style = 0;
  if (auto* model_bridge = parameterBridge(prefix + "model"))
    model = static_cast<int>(std::round(model_bridge->convertToEngineValue(model_bridge->getValue())));
  if (auto* style_bridge = parameterBridge(prefix + "style"))
    style = static_cast<int>(std::round(style_bridge->convertToEngineValue(style_bridge->getValue())));
  if (model == vital::constants::kFormant)
    style = jlimit(0, vital::FormantFilter::kNumFormantStyles - 1, style);

  const bool formant = model == vital::constants::kFormant;
  const bool vocal_tract = formant && style == vital::FormantFilter::kVocalTract;
  const bool comb = model == vital::constants::kComb;

  if (suffix == "cutoff" || suffix == "resonance" || suffix == "keytrack")
    return !formant;
  if (suffix == "drive")
    return !formant && !comb;
  if (suffix == "blend")
    return !formant || vocal_tract;
  if (suffix == "blend_transpose")
    return comb;
  if (suffix == "formant_x" || suffix == "formant_y" ||
      suffix == "formant_resonance" || suffix == "formant_spread")
    return formant;
  if (suffix == "formant_transpose")
    return formant && !vocal_tract;

  return true;
}

String SynthEditor::focusedParameterId() const {
  for (const auto& row : rows_) {
    if (row != nullptr && row->focusableControl()->hasKeyboardFocus(true))
      return row->parameterId();
  }
  return {};
}

String SynthEditor::focusedAccessibleTitle() const {
  auto* focused = Component::getCurrentlyFocusedComponent();
  if (focused == nullptr)
    return {};

  const String title = focused->getTitle();
  if (title.isNotEmpty())
    return title;
  return focused->getName();
}

Component* SynthEditor::persistentFocusedComponent() const {
  auto* focused = Component::getCurrentlyFocusedComponent();
  if (focused == nullptr)
    return nullptr;

  for (Component* component = focused; component != nullptr; component = component->getParentComponent()) {
    if (component == &rows_container_)
      return nullptr;
  }

  for (const auto& row : rows_) {
    if (row != nullptr && row->focusableControl() == focused)
      return nullptr;
  }
  return focused;
}

void SynthEditor::restoreFocusAfterRebuild(const String& parameterId, Component* persistentComponent,
                                           const String& accessibleTitle, const String& preferredSection) {
  Component* target = nullptr;
  if (parameterId.isNotEmpty()) {
    for (const auto& row : rows_) {
      if (row != nullptr && row->parameterId() == parameterId) {
        target = row->focusableControl();
        break;
      }
    }
  }

  if (target == nullptr && persistentComponent != nullptr && persistentComponent->isShowing())
    target = persistentComponent;

  if (target == nullptr && accessibleTitle.isNotEmpty() && preferredSection.isNotEmpty()) {
    for (auto* component : focus_order_) {
      if (component == nullptr || !component->isShowing())
        continue;
      if (component->getProperties()["SectionName"].toString() != preferredSection)
        continue;
      if (component->getTitle() == accessibleTitle || component->getName() == accessibleTitle) {
        target = component;
        break;
      }
    }
  }

  if (target == nullptr && accessibleTitle.isNotEmpty()) {
    for (auto* component : focus_order_) {
      if (component == nullptr || !component->isShowing())
        continue;
      if (component->getTitle() == accessibleTitle || component->getName() == accessibleTitle) {
        target = component;
        break;
      }
    }
  }

  if (target == nullptr)
    return;

  Component::SafePointer<SynthEditor> safe_this(this);
  Component::SafePointer<Component> safe_target(target);
  MessageManager::callAsync([safe_this, safe_target] {
    if (safe_this != nullptr && safe_target != nullptr && safe_target->isShowing()) {
      safe_this->ensureComponentVisible(safe_target.getComponent());
      safe_target->grabKeyboardFocus();
    }
  });
}

bool SynthEditor::refreshFilterRowsIfNeeded() {
  bool changed = false;
  for (int filter = 0; filter < 3; ++filter) {
    const String prefix = filter == 2 ? String("filter_fx_") : "filter_" + String(filter + 1) + "_";
    int model = -1;
    int style = -1;
    if (auto* model_bridge = parameterBridge(prefix + "model"))
      model = static_cast<int>(std::round(model_bridge->convertToEngineValue(model_bridge->getValue())));
    if (auto* style_bridge = parameterBridge(prefix + "style"))
      style = static_cast<int>(std::round(style_bridge->convertToEngineValue(style_bridge->getValue())));

    if (last_filter_models_[filter] != model || last_filter_styles_[filter] != style) {
      last_filter_models_[filter] = model;
      last_filter_styles_[filter] = style;
      changed = true;
    }
  }

  if (!changed)
    return false;

  const String parameter_id = focusedParameterId();
  const String accessible_title = focusedAccessibleTitle();
  auto* persistent_focus = persistentFocusedComponent();

  if (show_all_sections_)
    showAllSections(false);
  else if (last_section_name == "Filter 1" || last_section_name == "Filter 2" || last_section_name == "Filter")
    selectSectionByName(last_section_name, false);

  restoreFocusAfterRebuild(parameter_id, persistent_focus, accessible_title);

  return true;
}

bool SynthEditor::refreshZoneCrossfadeRowIfNeeded() {
  int glide_zones = 0;
  if (auto* bridge = parameterBridge("portamento_glide_zones"))
    glide_zones = static_cast<int>(std::round(bridge->convertToEngineValue(bridge->getValue())));

  if (last_glide_zones_ == glide_zones)
    return false;
  last_glide_zones_ = glide_zones;

  // Only the Voice and performance section shows the zone crossfade row, so a
  // change to glide zone crossing only needs to rebuild that view.
  if (!show_all_sections_ && last_section_name != "Voice and performance")
    return false;

  const String parameter_id = focusedParameterId();
  const String accessible_title = focusedAccessibleTitle();
  auto* persistent_focus = persistentFocusedComponent();

  if (show_all_sections_)
    showAllSections(false);
  else
    selectSectionByName(last_section_name, false);

  restoreFocusAfterRebuild(parameter_id, persistent_focus, accessible_title);

  return true;
}

void SynthEditor::showSection(int index, bool announce) {
  if (!isPositiveAndBelow(index, section_names_.size()))
    return;

  section_headers_.clear();
  row_focus_order_.clear();
  top_level_accessibility_order_.clear();
  rows_.clear();
  rows_container_.removeAllChildren();
  const auto section_name = section_names_[index];
  last_section_name = section_name;
  setPresetControlsVisible(section_name == kPresetSection);
  configureWavetableActions(section_name);
  setRoutingControlsVisible(section_name == kSignalRoutingSection);
  setEffectChainControlsVisible(isEffectChainSection(section_name));
  setModulationControlsVisible(section_name == "Modulation routing");
  if (section_name.startsWith("LFO ")) {
    active_lfo_index_ = jlimit(0, vital::kNumLfos - 1, section_name.fromFirstOccurrenceOf("LFO ", false, false).getIntValue() - 1);
    lfo_cursor_phase_ = 0.0f;
  }
  setLfoMsegControlsVisible(section_name.startsWith("LFO "));
  int y = 0;
  int focus_order = 10;

  auto addActionButton = [&](const String& title, const String& description, std::function<void()> action) {
    auto button = std::make_unique<OffscreenTextButton>(title);
    auto* button_ptr = button.get();
    button_ptr->setTitle(title);
    button_ptr->setDescription(description);
    button_ptr->setHelpText("Press Enter to " + description.toLowerCase());
    button_ptr->setWantsKeyboardFocus(true);
    button_ptr->onClick = std::move(action);
    button_ptr->setExplicitFocusOrder(focus_order++);
    button_ptr->setBounds(0, y, jmax(620, viewport_.getMaximumVisibleWidth()), 38);
    rows_container_.addAndMakeVisible(button_ptr);
    row_focus_order_.push_back(button_ptr);
    section_headers_.push_back(std::move(button));
    y += 44;
  };

  auto addWavetableEditorControls = [&](int oscillator) {
    const int frames = wavetableFrameCount(oscillator);

    auto summary = std::make_unique<Label>();
    auto* summary_ptr = summary.get();
    summary_ptr->setTitle("Wavetable editor");
    summary_ptr->setDescription(wavetableEditorSummary(oscillator));
    summary_ptr->setText(wavetableEditorSummary(oscillator), dontSendNotification);
    summary_ptr->setColour(Label::textColourId, Colours::white);
    summary_ptr->setBounds(0, y, jmax(620, viewport_.getMaximumVisibleWidth()), 38);
    rows_container_.addAndMakeVisible(summary_ptr);
    section_headers_.push_back(std::move(summary));
    y += 44;

    auto scope = std::make_unique<OffscreenComboBox>();
    auto* scope_ptr = scope.get();
    scope_ptr->setTitle("Edit scope");
    scope_ptr->setDescription("Choose whether wavetable edits affect the selected frame or all frames");
    scope_ptr->setHelpText("Choose current frame for precise edits, or all frames for global harmonic changes");
    scope_ptr->addItem("Current frame", 1);
    scope_ptr->addItem("All frames", 2);
    scope_ptr->setSelectedId(1, dontSendNotification);
    scope_ptr->setWantsKeyboardFocus(true);
    scope_ptr->setExplicitFocusOrder(focus_order++);
    scope_ptr->setBounds(0, y, jmax(620, viewport_.getMaximumVisibleWidth()), 38);
    rows_container_.addAndMakeVisible(scope_ptr);
    row_focus_order_.push_back(scope_ptr);
    section_headers_.push_back(std::move(scope));
    y += 44;

    auto addEditorSlider = [&](const String& title, const String& description, double minimum, double maximum,
                               double value, double interval, const String& suffix) {
      auto slider = std::make_unique<OffscreenSlider>();
      auto* slider_ptr = slider.get();
      slider_ptr->setTitle(title);
      slider_ptr->setName(title);
      slider_ptr->setDescription(description);
      slider_ptr->setHelpText("Use arrow keys for small changes, Page Up and Page Down for larger changes");
      slider_ptr->setRange(minimum, maximum, interval);
      slider_ptr->setValue(value, dontSendNotification);
      slider_ptr->setWantsKeyboardFocus(true);
      slider_ptr->setExplicitFocusOrder(focus_order++);
      slider_ptr->textFromValueFunction = [suffix](double slider_value) {
        return String(slider_value, 0) + suffix;
      };
      slider_ptr->onTextEntryCommand = [this, safe_slider = Component::SafePointer<OffscreenSlider>(slider_ptr),
                                        title](Component& target) {
        if (safe_slider == nullptr)
          return;
        promptForCustomValue(title, safe_slider->getTextFromValue(safe_slider->getValue()), target,
                             [safe_slider](const String& text) {
                               if (safe_slider == nullptr)
                                 return;
                               Slider::ScopedDragNotification drag(*safe_slider);
                               safe_slider->setValue(safe_slider->getValueFromText(text), sendNotificationSync);
                             });
      };
      slider_ptr->setBounds(0, y, jmax(620, viewport_.getMaximumVisibleWidth()), 42);
      rows_container_.addAndMakeVisible(slider_ptr);
      row_focus_order_.push_back(slider_ptr);
      section_headers_.push_back(std::move(slider));
      y += 48;
      return slider_ptr;
    };

    auto* frame_slider = addEditorSlider("Frame", "Selected wavetable frame", 1.0, frames, 1.0, 1.0, "");
    auto* harmonic_slider = addEditorSlider("Harmonic", "Selected harmonic number", 1.0,
                                            vital::WaveFrame::kNumRealComplex - 2, 1.0, 1.0, "");
    auto* level_slider = addEditorSlider("Harmonic level", "Harmonic level as percent", 0.0, 400.0, 100.0, 1.0, "%");
    auto* phase_slider = addEditorSlider("Harmonic phase", "Harmonic phase in degrees", -180.0, 180.0, 0.0, 1.0, " degrees");

    auto selected_frame = [frame_slider] {
      return jmax(0, roundToInt(frame_slider->getValue()) - 1);
    };
    auto all_frames = [scope_ptr] {
      return scope_ptr->getSelectedId() == 2;
    };
    auto selected_harmonic = [harmonic_slider] {
      return roundToInt(harmonic_slider->getValue());
    };

    addActionButton("Set harmonic", "Set or add the selected harmonic at the chosen level and phase",
                    [this, oscillator, selected_frame, all_frames, selected_harmonic, level_slider, phase_slider] {
                      applyWavetableHarmonicEdit(oscillator, selected_frame(), all_frames(), selected_harmonic(),
                                                 static_cast<float>(level_slider->getValue()),
                                                 static_cast<float>(phase_slider->getValue()));
                    });
    addActionButton("Clear harmonic", "Clear the selected harmonic",
                    [this, oscillator, selected_frame, all_frames, selected_harmonic] {
                      clearWavetableHarmonic(oscillator, selected_frame(), all_frames(), selected_harmonic());
                    });
    addActionButton("Remove fundamental", "Remove harmonic 1",
                    [this, oscillator, selected_frame, all_frames] {
                      removeWavetableFundamental(oscillator, selected_frame(), all_frames());
                    });
    addActionButton("Normalize wavetable", "Normalize the current frame or all frames",
                    [this, oscillator, selected_frame, all_frames] {
                      normalizeWavetableFrame(oscillator, selected_frame(), all_frames());
                    });
  };

  if (section_name.startsWith("Oscillator ")) {
    const int oscillator = section_name.fromFirstOccurrenceOf("Oscillator ", false, false).getIntValue() - 1;
    if (isPositiveAndBelow(oscillator, vital::kNumOscillators)) {
      addActionButton("Load wavetable", "Load a wavetable into " + section_name, [this, oscillator] {
        active_oscillator_ = oscillator;
        chooseWavetableFile();
      });
      addActionButton("Import audio as wavetable splice",
                      "Import an audio file into " + section_name + " using wavetable splice",
                      [this, oscillator] {
                        active_oscillator_ = oscillator;
                        chooseWavetableFile(WavetableCreator::kWavetableSplice);
                      });
      addActionButton("Import audio as vocode",
                      "Import an audio file into " + section_name + " using vocode",
                      [this, oscillator] {
                        active_oscillator_ = oscillator;
                        chooseWavetableFile(WavetableCreator::kVocoded);
                      });
      addActionButton("Import audio as pitched wavetable",
                      "Import an audio file into " + section_name + " using pitched mode",
                      [this, oscillator] {
                        active_oscillator_ = oscillator;
                        chooseWavetableFile(WavetableCreator::kPitched);
                      });
      addActionButton("Browse wavetable folders", "Browse wavetable folders for " + section_name,
                      [this, oscillator] {
                        active_oscillator_ = oscillator;
                        showWavetableBrowserMenu(oscillator, rows_container_);
                      });
      addActionButton("Previous wavetable", "Load the previous wavetable into " + section_name,
                      [this, oscillator] { loadShiftedWavetable(oscillator, -1); });
      addActionButton("Next wavetable", "Load the next wavetable into " + section_name,
                      [this, oscillator] { loadShiftedWavetable(oscillator, 1); });
      addActionButton("Export wavetable", "Save the current wavetable from " + section_name,
                      [this, oscillator] { saveWavetableFile(oscillator); });
      if (wavetable_editor_visible_[oscillator]) {
        addActionButton("Hide wavetable editor", "Hide the wavetable editor controls",
                        [this, oscillator] { setWavetableEditorVisible(oscillator, false); });
        addWavetableEditorControls(oscillator);
      }
      else {
        addActionButton("Show wavetable editor", "Show the wavetable editor controls",
                        [this, oscillator] { setWavetableEditorVisible(oscillator, true); });
      }
    }
  }
  else if (section_name == "Sample") {
    addActionButton("Load sample", "Load an audio sample", [this] { chooseSampleFile(); });
    addActionButton("Browse sample folders", "Browse sample folders",
                    [this] { showSampleBrowserMenu(rows_container_); });
    addActionButton("Previous sample", "Load the previous sample", [this] { loadShiftedSample(-1); });
    addActionButton("Next sample", "Load the next sample", [this] { loadShiftedSample(1); });
  }
  else if (section_name == "Granular") {
    addActionButton("Load sample", "Load an audio sample for Granular",
                    [this] { chooseSampleFile(true); });
    addActionButton("Browse sample folders", "Browse sample folders for Granular",
                    [this] { showSampleBrowserMenu(rows_container_, true); });
    addActionButton("Previous sample", "Load the previous sample into Granular",
                    [this] { loadShiftedSample(-1, true); });
    addActionButton("Next sample", "Load the next sample into Granular",
                    [this] { loadShiftedSample(1, true); });
  }
  else if (section_name.startsWith("LFO ")) {
    const int lfo = section_name.fromFirstOccurrenceOf("LFO ", false, false).getIntValue() - 1;
    if (isPositiveAndBelow(lfo, vital::kNumLfos)) {
      addActionButton("Import LFO preset", "Load an LFO shape into " + section_name,
                      [this, lfo] { chooseLfoPresetFile(lfo); });
      addActionButton("Save LFO preset", "Save the current shape from " + section_name,
                      [this, lfo] { saveLfoPresetFile(lfo); });
    }
  }
  else if (effectIdForSection(section_name).isNotEmpty()) {
    addActionButton("Import FX preset", "Load an FX preset into " + accessibleSectionTitle(section_name),
                    [this, section_name] { chooseEffectPresetFile(section_name); });
    addActionButton("Save FX preset", "Save the current " + accessibleSectionTitle(section_name) + " settings",
                    [this, section_name] { saveEffectPresetFile(section_name); });
  }

  if (routing_controls_visible_) {
    refreshRoutingControls();
    routing_summary_.setBounds(0, y, jmax(620, viewport_.getMaximumVisibleWidth()), 54);
    rows_container_.addAndMakeVisible(routing_summary_);
    y += 60;

    auto addRoutingControl = [&](Component& component) {
      component.setBounds(0, y, jmax(620, viewport_.getMaximumVisibleWidth()), 38);
      rows_container_.addAndMakeVisible(component);
      y += 44;
    };

    addRoutingControl(routing_mode_);
    addRoutingControl(routing_default_);
    addRoutingControl(routing_serial_forward_);
    addRoutingControl(routing_serial_backward_);
  }

  for (auto* parameter : sections_[section_name]) {
    if (auto* bridge = dynamic_cast<ValueBridge*>(parameter))
      if (bridge->getParameterId().endsWith("effect_chain_order") ||
          bridge->getParameterId().endsWith("post_effect_order") ||
          bridge->getParameterId().contains("_effect_chain_slot_") ||
          bridge->getParameterId().startsWith("effect_chain_slot_"))
        continue;
    if (!shouldShowParameterInSection(section_name, parameter))
      continue;

    auto row = createAccessibleParameterRow(*parameter, section_name);
    row->setModulationSourceSubmenuCallback([this](const String& destinationId,
                                                   std::map<int, String>& choices, int firstItemId) {
      return createModulationSourceSubmenu(destinationId, choices, firstItemId);
    });
    row->setModulationAssignCallback([this](const String& sourceId, const String& destinationId, Component& target) {
      connectModulationAndPromptForAmount(sourceId, destinationId, target);
    });
    row->setModulationEditCallback([this](const String& sourceId, const String& destinationId, Component& target) {
      promptForInitialModulationAmount(sourceId, destinationId, false, target);
    });
    row->setModulationDestinationPredicate([this](const String& id) { return isModulationDestinationId(id); });
    row->setModulationRemovalCallbacks(
        [this](const String& destinationId) { return modulationSourcesForDestination(destinationId); },
        [this](const String& sourceId, const String& destinationId, Component& target) {
          removeModulationFromParameter(sourceId, destinationId, target);
        });
    row->setMidiLearnCallback([this](const String& parameterId, Component&, bool clear) {
      if (clear) {
        synth_.clearMidiLearn(parameterId.toStdString());
        postPluginAnnouncement("Cleared MIDI learn for " + modulationDestinationLabelForId(parameterId),
                                               AccessibilityHandler::AnnouncementPriority::high);
      }
      else {
        synth_.armMidiLearn(parameterId.toStdString());
        postPluginAnnouncement("MIDI learn armed for " + modulationDestinationLabelForId(parameterId),
                                               AccessibilityHandler::AnnouncementPriority::high);
      }
    });
    row->setValueEntryCallback([this](const String& parameterId, Component& target) {
      promptForParameterValue(parameterId, target);
    });
    row->setExtraCommandCallback([this](const String& parameterId, const KeyPress& key, Component& target) {
      return handleMacroShortcut(parameterId, key, target) ||
             handleEffectShortcut(last_section_name, key, target);
    });
    wireSampleLoopFineTune(*row);
    row->setBounds(0, y, jmax(620, viewport_.getMaximumVisibleWidth()), 48);
    row->setExplicitFocusOrder(focus_order++);
    row->setControlFocusOrder(focus_order++);
    rows_container_.addAndMakeVisible(row.get());
    rows_.push_back(std::move(row));
    y += 48;
  }

  if (effect_chain_controls_visible_) {
    refreshEffectChainControls();
    effect_chain_summary_.getProperties().set("SectionName", section_name);
    effect_chain_selector_.getProperties().set("SectionName", section_name);
    effect_move_up_.getProperties().set("SectionName", section_name);
    effect_move_down_.getProperties().set("SectionName", section_name);
    effect_chain_summary_.setBounds(0, y, jmax(620, viewport_.getMaximumVisibleWidth()), 54);
    rows_container_.addAndMakeVisible(effect_chain_summary_);
    y += 60;

    auto addEffectControl = [&](Component& component) {
      component.setBounds(0, y, jmax(620, viewport_.getMaximumVisibleWidth()), 38);
      rows_container_.addAndMakeVisible(component);
      y += 44;
    };

    addEffectControl(effect_chain_selector_);
    addEffectControl(effect_move_up_);
    addEffectControl(effect_move_down_);
  }

  if (routing_controls_visible_)
    refreshRoutingControls();
  if (preset_controls_visible_)
    updatePresetSummary();
  if (modulation_controls_visible_) {
    refreshModulationRoutes();
    showSelectedModulationParameters();
  }
  if (lfo_mseg_controls_visible_) {
    refreshLfoMsegControls();
  }
  rows_container_.setSize(jmax(620, viewport_.getMaximumVisibleWidth()), y);
  viewport_.setViewPosition(0, 0);
  rebuildFocusOrder();

  if (announce)
    postPluginAnnouncement(accessibleSectionTitle(section_name) + ", " + String(rows_.size()) + " controls",
                                           AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::showAllSections(bool announce) {
  section_headers_.clear();
  rows_.clear();
  row_focus_order_.clear();
  top_level_accessibility_order_.clear();
  rows_container_.removeAllChildren();

  setPresetControlsVisible(true);
  configureWavetableActions({});
  setRoutingControlsVisible(false);
  setEffectChainControlsVisible(false);
  setModulationControlsVisible(false);
  setLfoMsegControlsVisible(false);

  int y = 0;
  const int width = jmax(760, viewport_.getMaximumVisibleWidth());
  int focus_order = 10;

  for (const auto& group : group_names_) {
    auto group_component = std::make_unique<AccessibleSectionHeader>(group, groupRank(group), true);
    auto* group_ptr = group_component.get();
    group_ptr->setExplicitFocusOrder(focus_order++);
    rows_container_.addAndMakeVisible(group_ptr);
    row_focus_order_.push_back(group_ptr);
    top_level_accessibility_order_.push_back(group_ptr);
    section_headers_.push_back(std::move(group_component));

    const auto found = group_sections_.find(group);
    if (found == group_sections_.end())
      continue;

    int group_y = group_ptr->headerHeight();
    if (group == kPresetSection) {
      preset_summary_.setBounds(16, group_y, jmax(620, width - 32), 54);
      group_ptr->addAndMakeVisible(preset_summary_);
      group_y += 60;

      auto addPresetControl = [&](Component& component) {
        component.setBounds(16, group_y, jmax(620, width - 32), 38);
        group_ptr->addAndMakeVisible(component);
        group_ptr->addAccessibleChild(&component);
        row_focus_order_.push_back(&component);
        group_y += 44;
      };

      addPresetControl(preset_menu_);
      addPresetControl(preset_library_);
      addPresetControl(preset_bank_);
      addPresetControl(preset_category_);
      addPresetControl(preset_search_);
      addPresetControl(preset_selector_);
      addPresetControl(preset_preview_);
      addPresetControl(preset_name_editor_);
    }

    for (const auto& section_name : found->second) {
      const bool flatten_section = found->second.size() == 1;
      AccessibleSectionHeader* section_ptr = group_ptr;
      int section_y = flatten_section ? group_y : 0;

      if (!flatten_section) {
        auto section_component = std::make_unique<AccessibleSectionHeader>(accessibleSectionTitle(section_name),
                                                                           groupRank(group), false, section_name);
        section_ptr = section_component.get();
        if (effectIdForSection(section_name).isNotEmpty()) {
          section_ptr->onKeyCommand = [this, section_name](const KeyPress& key, Component& target) {
            return handleEffectShortcut(section_name, key, target);
          };
        }
        section_ptr->setExplicitFocusOrder(focus_order++);
        group_ptr->addAndMakeVisible(section_ptr);
        group_ptr->addAccessibleChild(section_ptr);
        row_focus_order_.push_back(section_ptr);
        section_headers_.push_back(std::move(section_component));
        section_y = section_ptr->headerHeight();
      }

      if (section_name == "Modulation routing") {
        const int slot_width = jmax(620, width - (flatten_section ? 32 : 34));
        for (int slot = 1; slot <= vital::kMaxModulationConnections; ++slot) {
          const String title = modulationSlotTitle(slot);

          auto slot_component = std::make_unique<AccessibleSectionHeader>(title, groupRank(group), false);
          auto* slot_ptr = slot_component.get();
          slot_ptr->getProperties().set("ModulationSlot", slot);
          slot_ptr->setExplicitFocusOrder(focus_order++);
          slot_ptr->setBounds(flatten_section ? 16 : 10, section_y, slot_width, slot_ptr->headerHeight());
          section_ptr->addAndMakeVisible(slot_ptr);
          section_ptr->addAccessibleChild(slot_ptr);
          row_focus_order_.push_back(slot_ptr);
          section_headers_.push_back(std::move(slot_component));

          int slot_y = slot_ptr->headerHeight();
          const String prefix = "modulation_" + String(slot) + "_";
          for (auto* parameter : sections_[section_name]) {
            auto* bridge = dynamic_cast<ValueBridge*>(parameter);
            if (bridge == nullptr || !bridge->getParameterId().startsWith(prefix))
              continue;

            auto row = std::make_unique<AccessibleParameterRow>(*parameter);
            row->setModulationSourceSubmenuCallback([this](const String& destinationId,
                                                           std::map<int, String>& choices, int firstItemId) {
              return createModulationSourceSubmenu(destinationId, choices, firstItemId);
            });
            row->setModulationAssignCallback([this](const String& sourceId, const String& destinationId,
                                                    Component& target) {
              connectModulationAndPromptForAmount(sourceId, destinationId, target);
            });
            row->setModulationEditCallback([this](const String& sourceId, const String& destinationId,
                                                  Component& target) {
              promptForInitialModulationAmount(sourceId, destinationId, false, target);
            });
            row->setModulationDestinationPredicate([this](const String& id) { return isModulationDestinationId(id); });
            row->setModulationRemovalCallbacks(
                [this](const String& destinationId) { return modulationSourcesForDestination(destinationId); },
                [this](const String& sourceId, const String& destinationId, Component& target) {
                  removeModulationFromParameter(sourceId, destinationId, target);
                });
            row->setMidiLearnCallback([this](const String& parameterId, Component&, bool clear) {
              if (clear) {
                synth_.clearMidiLearn(parameterId.toStdString());
                postPluginAnnouncement("Cleared MIDI learn for " + modulationDestinationLabelForId(parameterId),
                                                       AccessibilityHandler::AnnouncementPriority::high);
              }
              else {
                synth_.armMidiLearn(parameterId.toStdString());
                postPluginAnnouncement("MIDI learn armed for " + modulationDestinationLabelForId(parameterId),
                                                       AccessibilityHandler::AnnouncementPriority::high);
              }
            });
            row->setValueEntryCallback([this](const String& parameterId, Component& target) {
              promptForParameterValue(parameterId, target);
            });
            row->setExtraCommandCallback([this, section_name](const String& parameterId, const KeyPress& key,
                                                              Component& target) {
              return handleMacroShortcut(parameterId, key, target) ||
                     handleEffectShortcut(section_name, key, target);
            });
            row->setAccessibleName(modulationControlTitle(bridge->getParameterId()));
            row->setExplicitFocusOrder(focus_order++);
            row->setControlFocusOrder(focus_order++);
            row->setBounds(10, slot_y, jmax(600, slot_width - 20), 48);
            slot_ptr->addAndMakeVisible(row.get());
            slot_ptr->addAccessibleChild(row->focusableControl());
            row_focus_order_.push_back(row->focusableControl());
            rows_.push_back(std::move(row));
            slot_y += 48;
          }

          slot_ptr->setBounds(flatten_section ? 16 : 10, section_y, slot_width, slot_y);
          section_y += slot_y + 8;
        }

        if (flatten_section)
          group_y = section_y + 8;
        else {
          section_ptr->setBounds(16, group_y, jmax(620, width - 24), section_y);
          group_y += section_y + 8;
        }
        continue;
      }

      auto addActionButton = [&](const String& title, const String& description, std::function<void()> action) {
        auto button = std::make_unique<OffscreenTextButton>(title);
        auto* button_ptr = button.get();
        button_ptr->setTitle(title);
        button_ptr->setDescription(description);
        button_ptr->setHelpText("Press Enter to " + description.toLowerCase());
        button_ptr->setWantsKeyboardFocus(true);
        button_ptr->onClick = std::move(action);
        button_ptr->setExplicitFocusOrder(focus_order++);
        button_ptr->setBounds(flatten_section ? 16 : 10, section_y,
                              jmax(620, width - (flatten_section ? 32 : 34)), 38);
        section_ptr->addAndMakeVisible(button_ptr);
        section_ptr->addAccessibleChild(button_ptr);
        row_focus_order_.push_back(button_ptr);
        section_headers_.push_back(std::move(button));
        section_y += 44;
      };

      auto addWavetableEditorControls = [&](int oscillator) {
        const int frames = wavetableFrameCount(oscillator);
        const int control_x = flatten_section ? 16 : 10;
        const int control_width = jmax(620, width - (flatten_section ? 32 : 34));

        auto summary = std::make_unique<Label>();
        auto* summary_ptr = summary.get();
        summary_ptr->setTitle("Wavetable editor");
        summary_ptr->setDescription(wavetableEditorSummary(oscillator));
        summary_ptr->setText(wavetableEditorSummary(oscillator), dontSendNotification);
        summary_ptr->setColour(Label::textColourId, Colours::white);
        summary_ptr->setBounds(control_x, section_y, control_width, 38);
        section_ptr->addAndMakeVisible(summary_ptr);
        section_headers_.push_back(std::move(summary));
        section_y += 44;

        auto scope = std::make_unique<OffscreenComboBox>();
        auto* scope_ptr = scope.get();
        scope_ptr->setTitle("Edit scope");
        scope_ptr->setDescription("Choose whether wavetable edits affect the selected frame or all frames");
        scope_ptr->setHelpText("Choose current frame for precise edits, or all frames for global harmonic changes");
        scope_ptr->addItem("Current frame", 1);
        scope_ptr->addItem("All frames", 2);
        scope_ptr->setSelectedId(1, dontSendNotification);
        scope_ptr->setWantsKeyboardFocus(true);
        scope_ptr->setExplicitFocusOrder(focus_order++);
        scope_ptr->setBounds(control_x, section_y, control_width, 38);
        section_ptr->addAndMakeVisible(scope_ptr);
        section_ptr->addAccessibleChild(scope_ptr);
        row_focus_order_.push_back(scope_ptr);
        section_headers_.push_back(std::move(scope));
        section_y += 44;

        auto addEditorSlider = [&](const String& title, const String& description, double minimum, double maximum,
                                   double value, double interval, const String& suffix) {
          auto slider = std::make_unique<OffscreenSlider>();
          auto* slider_ptr = slider.get();
          slider_ptr->setTitle(title);
          slider_ptr->setName(title);
          slider_ptr->setDescription(description);
          slider_ptr->setHelpText("Use arrow keys for small changes, Page Up and Page Down for larger changes");
          slider_ptr->setRange(minimum, maximum, interval);
          slider_ptr->setValue(value, dontSendNotification);
          slider_ptr->setWantsKeyboardFocus(true);
          slider_ptr->setExplicitFocusOrder(focus_order++);
          slider_ptr->textFromValueFunction = [suffix](double slider_value) {
            return String(slider_value, 0) + suffix;
          };
          slider_ptr->onTextEntryCommand = [this, safe_slider = Component::SafePointer<OffscreenSlider>(slider_ptr),
                                            title](Component& target) {
            if (safe_slider == nullptr)
              return;
            promptForCustomValue(title, safe_slider->getTextFromValue(safe_slider->getValue()), target,
                                 [safe_slider](const String& text) {
                                   if (safe_slider == nullptr)
                                     return;
                                   Slider::ScopedDragNotification drag(*safe_slider);
                                   safe_slider->setValue(safe_slider->getValueFromText(text), sendNotificationSync);
                                 });
          };
          slider_ptr->setBounds(control_x, section_y, control_width, 42);
          section_ptr->addAndMakeVisible(slider_ptr);
          section_ptr->addAccessibleChild(slider_ptr);
          row_focus_order_.push_back(slider_ptr);
          section_headers_.push_back(std::move(slider));
          section_y += 48;
          return slider_ptr;
        };

        auto* frame_slider = addEditorSlider("Frame", "Selected wavetable frame", 1.0, frames, 1.0, 1.0, "");
        auto* harmonic_slider = addEditorSlider("Harmonic", "Selected harmonic number", 1.0,
                                                vital::WaveFrame::kNumRealComplex - 2, 1.0, 1.0, "");
        auto* level_slider = addEditorSlider("Harmonic level", "Harmonic level as percent", 0.0, 400.0, 100.0, 1.0, "%");
        auto* phase_slider = addEditorSlider("Harmonic phase", "Harmonic phase in degrees", -180.0, 180.0, 0.0, 1.0, " degrees");

        auto selected_frame = [frame_slider] {
          return jmax(0, roundToInt(frame_slider->getValue()) - 1);
        };
        auto all_frames = [scope_ptr] {
          return scope_ptr->getSelectedId() == 2;
        };
        auto selected_harmonic = [harmonic_slider] {
          return roundToInt(harmonic_slider->getValue());
        };

        addActionButton("Set harmonic", "Set or add the selected harmonic at the chosen level and phase",
                        [this, oscillator, selected_frame, all_frames, selected_harmonic, level_slider, phase_slider] {
                          applyWavetableHarmonicEdit(oscillator, selected_frame(), all_frames(), selected_harmonic(),
                                                     static_cast<float>(level_slider->getValue()),
                                                     static_cast<float>(phase_slider->getValue()));
                        });
        addActionButton("Clear harmonic", "Clear the selected harmonic",
                        [this, oscillator, selected_frame, all_frames, selected_harmonic] {
                          clearWavetableHarmonic(oscillator, selected_frame(), all_frames(), selected_harmonic());
                        });
        addActionButton("Remove fundamental", "Remove harmonic 1",
                        [this, oscillator, selected_frame, all_frames] {
                          removeWavetableFundamental(oscillator, selected_frame(), all_frames());
                        });
        addActionButton("Normalize wavetable", "Normalize the current frame or all frames",
                        [this, oscillator, selected_frame, all_frames] {
                          normalizeWavetableFrame(oscillator, selected_frame(), all_frames());
                        });
      };

      if (section_name.startsWith("Oscillator ")) {
        const int oscillator = section_name.fromFirstOccurrenceOf("Oscillator ", false, false).getIntValue() - 1;
        if (isPositiveAndBelow(oscillator, vital::kNumOscillators)) {
          addActionButton("Load wavetable", "Load a wavetable into " + section_name, [this, oscillator] {
            active_oscillator_ = oscillator;
            chooseWavetableFile();
          });
          addActionButton("Import audio as wavetable splice",
                          "Import an audio file into " + section_name + " using wavetable splice",
                          [this, oscillator] {
                            active_oscillator_ = oscillator;
                            chooseWavetableFile(WavetableCreator::kWavetableSplice);
                          });
          addActionButton("Import audio as vocode",
                          "Import an audio file into " + section_name + " using vocode",
                          [this, oscillator] {
                            active_oscillator_ = oscillator;
                            chooseWavetableFile(WavetableCreator::kVocoded);
                          });
          addActionButton("Import audio as pitched wavetable",
                          "Import an audio file into " + section_name + " using pitched mode",
                          [this, oscillator] {
                            active_oscillator_ = oscillator;
                            chooseWavetableFile(WavetableCreator::kPitched);
                          });
          addActionButton("Browse wavetable folders", "Browse wavetable folders for " + section_name,
                          [this, oscillator, section_ptr] {
                            active_oscillator_ = oscillator;
                            showWavetableBrowserMenu(oscillator, *section_ptr);
                          });
          addActionButton("Previous wavetable", "Load the previous wavetable into " + section_name,
                          [this, oscillator] { loadShiftedWavetable(oscillator, -1); });
          addActionButton("Next wavetable", "Load the next wavetable into " + section_name,
                          [this, oscillator] { loadShiftedWavetable(oscillator, 1); });
          addActionButton("Export wavetable", "Save the current wavetable from " + section_name,
                          [this, oscillator] { saveWavetableFile(oscillator); });
          if (wavetable_editor_visible_[oscillator]) {
            addActionButton("Hide wavetable editor", "Hide the wavetable editor controls",
                            [this, oscillator] { setWavetableEditorVisible(oscillator, false); });
            addWavetableEditorControls(oscillator);
          }
          else {
            addActionButton("Show wavetable editor", "Show the wavetable editor controls",
                            [this, oscillator] { setWavetableEditorVisible(oscillator, true); });
          }
        }
      }
      else if (section_name == "Sample") {
        addActionButton("Load sample", "Load an audio sample", [this] { chooseSampleFile(); });
        addActionButton("Browse sample folders", "Browse sample folders",
                        [this, section_ptr] { showSampleBrowserMenu(*section_ptr); });
        addActionButton("Previous sample", "Load the previous sample", [this] { loadShiftedSample(-1); });
        addActionButton("Next sample", "Load the next sample", [this] { loadShiftedSample(1); });
      }
      else if (section_name == "Granular") {
        addActionButton("Load sample", "Load an audio sample for Granular",
                        [this] { chooseSampleFile(true); });
        addActionButton("Browse sample folders", "Browse sample folders for Granular",
                        [this, section_ptr] { showSampleBrowserMenu(*section_ptr, true); });
        addActionButton("Previous sample", "Load the previous sample into Granular",
                        [this] { loadShiftedSample(-1, true); });
        addActionButton("Next sample", "Load the next sample into Granular",
                        [this] { loadShiftedSample(1, true); });
      }
      else if (section_name == kSignalRoutingSection) {
        refreshRoutingControls();
        routing_summary_.setVisible(true);
        routing_mode_.setVisible(true);
        routing_default_.setVisible(true);
        routing_serial_forward_.setVisible(true);
        routing_serial_backward_.setVisible(true);

        routing_summary_.setBounds(flatten_section ? 16 : 10, section_y,
                                   jmax(620, width - (flatten_section ? 32 : 34)), 54);
        section_ptr->addAndMakeVisible(routing_summary_);
        section_y += 60;

        auto addRoutingControl = [&](Component& component) {
          component.setBounds(flatten_section ? 16 : 10, section_y,
                              jmax(620, width - (flatten_section ? 32 : 34)), 38);
          section_ptr->addAndMakeVisible(component);
          section_ptr->addAccessibleChild(&component);
          row_focus_order_.push_back(&component);
          section_y += 44;
        };

        addRoutingControl(routing_mode_);
        addRoutingControl(routing_default_);
        addRoutingControl(routing_serial_forward_);
        addRoutingControl(routing_serial_backward_);
      }
      else if (section_name.startsWith("LFO ")) {
        const int lfo = section_name.fromFirstOccurrenceOf("LFO ", false, false).getIntValue() - 1;
        if (isPositiveAndBelow(lfo, vital::kNumLfos)) {
          addActionButton("Import LFO preset", "Load an LFO shape into " + section_name,
                          [this, lfo] { chooseLfoPresetFile(lfo); });
          addActionButton("Save LFO preset", "Save the current shape from " + section_name,
                          [this, lfo] { saveLfoPresetFile(lfo); });

          auto shape = std::make_unique<OffscreenComboBox>();
          auto* shape_ptr = shape.get();
          shape_ptr->setTitle(section_name + " shape preset");
          shape_ptr->setDescription("Choose a standard shape for " + section_name);
          shape_ptr->setHelpText("Choose a shape, then use the apply shape button");
          shape_ptr->setWantsKeyboardFocus(true);
          shape_ptr->addItemList({ "Custom", "Sine", "Triangle", "Square", "Saw up", "Saw down",
                                   "Pulse", "Ramp hold", "Step climb", "Step fall",
                                   "Flat 0", "Flat 50", "Flat 100" }, 1);
          shape_ptr->setSelectedItemIndex(0, dontSendNotification);
          shape_ptr->setExplicitFocusOrder(focus_order++);
          shape_ptr->setBounds(flatten_section ? 16 : 10, section_y,
                               jmax(620, width - (flatten_section ? 32 : 34)), 38);
          section_ptr->addAndMakeVisible(shape_ptr);
          section_ptr->addAccessibleChild(shape_ptr);
          row_focus_order_.push_back(shape_ptr);
          section_headers_.push_back(std::move(shape));
          section_y += 44;

          auto apply_shape = std::make_unique<OffscreenTextButton>("Apply shape");
          auto* apply_shape_ptr = apply_shape.get();
          apply_shape_ptr->setTitle(section_name + " apply shape");
          apply_shape_ptr->setDescription("Replace " + section_name + " with the selected shape preset");
          apply_shape_ptr->setHelpText("Press Enter to apply the selected shape to " + section_name);
          apply_shape_ptr->setWantsKeyboardFocus(true);
          apply_shape_ptr->onClick = [this, lfo, shape_ptr] {
            active_lfo_index_ = lfo;
            lfo_mseg_shape_.setSelectedItemIndex(shape_ptr->getSelectedItemIndex(), dontSendNotification);
            applyLfoShapePreset();
          };
          apply_shape_ptr->setExplicitFocusOrder(focus_order++);
          apply_shape_ptr->setBounds(flatten_section ? 16 : 10, section_y,
                                     jmax(620, width - (flatten_section ? 32 : 34)), 38);
          section_ptr->addAndMakeVisible(apply_shape_ptr);
          section_ptr->addAccessibleChild(apply_shape_ptr);
          row_focus_order_.push_back(apply_shape_ptr);
          section_headers_.push_back(std::move(apply_shape));
          section_y += 44;

          auto keyboard = std::make_unique<LfoMsegKeyboardComponent>();
          auto* keyboard_ptr = keyboard.get();
          keyboard_ptr->setTitle(section_name + " accessible editor");
          keyboard_ptr->setDescription("Keyboard editor for " + section_name + " points, values, and curves");
          keyboard_ptr->onKeyPressed = [this, lfo](const KeyPress& key) {
            active_lfo_index_ = lfo;
            refreshLfoMsegControls();
            return handleLfoMsegShortcut(key);
          };
          keyboard_ptr->getStatusText = [this, lfo] { return lfoMsegStatusTextFor(lfo); };
          keyboard_ptr->setExplicitFocusOrder(focus_order++);
          keyboard_ptr->setBounds(flatten_section ? 16 : 10, section_y,
                                  jmax(620, width - (flatten_section ? 32 : 34)), 74);
          section_ptr->addAndMakeVisible(keyboard_ptr);
          section_ptr->addAccessibleChild(keyboard_ptr);
          row_focus_order_.push_back(keyboard_ptr);
          section_headers_.push_back(std::move(keyboard));
          section_y += 82;
        }
      }
      else if (effectIdForSection(section_name).isNotEmpty()) {
        addActionButton("Import FX preset", "Load an FX preset into " + accessibleSectionTitle(section_name),
                        [this, section_name] { chooseEffectPresetFile(section_name); });
        addActionButton("Save FX preset", "Save the current " + accessibleSectionTitle(section_name) + " settings",
                        [this, section_name] { saveEffectPresetFile(section_name); });
      }

      for (auto* parameter : sections_[section_name]) {
        if (auto* bridge = dynamic_cast<ValueBridge*>(parameter))
          if (bridge->getParameterId().endsWith("effect_chain_order") ||
              bridge->getParameterId().endsWith("post_effect_order") ||
              bridge->getParameterId().contains("_effect_chain_slot_") ||
              bridge->getParameterId().startsWith("effect_chain_slot_"))
            continue;
        if (!shouldShowParameterInSection(section_name, parameter))
          continue;

        auto row = createAccessibleParameterRow(*parameter, section_name);
        row->setModulationSourceSubmenuCallback([this](const String& destinationId,
                                                       std::map<int, String>& choices, int firstItemId) {
          return createModulationSourceSubmenu(destinationId, choices, firstItemId);
        });
        row->setModulationAssignCallback([this](const String& sourceId, const String& destinationId,
                                                Component& target) {
          connectModulationAndPromptForAmount(sourceId, destinationId, target);
        });
        row->setModulationEditCallback([this](const String& sourceId, const String& destinationId,
                                              Component& target) {
          promptForInitialModulationAmount(sourceId, destinationId, false, target);
        });
        row->setModulationDestinationPredicate([this](const String& id) { return isModulationDestinationId(id); });
        row->setModulationRemovalCallbacks(
            [this](const String& destinationId) { return modulationSourcesForDestination(destinationId); },
            [this](const String& sourceId, const String& destinationId, Component& target) {
              removeModulationFromParameter(sourceId, destinationId, target);
            });
        row->setMidiLearnCallback([this](const String& parameterId, Component&, bool clear) {
          if (clear) {
            synth_.clearMidiLearn(parameterId.toStdString());
            postPluginAnnouncement("Cleared MIDI learn for " + modulationDestinationLabelForId(parameterId),
                                                   AccessibilityHandler::AnnouncementPriority::high);
          }
          else {
            synth_.armMidiLearn(parameterId.toStdString());
            postPluginAnnouncement("MIDI learn armed for " + modulationDestinationLabelForId(parameterId),
                                                   AccessibilityHandler::AnnouncementPriority::high);
          }
        });
        row->setValueEntryCallback([this](const String& parameterId, Component& target) {
          promptForParameterValue(parameterId, target);
        });
        row->setExtraCommandCallback([this, section_name](const String& parameterId, const KeyPress& key,
                                                          Component& target) {
          return handleMacroShortcut(parameterId, key, target) ||
                 handleEffectShortcut(section_name, key, target);
        });
        wireSampleLoopFineTune(*row);
        row->setExplicitFocusOrder(focus_order++);
        row->setControlFocusOrder(focus_order++);
        row->setBounds(flatten_section ? 16 : 10, section_y, jmax(620, width - (flatten_section ? 32 : 34)), 48);
        section_ptr->addAndMakeVisible(row.get());
        section_ptr->addAccessibleChild(row->focusableControl());
        row_focus_order_.push_back(row->focusableControl());
        rows_.push_back(std::move(row));
        section_y += 48;
      }

      if (isEffectChainSection(section_name)) {
        auto summary = std::make_unique<Label>();
        auto* summary_ptr = summary.get();
        summary_ptr->setTitle("Effects chain order");
        summary_ptr->setDescription("Current order of the effects chain");
        summary_ptr->getProperties().set("SectionName", section_name);
        summary_ptr->setColour(Label::textColourId, Colours::white);
        summary_ptr->setJustificationType(Justification::centredLeft);
        summary_ptr->setBounds(flatten_section ? 16 : 10, section_y,
                               jmax(620, width - (flatten_section ? 32 : 34)), 54);
        section_ptr->addAndMakeVisible(summary_ptr);
        section_ptr->addAccessibleChild(summary_ptr);
        section_headers_.push_back(std::move(summary));
        section_y += 60;

        auto addEffectControl = [&](Component& component) {
          component.setBounds(flatten_section ? 16 : 10, section_y,
                              jmax(620, width - (flatten_section ? 32 : 34)), 38);
          section_ptr->addAndMakeVisible(component);
          section_ptr->addAccessibleChild(&component);
          row_focus_order_.push_back(&component);
          section_y += 44;
        };

        auto selector = std::make_unique<AccessibleComboBox>();
        auto* selector_ptr = selector.get();
        selector_ptr->setTitle("Effect position");
        selector_ptr->setDescription("Choose an effect in the current chain order");
        selector_ptr->setHelpText("Choose an effect, then use the move buttons to reorder it in the chain");
        selector_ptr->getProperties().set("SectionName", section_name);
        selector_ptr->setWantsKeyboardFocus(true);
        selector_ptr->setExplicitFocusOrder(focus_order++);
        populateEffectChainSelector(*selector_ptr, summary_ptr, section_name);
        addEffectControl(*selector_ptr);
        section_headers_.push_back(std::move(selector));

        auto moveLocalEffect = [this, section_name, selector_ptr](int direction, const String& focusedTitle) {
          const int selected = selector_ptr->getSelectedItemIndex();
          const int next = selected + direction;
          if (!isPositiveAndBelow(selected, vital::constants::kNumEffects) ||
              !isPositiveAndBelow(next, vital::constants::kNumEffects))
            return;

          int order[vital::constants::kNumEffects];
          readEffectChainOrder(section_name, order);
          std::swap(order[selected], order[next]);
          const String moved = effectName(order[next]);
          writeEffectChainOrder(section_name, order);
          pending_effect_chain_section_ = section_name;
          pending_effect_chain_selected_index_ = next;
          MessageManager::callAsync([this, section_name, focusedTitle] {
            rebuildSectionsAfterEffectOrderChange(section_name, focusedTitle);
          });
          postPluginAnnouncement(moved + " moved to position " + String(next + 1),
                                                 AccessibilityHandler::AnnouncementPriority::high);
        };

        auto move_up = std::make_unique<AccessibleTextButton>("Move effect earlier");
        auto* move_up_ptr = move_up.get();
        move_up_ptr->setTitle("Move effect earlier");
        move_up_ptr->setDescription("Move the selected effect one position earlier in the effects chain");
        move_up_ptr->setHelpText("Press Enter to move the selected effect earlier");
        move_up_ptr->getProperties().set("SectionName", section_name);
        move_up_ptr->setWantsKeyboardFocus(true);
        move_up_ptr->setExplicitFocusOrder(focus_order++);
        move_up_ptr->onClick = [moveLocalEffect] { moveLocalEffect(-1, "Move effect earlier"); };
        addEffectControl(*move_up_ptr);
        section_headers_.push_back(std::move(move_up));

        auto move_down = std::make_unique<AccessibleTextButton>("Move effect later");
        auto* move_down_ptr = move_down.get();
        move_down_ptr->setTitle("Move effect later");
        move_down_ptr->setDescription("Move the selected effect one position later in the effects chain");
        move_down_ptr->setHelpText("Press Enter to move the selected effect later");
        move_down_ptr->getProperties().set("SectionName", section_name);
        move_down_ptr->setWantsKeyboardFocus(true);
        move_down_ptr->setExplicitFocusOrder(focus_order++);
        move_down_ptr->onClick = [moveLocalEffect] { moveLocalEffect(1, "Move effect later"); };
        addEffectControl(*move_down_ptr);
        section_headers_.push_back(std::move(move_down));
      }

      if (flatten_section)
        group_y = section_y + 8;
      else {
        section_ptr->setBounds(16, group_y, jmax(620, width - 24), section_y);
        group_y += section_y + 8;
      }
    }
    group_ptr->setBounds(0, y, width, group_y + 8);
    y += group_y + 12;
  }

  rows_container_.setSize(width, y);
  viewport_.setViewPosition(0, 0);
  rebuildFocusOrder();

  if (announce)
    postPluginAnnouncement("All controls visible, grouped like Surge, " +
                                             String(rows_.size()) + " controls",
                                             AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::showNavigationMenu() {
  PopupMenu menu;
  menu.addSectionHeader("Editor");
  menu.addItem(1, "Save patch as default");
  menu.addItem(2, "Initialize patch");
  menu.addItem(3, "Save patch as...");
  menu.addItem(5, "Export preset...");
  menu.addSeparator();
  menu.addItem(6, "Load tuning file...");
  if (!synth_.getTuning()->isDefault())
    menu.addItem(7, "Clear tuning: " + String(synth_.getTuning()->getName()));
  menu.addSeparator();
  menu.addItem(4, "Accessibility settings");

  menu.showMenuAsync(PopupMenu::Options().withTargetComponent(menu_button_),
                     [this](int result) {
    if (result == 1)
      savePatchAsDefault();
    else if (result == 2)
      initializePatch();
    else if (result == 3)
      savePresetAs();
    else if (result == 5)
      exportPreset();
    else if (result == 6)
      loadTuningFile();
    else if (result == 7)
      clearTuning();
    else if (result == 4)
      showAccessibilitySettingsMenu();
  });
}

void SynthEditor::showAccessibilitySettingsMenu() {
  struct Entry {
    const char* label;
    bool AccessibilitySpeechSettings::* field;
  };

  static const Entry kEntries[] = {
    { "Use speech feedback",               &AccessibilitySpeechSettings::speech_feedback  },
    { "Navigation announcements",          &AccessibilitySpeechSettings::navigation       },
    { "Preset announcements",              &AccessibilitySpeechSettings::presets          },
    { "LFO editor announcements",          &AccessibilitySpeechSettings::lfo_editor       },
    { "Wavetable editor announcements",    &AccessibilitySpeechSettings::wavetable_editor },
    { "Sample and granular announcements", &AccessibilitySpeechSettings::sample_browser   },
    { "Modulation announcements",          &AccessibilitySpeechSettings::modulation       },
    { "Parameter value announcements",     &AccessibilitySpeechSettings::parameters       },
    { "Other announcements",               &AccessibilitySpeechSettings::other            },
    { "Frequency values in Hz",            &AccessibilitySpeechSettings::frequency_values_in_hz },
  };

  class DialogContent : public Component {
  public:
    DialogContent() {
      auto& settings = accessibilitySpeechSettings();
      for (const auto& e : kEntries) {
        auto btn = std::make_unique<ToggleButton>(e.label);
        btn->setToggleState(settings.*(e.field), dontSendNotification);
        auto* btn_ptr = btn.get();
        auto field = e.field;
        btn->onStateChange = [btn_ptr, field]() {
          accessibilitySpeechSettings().*field = btn_ptr->getToggleState();
          saveAccessibilitySpeechSettings();
        };
        addAndMakeVisible(*btn);
        buttons_.push_back(std::move(btn));
      }
      setSize(360, static_cast<int>(sizeof(kEntries) / sizeof(kEntries[0])) * 28 + 16);
    }

    void resized() override {
      int y = 8;
      for (auto& btn : buttons_) {
        btn->setBounds(8, y, getWidth() - 16, 24);
        y += 28;
      }
    }

  private:
    std::vector<std::unique_ptr<ToggleButton>> buttons_;
  };

  DialogWindow::LaunchOptions opts;
  opts.dialogTitle = "Accessibility Settings";
  opts.content.setOwned(new DialogContent());
  opts.componentToCentreAround = this;
  opts.useNativeTitleBar = true;
  opts.resizable = false;
  opts.escapeKeyTriggersCloseButton = true;
  auto* dlg = opts.launchAsync();
  if (dlg) {
    Component::SafePointer<DialogWindow> safe_dlg(dlg);
    MessageManager::callAsync([safe_dlg]() {
      if (safe_dlg && safe_dlg->getContentComponent()) {
        if (auto* first = safe_dlg->getContentComponent()->getChildComponent(0))
          first->grabKeyboardFocus();
      }
    });
  }
}

File SynthEditor::defaultPatchFile() const {
  return LoadSave::getUserPresetDirectory().getChildFile("Default").withFileExtension(vital::kPresetExtension);
}

void SynthEditor::initializePatch() {
  const String parameter_id = focusedParameterId();
  const String accessible_title = focusedAccessibleTitle();
  auto* persistent_focus = persistentFocusedComponent();
  const File default_file = defaultPatchFile();
  std::string error;
  if (default_file.existsAsFile() && synth_.loadFromFile(default_file, error)) {
    last_preset_path = default_file.getFullPathName();
    synth_.updateHostDisplay();
    updateFullGui();
    selectPresetFile(default_file);
    updatePresetSummary();
    restoreFocusAfterRebuild(parameter_id, persistent_focus, accessible_title);
    postPluginAnnouncement("Loaded default patch", AccessibilityHandler::AnnouncementPriority::high);
    return;
  }

  synth_.loadInitPreset();
  synth_.updateHostDisplay();
  updateFullGui();
  refreshPresetList();
  restoreFocusAfterRebuild(parameter_id, persistent_focus, accessible_title);
  postPluginAnnouncement("Initialized patch", AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::savePatchAsDefault() {
  const File destination = defaultPatchFile().withFileExtension(vital::kPresetExtension);
  if (!synth_.saveCopyToFile(destination)) {
    NativeMessageBox::showMessageBoxAsync(MessageBoxIconType::WarningIcon, "Unable to save default patch",
                                          "The default patch could not be written.");
    postPluginAnnouncement("Unable to save default patch", AccessibilityHandler::AnnouncementPriority::high);
    return;
  }

  refreshPresetList();
  updatePresetSummary();
  postPluginAnnouncement("Saved patch as default", AccessibilityHandler::AnnouncementPriority::high);
}


void SynthEditor::rebuildFocusOrder() {
  focus_order_.clear();
  accessibility_focus_order_.clear();
  focus_order_.push_back(&menu_button_);
  accessibility_focus_order_.push_back(&menu_button_);
  if (modulation_amount_prompt_visible_ || parameter_value_prompt_visible_) {
    focus_order_.push_back(&modulation_amount_editor_);
    focus_order_.push_back(&modulation_amount_ok_);
    focus_order_.push_back(&modulation_amount_cancel_);
    accessibility_focus_order_.push_back(&modulation_amount_editor_);
    accessibility_focus_order_.push_back(&modulation_amount_ok_);
    accessibility_focus_order_.push_back(&modulation_amount_cancel_);
  }
  if (save_patch_dialog_visible_) {
    focus_order_.push_back(&save_patch_name_);
    focus_order_.push_back(&save_patch_author_);
    focus_order_.push_back(&save_patch_bank_);
    focus_order_.push_back(&save_patch_category_);
    focus_order_.push_back(&save_patch_tags_);
    focus_order_.push_back(&save_patch_ok_);
    focus_order_.push_back(&save_patch_cancel_);
    accessibility_focus_order_.push_back(&save_patch_name_);
    accessibility_focus_order_.push_back(&save_patch_author_);
    accessibility_focus_order_.push_back(&save_patch_bank_);
    accessibility_focus_order_.push_back(&save_patch_category_);
    accessibility_focus_order_.push_back(&save_patch_tags_);
    accessibility_focus_order_.push_back(&save_patch_ok_);
    accessibility_focus_order_.push_back(&save_patch_cancel_);
  }
  if (show_all_sections_) {
    for (auto* component : row_focus_order_)
      focus_order_.push_back(component);
    for (auto* component : top_level_accessibility_order_)
      accessibility_focus_order_.push_back(component);
    return;
  }

  focus_order_.push_back(&group_selector_);
  focus_order_.push_back(&section_selector_);
  if (preset_controls_visible_) {
    focus_order_.push_back(&preset_menu_);
    focus_order_.push_back(&preset_library_);
    focus_order_.push_back(&preset_bank_);
    focus_order_.push_back(&preset_category_);
    focus_order_.push_back(&preset_search_);
    focus_order_.push_back(&preset_selector_);
    focus_order_.push_back(&preset_preview_);
    focus_order_.push_back(&preset_name_editor_);
  }
  if (wavetable_name_.isVisible()) {
    focus_order_.push_back(&load_wavetable_);
    focus_order_.push_back(&reset_wavetable_);
    focus_order_.push_back(&oscillator_octave_);
    focus_order_.push_back(&oscillator_semitone_);
    focus_order_.push_back(&oscillator_fine_tune_);
    focus_order_.push_back(&oscillator_wave_frame_);
    focus_order_.push_back(&oscillator_scale_key_);
    focus_order_.push_back(&oscillator_scale_type_);
    focus_order_.push_back(&oscillator_scale_mode_);
  }
  if (routing_controls_visible_) {
    focus_order_.push_back(&routing_mode_);
    focus_order_.push_back(&routing_default_);
    focus_order_.push_back(&routing_serial_forward_);
    focus_order_.push_back(&routing_serial_backward_);
  }
  if (effect_chain_controls_visible_) {
    focus_order_.push_back(&effect_chain_selector_);
    focus_order_.push_back(&effect_move_up_);
    focus_order_.push_back(&effect_move_down_);
  }
  if (modulation_controls_visible_) {
    focus_order_.push_back(&modulation_source_);
    focus_order_.push_back(&modulation_destination_group_);
    focus_order_.push_back(&modulation_destination_);
    focus_order_.push_back(&add_modulation_);
    focus_order_.push_back(&remove_modulation_);
    focus_order_.push_back(&modulation_list_);
  }
  if (lfo_mseg_controls_visible_) {
    focus_order_.push_back(&lfo_mseg_lfo_);
    focus_order_.push_back(&lfo_mseg_mode_);
    focus_order_.push_back(&lfo_mseg_cycle_);
    focus_order_.push_back(&lfo_mseg_grid_);
    focus_order_.push_back(&lfo_mseg_shape_);
    focus_order_.push_back(&lfo_mseg_apply_shape_);
    focus_order_.push_back(&lfo_mseg_keyboard_);
    focus_order_.push_back(&lfo_mseg_point_);
    focus_order_.push_back(&lfo_mseg_add_point_);
    focus_order_.push_back(&lfo_mseg_delete_point_);
    focus_order_.push_back(&lfo_mseg_time_down_);
    focus_order_.push_back(&lfo_mseg_time_up_);
    focus_order_.push_back(&lfo_mseg_value_down_);
    focus_order_.push_back(&lfo_mseg_value_up_);
    focus_order_.push_back(&lfo_mseg_curve_);
    focus_order_.push_back(&lfo_mseg_smooth_);
  }
  for (auto* component : row_focus_order_)
    focus_order_.push_back(component);
  for (auto& row : rows_)
    focus_order_.push_back(row->focusableControl());

  accessibility_focus_order_ = focus_order_;
}

std::unique_ptr<ComponentTraverser> SynthEditor::createFocusTraverser() {
  return std::make_unique<OrderedFocusTraverser>(*this, false);
}

std::unique_ptr<ComponentTraverser> SynthEditor::createKeyboardFocusTraverser() {
  return std::make_unique<OrderedFocusTraverser>(*this, true);
}

void SynthEditor::ensureComponentVisible(Component* component) {
  if (component == nullptr)
    return;

  Component* parent = component;
  bool inside_rows = false;
  while (parent != nullptr) {
    if (parent == &rows_container_) {
      inside_rows = true;
      break;
    }
    parent = parent->getParentComponent();
  }
  if (!inside_rows)
    return;

  const auto area = rows_container_.getLocalArea(component, component->getLocalBounds()).expanded(0, 12);
  const int current_y = viewport_.getViewPositionY();
  const int visible_height = viewport_.getViewHeight();
  int target_y = current_y;
  if (area.getY() < current_y)
    target_y = area.getY();
  else if (area.getBottom() > current_y + visible_height)
    target_y = area.getBottom() - visible_height;
  target_y = jlimit(0, jmax(0, rows_container_.getHeight() - visible_height), target_y);
  if (target_y != current_y)
    viewport_.setViewPosition(0, target_y);
}

String SynthEditor::modulationDestinationsForSource(const String& sourceId) const {
  StringArray destinations;
  for (auto* route : synth_.getModulationConnections()) {
    if (route == nullptr || route->source_name != sourceId.toStdString() || route->destination_name.empty())
      continue;

    const int index = synth_.getConnectionIndex(route->source_name, route->destination_name);
    float amount = 0.0f;
    if (index >= 0) {
      if (auto* amount_bridge = parameterBridge("modulation_" + String(index + 1) + "_amount"))
        amount = amount_bridge->convertToEngineValue(amount_bridge->getValue());
    }

    const String destination = vital::Parameters::isParameter(route->destination_name)
        ? vital::Parameters::getDisplayName(route->destination_name)
        : readableId(route->destination_name);
    const String amount_text = amount == 0.0f ? String("0")
        : String(amount > 0.0f ? "+" : "") + String(amount, 2);
    destinations.add(destination + " " + amount_text);
  }
  return destinations.joinIntoString(", ");
}

std::vector<std::pair<String, String>> SynthEditor::modulationSourcesForDestination(const String& destinationId) const {
  std::vector<std::pair<String, String>> sources;
  StringArray seen_sources;
  for (auto* route : synth_.getModulationConnections()) {
    if (route == nullptr || route->source_name.empty() || route->destination_name != destinationId.toStdString())
      continue;

    const String source_id(route->source_name);
    if (seen_sources.contains(source_id))
      continue;

    seen_sources.add(source_id);
    sources.push_back({ source_id, modulationSourceLabelForId(source_id) });
  }

  std::sort(sources.begin(), sources.end(), [](const auto& a, const auto& b) {
    return a.second.compareNatural(b.second) < 0;
  });
  return sources;
}

void SynthEditor::removeModulationFromParameter(const String& sourceId, const String& destinationId, Component& target) {
  if (sourceId.isEmpty() || destinationId.isEmpty())
    return;

  const String source_label = modulationSourceLabelForId(sourceId);
  const String destination_label = modulationDestinationLabelForId(destinationId);
  synth_.disconnectModulation(sourceId.toStdString(), destinationId.toStdString());
  refreshModulationRoutes();
  target.grabKeyboardFocus();
  postPluginAnnouncement("Removed modulation from " + source_label + " to " + destination_label,
                                         AccessibilityHandler::AnnouncementPriority::high);
}

String SynthEditor::modulationSlotTitle(int slot) const {
  String title = modulationSlotHeaderTitle(slot);
  auto* connection = synth_.getModulationBank().atIndex(slot - 1);
  if (connection == nullptr)
    return title;

  if (!connection->source_name.empty()) {
    const String source = readableId(connection->source_name);
    const String destinations = modulationDestinationsForSource(connection->source_name);
    return destinations.isEmpty() ? title + ", " + source
                                  : title + ", " + source + " (" + destinations + ")";
  }

  if (!connection->destination_name.empty())
    title += " (" + modulationDestinationLabelForId(connection->destination_name) + ")";
  return title;
}

String SynthEditor::modulationSlotHeaderTitle(int slot) const {
  return "Modulation " + numberWord(slot);
}

String SynthEditor::modulationControlTitle(const String& parameterId) const {
  const String rest = parameterId.fromFirstOccurrenceOf("modulation_", false, false);
  const String suffix = rest.fromFirstOccurrenceOf("_", false, false);
  if (suffix == "amount")
    return "Amount";
  if (suffix == "power")
    return "Power";
  if (suffix == "bipolar")
    return "Bipolar";
  if (suffix == "stereo")
    return "Stereo";
  if (suffix == "bypass")
    return "Bypass";
  return readableId(suffix);
}

void SynthEditor::setPresetControlsVisible(bool visible) {
  preset_controls_visible_ = visible;
  if (!show_all_sections_) {
    addChildComponent(preset_summary_);
    addChildComponent(preset_menu_);
    addChildComponent(preset_library_);
    addChildComponent(preset_bank_);
    addChildComponent(preset_category_);
    addChildComponent(preset_search_);
    addChildComponent(preset_selector_);
    addChildComponent(preset_preview_);
    addChildComponent(preset_name_editor_);
    addChildComponent(save_patch_prompt_);
    addChildComponent(save_patch_name_);
    addChildComponent(save_patch_author_);
    addChildComponent(save_patch_bank_);
    addChildComponent(save_patch_category_);
    addChildComponent(save_patch_tags_);
    addChildComponent(save_patch_ok_);
    addChildComponent(save_patch_cancel_);
  }
  preset_summary_.setVisible(visible);
  preset_menu_.setVisible(visible);
  preset_library_.setVisible(visible);
  preset_bank_.setVisible(visible);
  preset_category_.setVisible(visible);
  preset_search_.setVisible(visible);
  preset_selector_.setVisible(visible);
  preset_preview_.setVisible(visible);
  preset_name_editor_.setVisible(visible);
  if (!visible)
    hideSavePresetDialog();
  if (visible)
    updatePresetSummary();
  resized();
}

void SynthEditor::ensurePresetListLoaded() {
  if (preset_list_loaded_ || preset_list_loading_)
    return;

  startPresetListLoad(false);
}

void SynthEditor::refreshPresetList(bool announce) {
  startPresetListLoad(announce, true);
}

void SynthEditor::startPresetListLoad(bool announce, bool forceRefresh) {
  const int generation = ++preset_list_generation_;

  if (!forceRefresh) {
    std::vector<PresetBrowserItem> cached_items;
    bool cache_valid = false;
    {
      const ScopedLock lock(presetBrowserCacheLock());
      cache_valid = cachedPresetBrowserItemsValid();
      if (cache_valid)
        cached_items = cachedPresetBrowserItems();
    }

    if (cache_valid) {
      applyPresetList(std::move(cached_items), announce, generation);
      return;
    }
  }

  preset_list_loaded_ = false;
  preset_list_loading_ = true;
  filtered_presets_.clear();
  preset_selector_.clear(dontSendNotification);
  updatePresetSummary();

  Component::SafePointer<SynthEditor> safe_this(this);
  Thread::launch([safe_this, generation, announce, forceRefresh] {
    auto persisted_items = forceRefresh ? std::map<std::string, PresetBrowserItem>()
                                        : loadPresetIndexDatabase();
    bool displayed_persisted_database = false;
    if (!forceRefresh && !persisted_items.empty()) {
      std::vector<PresetBrowserItem> database_items;
      database_items.reserve(persisted_items.size());
      for (const auto& [path, item] : persisted_items)
        database_items.push_back(item);

      {
        const ScopedLock lock(presetBrowserCacheLock());
        cachedPresetBrowserItems() = database_items;
        cachedPresetBrowserItemsValid() = true;
      }

      displayed_persisted_database = true;
      MessageManager::callAsync([safe_this, generation, database_items = std::move(database_items)]() mutable {
        if (safe_this != nullptr)
          safe_this->applyPresetList(std::move(database_items), false, generation);
      });
    }

    Array<File> presets;
    LoadSave::getAllPresets(presets);
    LoadSave::FileSorterAscending sorter;
    presets.sort(sorter);

    const File data_directory = LoadSave::getDataDirectory();
    std::vector<PresetBrowserItem> items;
    items.reserve(static_cast<size_t>(presets.size()));
    bool database_changed = false;
    for (const auto& preset : presets) {
      const String key = presetPathKey(preset);
      auto found = persisted_items.find(key.toStdString());
      if (found != persisted_items.end() && presetFileStampMatches(found->second, preset)) {
        items.push_back(found->second);
        continue;
      }

      items.push_back(buildPresetBrowserItem(preset, data_directory));
      database_changed = true;
    }
    if (forceRefresh || persisted_items.size() != static_cast<size_t>(presets.size()))
      database_changed = true;
    if (database_changed)
      savePresetIndexDatabase(items);

    {
      const ScopedLock lock(presetBrowserCacheLock());
      cachedPresetBrowserItems() = items;
      cachedPresetBrowserItemsValid() = true;
    }

    if (displayed_persisted_database && !database_changed && !announce)
      return;

    MessageManager::callAsync([safe_this, generation, announce, items = std::move(items)]() mutable {
      if (safe_this != nullptr)
        safe_this->applyPresetList(std::move(items), announce, generation);
    });
  });
}

void SynthEditor::applyPresetList(std::vector<PresetBrowserItem> items, bool announce, int generation) {
  if (generation != preset_list_generation_)
    return;

  preset_index_ = std::move(items);
  preset_list_loading_ = false;
  preset_list_loaded_ = true;
  all_presets_.clear();
  preset_index_by_path_.clear();
  preset_index_by_path_.reserve(preset_index_.size());
  for (size_t i = 0; i < preset_index_.size(); ++i) {
    const auto& item = preset_index_[i];
    all_presets_.add(item.file);
    preset_index_by_path_[item.file.getFullPathName().toStdString()] = i;
  }
  populatePresetFilters();
  filterPresetList();
  updatePresetSummary();
  if (announce)
    postPluginAnnouncement("Preset list refreshed, " + String(all_presets_.size()) + " presets found",
                                           AccessibilityHandler::AnnouncementPriority::medium);
}

const PresetBrowserItem* SynthEditor::presetItemForFile(const File& file) const {
  const auto found = preset_index_by_path_.find(file.getFullPathName().toStdString());
  if (found == preset_index_by_path_.end() || found->second >= preset_index_.size())
    return nullptr;
  return &preset_index_[found->second];
}

void SynthEditor::populatePresetFilters() {
  ScopedValueSetter<bool> guard(updating_preset_list_, true);

  preset_libraries_.clear();
  preset_libraries_.add(kAllLibraries);
  std::set<String> libraries;
  for (const auto& item : preset_index_) {
    if (item.library.isNotEmpty())
      libraries.insert(item.library);
  }
  for (const auto& library : libraries) {
    if (library != kAllLibraries)
      preset_libraries_.add(library);
  }

  preset_library_.clear(dontSendNotification);
  preset_library_.addItemList(preset_libraries_, 1);
  int library_index = preset_libraries_.indexOf(last_preset_library);
  if (library_index < 0)
    library_index = 0;
  preset_library_.setSelectedItemIndex(library_index, dontSendNotification);
  last_preset_library = preset_library_.getText();

  preset_banks_.clear();
  preset_banks_.add(kAllBanks);
  std::set<String> banks;
  for (const auto& item : preset_index_) {
    if (last_preset_library != kAllLibraries && item.library != last_preset_library)
      continue;
    if (item.bank.isNotEmpty())
      banks.insert(item.bank);
  }
  for (const auto& bank : banks) {
    if (bank != kAllBanks)
      preset_banks_.add(bank);
  }

  preset_bank_.clear(dontSendNotification);
  preset_bank_.addItemList(preset_banks_, 1);
  int bank_index = preset_banks_.indexOf(last_preset_bank);
  if (bank_index < 0)
    bank_index = 0;
  preset_bank_.setSelectedItemIndex(bank_index, dontSendNotification);
  last_preset_bank = preset_bank_.getText();

  preset_categories_.clear();
  preset_categories_.add(kAllCategories);
  std::set<String> categories;
  for (const auto& item : preset_index_) {
    if (last_preset_library != kAllLibraries && item.library != last_preset_library)
      continue;
    if (last_preset_bank != kAllBanks && item.bank != last_preset_bank)
      continue;
    if (item.category.isNotEmpty())
      categories.insert(item.category);
    for (const auto& tag : item.tags) {
      if (tag.isNotEmpty())
        categories.insert(tag);
    }
  }
  for (const auto& category : categories) {
    if (category != kAllCategories)
      preset_categories_.add(category);
  }

  preset_category_.clear(dontSendNotification);
  preset_category_.addItemList(preset_categories_, 1);
  int category_index = preset_categories_.indexOf(last_preset_category);
  if (category_index < 0)
    category_index = 0;
  preset_category_.setSelectedItemIndex(category_index, dontSendNotification);
  last_preset_category = preset_category_.getText();
}

void SynthEditor::filterPresetList() {
  if (!preset_list_loaded_) {
    filtered_presets_.clear();
    preset_selector_.clear(dontSendNotification);
    updatePresetSummary();
    return;
  }

  ScopedValueSetter<bool> guard(updating_preset_list_, true);
  filtered_presets_.clear();
  const String filter = preset_search_.getText().trim().toLowerCase();
  const String library_filter = preset_library_.getText().isEmpty() ? kAllLibraries : preset_library_.getText();
  const String bank_filter = preset_bank_.getText().isEmpty() ? kAllBanks : preset_bank_.getText();
  const String category_filter = preset_category_.getText().isEmpty() ? kAllCategories : preset_category_.getText();

  for (const auto& item : preset_index_) {
    if (library_filter != kAllLibraries && item.library != library_filter)
      continue;
    if (bank_filter != kAllBanks && item.bank != bank_filter)
      continue;
    if (category_filter != kAllCategories && item.category != category_filter && !item.tags.contains(category_filter))
      continue;

    if (filter.isEmpty() || item.searchable.contains(filter))
      filtered_presets_.add(item.file);
  }

  const String previous_path = last_preset_path.isNotEmpty() ? last_preset_path
                             : synth_.getActiveFile().existsAsFile() ? synth_.getActiveFile().getFullPathName()
                             : String();
  preset_selector_.clear(dontSendNotification);
  for (int i = 0; i < filtered_presets_.size(); ++i) {
    const auto& preset = filtered_presets_.getReference(i);
    if (const auto* item = presetItemForFile(preset))
      preset_selector_.addItem(item->label, i + 1);
    else
      preset_selector_.addItem(preset.getFileNameWithoutExtension(), i + 1);
  }
  if (!filtered_presets_.isEmpty()) {
    int selected = 0;
    for (int i = 0; i < filtered_presets_.size(); ++i) {
      if (filtered_presets_[i].getFullPathName() == previous_path) {
        selected = i;
        break;
      }
    }
    preset_selector_.setSelectedItemIndex(selected, dontSendNotification);
    last_preset_path = filtered_presets_[selected].getFullPathName();
  }
  updatePresetSummary();
}

void SynthEditor::schedulePresetFilterUpdate(bool rebuildFilters) {
  preset_filter_rebuild_pending_ = preset_filter_rebuild_pending_ || rebuildFilters;
  if (preset_filter_update_pending_)
    return;

  preset_filter_update_pending_ = true;
  Component::SafePointer<SynthEditor> safe_this(this);
  MessageManager::callAsync([safe_this] {
    if (safe_this == nullptr)
      return;

    const bool rebuild = safe_this->preset_filter_rebuild_pending_;
    safe_this->preset_filter_update_pending_ = false;
    safe_this->preset_filter_rebuild_pending_ = false;
    if (!safe_this->preset_list_loaded_)
      return;

    if (rebuild)
      safe_this->populatePresetFilters();
    safe_this->filterPresetList();
  });
}

void SynthEditor::updatePresetSummary() {
  const String current = synth_.getPresetName().isEmpty() ? "Untitled" : synth_.getPresetName();
  if (preset_name_editor_.getText().trim().isEmpty() && current != "Untitled")
    preset_name_editor_.setText(current, false);

  if (!preset_list_loaded_) {
    const String summary = "Current preset: " + current +
                           ". Atlas resources path: " + LoadSave::getDataDirectory().getFullPathName() +
                           (preset_list_loading_ ?
                              ". Preset list loading in the background. Autoload preset when scrolling is " :
                              ". Preset list not loaded yet. Open the Preset menu or focus the preset browser to scan presets. Autoload preset when scrolling is ") +
                           (preset_preview_.getToggleState() ? "on." : "off.");
    preset_summary_.setText(summary, dontSendNotification);
    preset_summary_.setDescription(summary);
    if (auto* handler = preset_summary_.getAccessibilityHandler())
      handler->notifyAccessibilityEvent(AccessibilityEvent::titleChanged);
    return;
  }

  const String summary = "Current preset: " + current +
                         ". Atlas resources path: " + LoadSave::getDataDirectory().getFullPathName() +
                         ". " + String(all_presets_.size()) + " presets found, " +
                         String(filtered_presets_.size()) + " shown. Library: " +
                         (preset_library_.getText().isEmpty() ? kAllLibraries : preset_library_.getText()) +
                         ". Bank: " +
                         (preset_bank_.getText().isEmpty() ? kAllBanks : preset_bank_.getText()) +
                         ". Category: " +
                         (preset_category_.getText().isEmpty() ? kAllCategories : preset_category_.getText()) +
                         ". Autoload preset when scrolling is " +
                         (preset_preview_.getToggleState() ? "on." : "off.");
  preset_summary_.setText(summary, dontSendNotification);
  preset_summary_.setDescription(summary);
  if (auto* handler = preset_summary_.getAccessibilityHandler())
    handler->notifyAccessibilityEvent(AccessibilityEvent::titleChanged);
}

void SynthEditor::showPresetMenu() {
  ensurePresetListLoaded();

  PopupMenu menu;
  menu.addSectionHeader("Preset actions");
  menu.addItem(2, "Refresh preset list");
  menu.addSeparator();
  menu.addItem(3, "Open preset file...");
  menu.addItem(4, "Import bank...");
  menu.addItem(8, "Export folder as bank...");
  menu.addSeparator();
  menu.addItem(7, preset_preview_.getToggleState() ? "Turn autoload preset when scrolling off"
                                                   : "Turn autoload preset when scrolling on");
  menu.addItem(9, LoadSave::shouldScanDownloads() ? "Turn scan Downloads folder on startup off"
                                                  : "Turn scan Downloads folder on startup on");

  menu.showMenuAsync(PopupMenu::Options().withTargetComponent(preset_menu_),
                     [this](int result) {
    if (result == 2)
      refreshPresetList();
    else if (result == 3)
      choosePresetFile();
    else if (result == 4)
      chooseBankFile();
    else if (result == 8)
      chooseFolderToExportBank();
    else if (result == 7) {
      preset_preview_.setToggleState(!preset_preview_.getToggleState(), sendNotificationSync);
    }
    else if (result == 9)
      toggleScanDownloads();
  });
}

void SynthEditor::toggleScanDownloads() {
  const bool enabled = !LoadSave::shouldScanDownloads();
  LoadSave::saveScanDownloads(enabled);

  if (enabled) {
    const int imported = LoadSave::scanDownloadsForPresets();
    refreshPresetList(false);
    String message = "Scan Downloads folder for presets on startup on";
    if (imported > 0)
      message += ", " + String(imported) + " new presets imported";
    postPluginAnnouncement(message, AccessibilityHandler::AnnouncementPriority::high);
  }
  else {
    postPluginAnnouncement("Scan Downloads folder for presets on startup off",
                           AccessibilityHandler::AnnouncementPriority::high);
  }
}



void SynthEditor::selectPresetFile(const File& file) {
  if (!file.existsAsFile())
    return;
  last_preset_path = file.getFullPathName();
  if (!preset_list_loaded_) {
    updatePresetSummary();
    return;
  }

  const auto* item = presetItemForFile(file);
  const String library = item != nullptr ? item->library : presetLibraryName(file);
  if (preset_libraries_.contains(library)) {
    last_preset_library = library;
    populatePresetFilters();
  }
  const String bank = item != nullptr ? item->bank : presetBankName(file);
  if (preset_banks_.contains(bank)) {
    last_preset_bank = bank;
    populatePresetFilters();
  }
  const String category = item != nullptr ? item->category : presetCategoryName(file);
  if (preset_categories_.contains(category)) {
    last_preset_category = category;
    preset_category_.setSelectedItemIndex(preset_categories_.indexOf(category), dontSendNotification);
  }
  filterPresetList();
}


void SynthEditor::loadSelectedPreset() {
  ensurePresetListLoaded();

  const int index = preset_selector_.getSelectedItemIndex();
  if (!isPositiveAndBelow(index, filtered_presets_.size())) {
    postPluginAnnouncement("Choose a preset first", AccessibilityHandler::AnnouncementPriority::high);
    return;
  }
  loadPresetFile(filtered_presets_[index], false, false);
}

void SynthEditor::choosePresetFile() {
  preset_chooser_ = std::make_unique<FileChooser>("Open Atlas preset", LoadSave::getDataDirectory(),
                                                  vital::kPresetExtensionsList);
  preset_chooser_->launchAsync(FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles,
                               [this](const FileChooser& chooser) {
    const auto file = chooser.getResult();
    if (file.existsAsFile())
      loadPresetFile(file, false, true);
    preset_chooser_.reset();
  });
}

void SynthEditor::loadPresetFile(const File& file, bool preview, bool updateBrowserFilters) {
  const String parameter_id = focusedParameterId();
  const String accessible_title = focusedAccessibleTitle();
  auto* persistent_focus = persistentFocusedComponent();
  std::string error;
  if (!synth_.loadFromFile(file, error)) {
    NativeMessageBox::showMessageBoxAsync(MessageBoxIconType::WarningIcon, "Unable to load preset", error);
    postPluginAnnouncement("Unable to load preset", AccessibilityHandler::AnnouncementPriority::high);
    return;
  }
  last_preset_path = file.getFullPathName();
  synth_.updateHostDisplay();
  updateFullGui();
  if (updateBrowserFilters)
    selectPresetFile(file);
  else
    filterPresetList();
  updatePresetSummary();
  restoreFocusAfterRebuild(parameter_id, persistent_focus, accessible_title);
  if (preview || persistent_focus == &preset_selector_) {
    Component::SafePointer<SynthEditor> safe_this(this);
    Component::SafePointer<Component> preset_list_focus(&preset_selector_);
    MessageManager::callAsync([safe_this, preset_list_focus] {
      if (safe_this != nullptr && preset_list_focus != nullptr && preset_list_focus->isShowing()) {
        safe_this->ensureComponentVisible(preset_list_focus.getComponent());
        preset_list_focus->grabKeyboardFocus();
      }
    });
  }
  postPluginAnnouncement((preview ? "Autoloaded preset " : "Loaded preset ") +
                                           file.getFileNameWithoutExtension(),
                                         preview ? AccessibilityHandler::AnnouncementPriority::medium
                                                 : AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::chooseBankFile() {
  preset_chooser_ = std::make_unique<FileChooser>("Import Atlas bank", LoadSave::getDataDirectory(),
                                                  vital::kBankExtensionsList);
  preset_chooser_->launchAsync(FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles,
                               [this](const FileChooser& chooser) {
    const auto file = chooser.getResult();
    if (file.existsAsFile())
      importBankFile(file);
    preset_chooser_.reset();
  });
}

void SynthEditor::importBankFile(const File& file) {
  FileInputStream input_stream(file);
  if (!input_stream.openedOk()) {
    NativeMessageBox::showMessageBoxAsync(MessageBoxIconType::WarningIcon, "Unable to import bank",
                                          "The bank file could not be opened.");
    postPluginAnnouncement("Unable to open bank file", AccessibilityHandler::AnnouncementPriority::high);
    return;
  }

  File data_directory = LoadSave::getDataDirectory();
  data_directory.createDirectory();
  if (!LoadSave::hasDataDirectory())
    LoadSave::saveDataDirectory(data_directory);

  ZipFile import_zip(input_stream);
  const Result unzip_result = import_zip.uncompressTo(data_directory);
  if (unzip_result.wasOk()) {
    LoadSave::markPackInstalled(file.getFileNameWithoutExtension().toStdString());
    refreshPresetList();
    postPluginAnnouncement("Imported bank " + file.getFileNameWithoutExtension() +
                                             ". Preset list refreshed.",
                                           AccessibilityHandler::AnnouncementPriority::high);
    return;
  }

  LoadSave::writeErrorLog("Unzipping bank failed: " + unzip_result.getErrorMessage());
  NativeMessageBox::showMessageBoxAsync(MessageBoxIconType::WarningIcon, "Unable to import bank",
                                        unzip_result.getErrorMessage());
  postPluginAnnouncement("Unable to import bank", AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::chooseFolderToExportBank() {
  preset_chooser_ = std::make_unique<FileChooser>("Choose folder to export as an Atlas bank",
                                                  LoadSave::getUserPresetDirectory());
  preset_chooser_->launchAsync(FileBrowserComponent::openMode | FileBrowserComponent::canSelectDirectories,
                               [this](const FileChooser& chooser) {
    const File source_folder = chooser.getResult();
    preset_chooser_.reset();
    if (!source_folder.isDirectory())
      return;

    String bank_name = source_folder.getFileName().removeCharacters("\\/:*?\"<>|");
    if (bank_name.isEmpty())
      bank_name = "Atlas Bank";
    const File default_file = source_folder.getSiblingFile(bank_name).withFileExtension(vital::kBankExtension);
    bank_chooser_ = std::make_unique<FileChooser>("Export Atlas bank", default_file,
                                                  String("*.") + vital::kBankExtension);
    bank_chooser_->launchAsync(FileBrowserComponent::saveMode | FileBrowserComponent::canSelectFiles |
                                   FileBrowserComponent::warnAboutOverwriting,
                               [this, source_folder](const FileChooser& save_chooser) {
      const File destination = save_chooser.getResult();
      if (destination != File())
        exportFolderAsBank(source_folder, destination);
      bank_chooser_.reset();
    });
  });
}

void SynthEditor::exportFolderAsBank(const File& sourceFolder, const File& destinationFile) {
  if (!sourceFolder.isDirectory()) {
    NativeMessageBox::showMessageBoxAsync(MessageBoxIconType::WarningIcon, "Unable to export bank",
                                          "The selected source is not a folder.");
    return;
  }

  Array<File> files;
  sourceFolder.findChildFiles(files, File::findFiles, true,
                              vital::kPresetExtensionsList + ";*." + vital::kWavetableExtension +
                              ";*." + vital::kLegacyWavetableExtension + ";" + vital::kSampleExtensionsList +
                              ";" + vital::kLfoExtensionsList + ";*." + vital::kFxExtension);
  if (files.isEmpty()) {
    NativeMessageBox::showMessageBoxAsync(MessageBoxIconType::WarningIcon, "Unable to export bank",
                                          "No Atlas presets, tables, samples, LFOs, or FX presets were found.");
    postPluginAnnouncement("No bank content found", AccessibilityHandler::AnnouncementPriority::high);
    return;
  }

  const File destination = destinationFile.withFileExtension(vital::kBankExtension);
  String bank_name = destination.getFileNameWithoutExtension().removeCharacters("\\/:*?\"<>|");
  if (bank_name.isEmpty())
    bank_name = "Atlas Bank";

  ZipFile::Builder bank_zip;
  int added = 0;
  for (const File& file : files) {
    const String extension = file.getFileExtension().toLowerCase();
    String folder_name;
    if (extension == "." + String(vital::kPresetExtension) ||
        extension == "." + String(vital::kLegacyPresetExtension)) {
      folder_name = LoadSave::kPresetFolderName;
    }
    else if (extension == "." + String(vital::kWavetableExtension) ||
             extension == "." + String(vital::kLegacyWavetableExtension)) {
      folder_name = LoadSave::kWavetableFolderName;
    }
    else if (extension == "." + String(vital::kLfoExtension) ||
             extension == "." + String(vital::kLegacyLfoExtension)) {
      folder_name = LoadSave::kLfoFolderName;
    }
    else if (extension == "." + String(vital::kFxExtension)) {
      folder_name = LoadSave::kFxFolderName;
    }
    else if (extension == ".wav" || extension == ".flac" || extension == ".aif" || extension == ".aiff") {
      folder_name = pathContainsFolderName(file, sourceFolder, LoadSave::kWavetableFolderName) ?
                    LoadSave::kWavetableFolderName : LoadSave::kSampleFolderName;
    }
    else {
      continue;
    }

    const String relative_path = bankRelativePathForFile(file, sourceFolder, folder_name);
    bank_zip.addFile(file, 9, bank_name + "/" + folder_name + "/" + relative_path);
    ++added;
  }

  FileOutputStream output(destination);
  if (!output.openedOk() || !bank_zip.writeToStream(output, nullptr)) {
    NativeMessageBox::showMessageBoxAsync(MessageBoxIconType::WarningIcon, "Unable to export bank",
                                          "The bank file could not be written.");
    postPluginAnnouncement("Unable to export bank", AccessibilityHandler::AnnouncementPriority::high);
    return;
  }

  postPluginAnnouncement("Exported bank " + destination.getFileNameWithoutExtension() +
                                         " with " + String(added) + " files",
                                         AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::loadTuningFile() {
  tuning_chooser_ = std::make_unique<FileChooser>("Load Tuning", LoadSave::getDataDirectory(),
                                                  Tuning::allFileExtensions());
  tuning_chooser_->launchAsync(FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles,
                               [this](const FileChooser& chooser) {
    const File file = chooser.getResult();
    if (file.existsAsFile()) {
      synth_.loadTuningFile(file);
      postPluginAnnouncement("Loaded tuning " + String(synth_.getTuning()->getName()),
                             AccessibilityHandler::AnnouncementPriority::high);
    }
    tuning_chooser_.reset();
  });
}

void SynthEditor::clearTuning() {
  synth_.getTuning()->setDefaultTuning();
  postPluginAnnouncement("Tuning cleared", AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::exportPreset() {
  const File default_file = LoadSave::getUserPresetDirectory()
                                .getChildFile(synth_.getPresetName().isEmpty() ? String("Untitled")
                                                                               : synth_.getPresetName())
                                .withFileExtension(vital::kPresetExtension);
  export_chooser_ = std::make_unique<FileChooser>("Export Preset", default_file,
                                                  String("*.") + vital::kPresetExtension);
  export_chooser_->launchAsync(FileBrowserComponent::saveMode | FileBrowserComponent::canSelectFiles |
                                   FileBrowserComponent::warnAboutOverwriting,
                               [this](const FileChooser& chooser) {
    const File destination = chooser.getResult();
    if (destination != File()) {
      const File preset = destination.withFileExtension(vital::kPresetExtension);
      if (synth_.saveToFile(preset))
        postPluginAnnouncement("Exported preset " + preset.getFileNameWithoutExtension(),
                               AccessibilityHandler::AnnouncementPriority::high);
      else {
        NativeMessageBox::showMessageBoxAsync(MessageBoxIconType::WarningIcon, "Unable to export preset",
                                              "The preset could not be written to disk.");
        postPluginAnnouncement("Unable to export preset", AccessibilityHandler::AnnouncementPriority::high);
      }
    }
    export_chooser_.reset();
  });
}

void SynthEditor::savePresetToUserDirectory() {
  String preset_name = preset_name_editor_.getText().trim();
  if (preset_name.isEmpty())
    preset_name = synth_.getPresetName().isEmpty() ? "Untitled" : synth_.getPresetName();
  preset_name = preset_name.removeCharacters("\\/:*?\"<>|");
  if (preset_name.isEmpty())
    preset_name = "Untitled";

  savePresetFile(LoadSave::getUserPresetDirectory().getChildFile(preset_name));
}

void SynthEditor::savePresetAs() {
  showSavePresetDialog();
}

void SynthEditor::showSavePresetDialog() {
  String preset_name = preset_name_editor_.getText().trim();
  if (preset_name.isEmpty())
    preset_name = synth_.getPresetName().isEmpty() ? "Untitled" : synth_.getPresetName();
  save_patch_name_.setText(preset_name, false);
  save_patch_author_.setText(synth_.getAuthor().isNotEmpty() ? synth_.getAuthor() : String(LoadSave::getAuthor()), false);
  save_patch_bank_.setText(last_preset_bank != kAllBanks ? last_preset_bank : "User", false);
  save_patch_category_.setText(synth_.getStyle().isNotEmpty() ? synth_.getStyle() :
                               (last_preset_category != kAllCategories ? last_preset_category : ""), false);
  save_patch_tags_.setText(synth_.getTags(), false);
  save_patch_prompt_.setText("Save patch", dontSendNotification);
  save_patch_dialog_visible_ = true;
  save_patch_prompt_.setVisible(true);
  save_patch_name_.setVisible(true);
  save_patch_author_.setVisible(true);
  save_patch_bank_.setVisible(true);
  save_patch_category_.setVisible(true);
  save_patch_tags_.setVisible(true);
  save_patch_ok_.setVisible(true);
  save_patch_cancel_.setVisible(true);
  rebuildFocusOrder();
  resized();
  MessageManager::callAsync([this] {
    if (save_patch_name_.isShowing())
      save_patch_name_.grabKeyboardFocus();
  });
}

void SynthEditor::hideSavePresetDialog() {
  save_patch_dialog_visible_ = false;
  save_patch_prompt_.setVisible(false);
  save_patch_name_.setVisible(false);
  save_patch_author_.setVisible(false);
  save_patch_bank_.setVisible(false);
  save_patch_category_.setVisible(false);
  save_patch_tags_.setVisible(false);
  save_patch_ok_.setVisible(false);
  save_patch_cancel_.setVisible(false);
  rebuildFocusOrder();
  resized();
}

void SynthEditor::commitSavePresetDialog() {
  String preset_name = sanitizePresetPathPart(save_patch_name_.getText(), "Untitled");
  String bank = sanitizePresetPathPart(save_patch_bank_.getText(), "User");
  String category = sanitizePresetPathPart(save_patch_category_.getText(), "");
  String author = save_patch_author_.getText().trim().removeCharacters("\"");
  String tags = save_patch_tags_.getText().trim().removeCharacters("\"");

  if (category == ".")
    category = "";

  synth_.setPresetName(preset_name);
  synth_.setAuthor(author);
  synth_.setStyle(category);
  synth_.setTags(tags);
  preset_name_editor_.setText(preset_name, false);

  File destination = LoadSave::getUserPresetDirectory().getChildFile(bank);
  if (category.isNotEmpty())
    destination = destination.getChildFile(category);
  destination = destination.getChildFile(preset_name);

  hideSavePresetDialog();
  savePresetFile(destination);
}

void SynthEditor::savePresetFile(const File& file) {
  if (!synth_.saveToFile(file)) {
    NativeMessageBox::showMessageBoxAsync(MessageBoxIconType::WarningIcon, "Unable to save preset",
                                          "The preset could not be written to disk.");
    postPluginAnnouncement("Unable to save preset", AccessibilityHandler::AnnouncementPriority::high);
    return;
  }
  last_preset_path = synth_.getActiveFile().getFullPathName();
  refreshPresetList();
  selectPresetFile(synth_.getActiveFile());
  updatePresetSummary();
  postPluginAnnouncement("Saved preset " + file.withFileExtension(vital::kPresetExtension).getFileNameWithoutExtension(),
                                         AccessibilityHandler::AnnouncementPriority::high);
}

bool SynthEditor::isModulationDestinationId(const String& id) const {
  // Only parameters registered with the engine as modulation destinations can be connected.
  // Plain base controls (e.g. the envelope power sliders) have no destination, so offering to
  // modulate them would connect to a null destination and crash on the audio thread.
  return synth_.getEngine()->getMonoModulationDestination(id.toStdString()) != nullptr;
}

void SynthEditor::populateModulationDestinations() {
  modulation_destination_all_ids_.clear();
  modulation_destination_groups_.clear();
  modulation_destination_groups_.add("All destinations");

  for (const auto& destination : synth_.getEngine()->getMonoModulationDestinations()) {
    const String id = destination.first;
    if (!vital::Parameters::isParameter(destination.first) || id.startsWith("modulation_") ||
        isMacroBipolarParameterId(id))
      continue;

    modulation_destination_all_ids_.add(id);
    const String group = modulationDestinationGroupForId(id);
    if (group.isNotEmpty() && !modulation_destination_groups_.contains(group))
      modulation_destination_groups_.add(group);
  }

  std::vector<String> sorted_ids;
  for (const auto& id : modulation_destination_all_ids_)
    sorted_ids.push_back(id);
  std::stable_sort(sorted_ids.begin(), sorted_ids.end(), [](const String& a, const String& b) {
    const int rank_a = modulationDestinationSortRank(a);
    const int rank_b = modulationDestinationSortRank(b);
    if (rank_a != rank_b)
      return rank_a < rank_b;
    return modulationDestinationLabelForId(a).compareNatural(modulationDestinationLabelForId(b)) < 0;
  });
  modulation_destination_all_ids_.clear();
  for (const auto& id : sorted_ids)
    modulation_destination_all_ids_.add(id);

  modulation_destination_groups_.sort(true);
  modulation_destination_groups_.removeString("All destinations");
  modulation_destination_groups_.insert(0, "All destinations");

  ScopedValueSetter<bool> guard(updating_modulation_destinations_, true);
  modulation_destination_group_.clear(dontSendNotification);
  modulation_destination_group_.addItemList(modulation_destination_groups_, 1);
  modulation_destination_group_.setSelectedItemIndex(0, dontSendNotification);
  updateModulationDestinationList();
}

void SynthEditor::updateModulationDestinationList() {
  ScopedValueSetter<bool> guard(updating_modulation_destinations_, true);
  const String selected_group = modulation_destination_group_.getText().isEmpty()
      ? String("All destinations") : modulation_destination_group_.getText();
  const String previous_id = isPositiveAndBelow(modulation_destination_.getSelectedItemIndex(),
                                                modulation_destination_ids_.size())
      ? modulation_destination_ids_[modulation_destination_.getSelectedItemIndex()]
      : String();

  modulation_destination_ids_.clear();
  modulation_destination_.clear(dontSendNotification);
  const String selected_source = isPositiveAndBelow(modulation_source_.getSelectedItemIndex(),
                                                    modulation_source_ids_.size())
      ? modulation_source_ids_[modulation_source_.getSelectedItemIndex()]
      : String();
  for (const auto& id : modulation_destination_all_ids_) {
    const String group = modulationDestinationGroupForId(id);
    if (selected_group != "All destinations" && group != selected_group)
      continue;
    if (isInvalidModulationPair(selected_source, id))
      continue;

    modulation_destination_ids_.add(id);
    modulation_destination_.addItem(modulationDestinationLabelForId(id), modulation_destination_.getNumItems() + 1);
  }

  int selected = previous_id.isNotEmpty() ? modulation_destination_ids_.indexOf(previous_id) : 0;
  if (selected < 0)
    selected = 0;
  if (!modulation_destination_ids_.isEmpty())
    modulation_destination_.setSelectedItemIndex(selected, dontSendNotification);

  modulation_destination_.setDescription(selected_group + " destinations, " +
                                         String(modulation_destination_ids_.size()) + " choices");
  if (auto* handler = modulation_destination_.getAccessibilityHandler())
    handler->notifyAccessibilityEvent(AccessibilityEvent::titleChanged);
}

LineGenerator* SynthEditor::lfoGeneratorForIndex(int lfoIndex) const {
  if (!isPositiveAndBelow(lfoIndex, vital::kNumLfos))
    return nullptr;
  return synth_.getLfoSource(lfoIndex);
}

LineGenerator* SynthEditor::activeLfoGenerator() const {
  return lfoGeneratorForIndex(active_lfo_index_);
}

int SynthEditor::selectedLfoPointIndex() const {
  return lfo_mseg_point_.getSelectedItemIndex();
}

int SynthEditor::lfoPointIndexAtPhase(float phase) const {
  auto* generator = activeLfoGenerator();
  if (generator == nullptr)
    return -1;

  for (int i = 0; i < generator->getNumPoints(); ++i) {
    if (std::abs(generator->getPoint(i).first - phase) <= 0.0005f)
      return i;
  }
  return -1;
}

int SynthEditor::currentLfoPointIndex() const {
  auto* generator = activeLfoGenerator();
  if (generator == nullptr)
    return -1;

  const int selected = selectedLfoPointIndex();
  if (isPositiveAndBelow(selected, generator->getNumPoints()) &&
      std::abs(generator->getPoint(selected).first - lfo_cursor_phase_) <= 0.0005f)
    return selected;

  return lfoPointIndexAtPhase(lfo_cursor_phase_);
}

bool SynthEditor::isLfoPointSelected(float phase) const {
  return std::any_of(selected_lfo_point_phases_.begin(), selected_lfo_point_phases_.end(),
                     [phase](float selected) { return std::abs(selected - phase) <= 0.0005f; });
}

std::vector<int> SynthEditor::selectedLfoPointIndices() const {
  std::vector<int> indices;
  auto* generator = activeLfoGenerator();
  if (generator == nullptr)
    return indices;

  for (int i = 0; i < generator->getNumPoints(); ++i) {
    if (isLfoPointSelected(generator->getPoint(i).first))
      indices.push_back(i);
  }
  return indices;
}

void SynthEditor::pruneSelectedLfoPointPhases() {
  selected_lfo_point_phases_.erase(std::remove_if(selected_lfo_point_phases_.begin(),
                                                  selected_lfo_point_phases_.end(),
                                                  [this](float phase) {
                                                    return lfoPointIndexAtPhase(phase) < 0;
                                                  }),
                                   selected_lfo_point_phases_.end());
}

float SynthEditor::lfoCycleQuarterNotes() const {
  const auto& divisions = msegTimeDivisions();
  const int cycle = jlimit(0, static_cast<int>(divisions.size()) - 1, lfo_cycle_index_);
  return jmax(0.03125f, divisions[cycle].quarter_notes);
}

float SynthEditor::lfoGridQuarterNotes() const {
  const auto& divisions = msegTimeDivisions();
  const int grid = jlimit(0, static_cast<int>(divisions.size()) - 1, lfo_grid_index_);
  return jmax(0.03125f, divisions[grid].quarter_notes);
}

float SynthEditor::lfoGridAmount() const {
  return jlimit(0.001f, 1.0f, lfoGridQuarterNotes() / lfoCycleQuarterNotes());
}

float SynthEditor::lfoPhaseToQuarterNotes(float phase) const {
  return jmax(0.0f, phase) * lfoCycleQuarterNotes();
}

float SynthEditor::lfoQuarterNotesToPhase(float quarterNotes) const {
  return jlimit(0.0f, 1.0f, quarterNotes / lfoCycleQuarterNotes());
}

float SynthEditor::snapLfoPhaseToGrid(float phase) const {
  const float grid = lfoGridQuarterNotes();
  const float quarter_notes = lfoPhaseToQuarterNotes(jlimit(0.0f, 1.0f, phase));
  const float snapped = std::round(quarter_notes / grid) * grid;
  return lfoQuarterNotesToPhase(snapped);
}

String SynthEditor::lfoPointDescription(int pointIndex) const {
  return lfoPointDescriptionFor(active_lfo_index_, pointIndex);
}

String SynthEditor::lfoPointDescriptionFor(int lfoIndex, int pointIndex) const {
  auto* generator = lfoGeneratorForIndex(lfoIndex);
  if (generator == nullptr || !isPositiveAndBelow(pointIndex, generator->getNumPoints()))
    return {};

  const auto point = generator->getPoint(pointIndex);
  const float power = pointIndex < generator->getNumPoints() - 1
      ? generator->getPower(pointIndex)
      : generator->getPower(jmax(0, pointIndex - 1));
  return lfoTimeDescription(point.first) +
         ", " + percentString(1.0f - point.second) +
         ", " + curveNameForIndex(curveIndexForPower(power, generator->smooth()));
}

String SynthEditor::lfoTimeDescription(float phase) const {
  const float quarter_note = lfoPhaseToQuarterNotes(phase);
  const int bar = static_cast<int>(std::floor(quarter_note / 4.0f)) + 1;
  const float within_bar = quarter_note - static_cast<float>(bar - 1) * 4.0f;
  const int beat = static_cast<int>(std::floor(within_bar)) + 1;
  const int percent = jlimit(0, 99, roundToInt((within_bar - std::floor(within_bar)) * 100.0f));
  return "Bar " + String(bar) + " beat " + String(beat) + " " + String(percent) + "%";
}

String SynthEditor::lfoMsegStatusText() const {
  return lfoMsegStatusTextFor(active_lfo_index_);
}

String SynthEditor::lfoMsegStatusTextFor(int lfoIndex) const {
  auto* generator = lfoGeneratorForIndex(lfoIndex);
  if (generator == nullptr)
    return "LFO editor unavailable";
  const int selected = lfoIndex == active_lfo_index_ ? currentLfoPointIndex() : 0;
  if (isPositiveAndBelow(selected, generator->getNumPoints()))
    return lfoPointDescriptionFor(lfoIndex, selected) + ", " + String(msegTimeDivisions()[lfo_grid_index_].name);
  return lfoTimeDescription(lfo_cursor_phase_) + ", value " +
         percentString(generator->valueAtPhase(lfo_cursor_phase_)) + ", no point, grid " +
         String(msegTimeDivisions()[lfo_grid_index_].name);
}

void SynthEditor::applyLfoMode() {
  if (active_lfo_index_ < 0)
    return;

  const int mode = jlimit(0, 2, lfo_mseg_mode_.getSelectedItemIndex());
  const int vital_mode = mode == 0 ? vital::SynthLfo::kSync
                       : mode == 1 ? vital::SynthLfo::kEnvelope
                                   : vital::SynthLfo::kTrigger;
  setParameterEngineValue("lfo_" + String(active_lfo_index_ + 1) + "_sync_type",
                          static_cast<float>(vital_mode));
  postLfoAnnouncement("Mode " + lfo_mseg_mode_.getText() + ". Editing LFO " + String(active_lfo_index_ + 1));
}

void SynthEditor::applyLfoCycleLength() {
  if (active_lfo_index_ < 0)
    return;

  const auto& divisions = msegTimeDivisions();
  const int index = jlimit(0, static_cast<int>(divisions.size()) - 1, lfo_cycle_index_);
  const int tempo_index = closestVitalTempoIndex(divisions[index].quarter_notes);
  const String prefix = "lfo_" + String(active_lfo_index_ + 1) + "_";
  setParameterEngineValue(prefix + "sync", vital::SynthLfo::kTempo);
  setParameterEngineValue(prefix + "tempo", static_cast<float>(tempo_index));
  lfo_cursor_phase_ = snapLfoPhaseToGrid(lfo_cursor_phase_);
  postLfoAnnouncement("Cycle length " + String(divisions[index].name));
}

void SynthEditor::setLfoMsegControlsVisible(bool visible) {
  lfo_mseg_controls_visible_ = visible;
  lfo_mseg_summary_.setVisible(visible);
  lfo_mseg_lfo_.setVisible(visible);
  lfo_mseg_mode_.setVisible(visible);
  lfo_mseg_cycle_.setVisible(visible);
  lfo_mseg_grid_.setVisible(visible);
  lfo_mseg_shape_.setVisible(visible);
  lfo_mseg_apply_shape_.setVisible(visible);
  lfo_mseg_point_.setVisible(visible);
  lfo_mseg_add_point_.setVisible(visible);
  lfo_mseg_delete_point_.setVisible(visible);
  lfo_mseg_time_down_.setVisible(visible);
  lfo_mseg_time_up_.setVisible(visible);
  lfo_mseg_value_down_.setVisible(visible);
  lfo_mseg_value_up_.setVisible(visible);
  lfo_mseg_curve_.setVisible(visible);
  lfo_mseg_smooth_.setVisible(visible);
  lfo_mseg_keyboard_.setVisible(visible);
  if (visible)
    refreshLfoMsegControls();
  resized();
}

void SynthEditor::refreshLfoMsegControls() {
  ScopedValueSetter<bool> guard(updating_lfo_mseg_controls_, true);
  active_lfo_index_ = jlimit(0, vital::kNumLfos - 1, active_lfo_index_);
  lfo_mseg_lfo_.setSelectedItemIndex(active_lfo_index_, dontSendNotification);
  if (auto* mode_bridge = parameterBridge("lfo_" + String(active_lfo_index_ + 1) + "_sync_type")) {
    const int vital_mode = roundToInt(mode_bridge->convertToEngineValue(mode_bridge->getValue()));
    const int editor_mode = vital_mode == vital::SynthLfo::kEnvelope ? 1
                          : vital_mode == vital::SynthLfo::kTrigger ? 2 : 0;
    lfo_mseg_mode_.setSelectedItemIndex(editor_mode, dontSendNotification);
  }
  lfo_mseg_cycle_.setSelectedItemIndex(jlimit(0, static_cast<int>(msegTimeDivisions().size()) - 1, lfo_cycle_index_),
                                       dontSendNotification);
  lfo_mseg_grid_.setSelectedItemIndex(jlimit(0, static_cast<int>(msegTimeDivisions().size()) - 1, lfo_grid_index_),
                                      dontSendNotification);
  if (auto* generator = activeLfoGenerator()) {
    lfo_mseg_smooth_.setToggleState(generator->smooth(), dontSendNotification);
    pruneSelectedLfoPointPhases();
    updateLfoPointSelector();
  }
  updateLfoMsegSummary();
}

void SynthEditor::updateLfoPointSelector() {
  auto* generator = activeLfoGenerator();
  const int previous = selectedLfoPointIndex();
  lfo_mseg_point_.clear(dontSendNotification);
  if (generator == nullptr)
    return;

  for (int i = 0; i < generator->getNumPoints(); ++i)
    lfo_mseg_point_.addItem(lfoPointDescription(i), i + 1);

  if (generator->getNumPoints() > 0)
    lfo_mseg_point_.setSelectedItemIndex(jlimit(0, generator->getNumPoints() - 1, previous), dontSendNotification);
}

void SynthEditor::updateLfoMsegSummary() {
  auto* generator = activeLfoGenerator();
  String summary = "Accessible MSEG editor unavailable";
  if (generator) {
    const int point_index = currentLfoPointIndex();
    summary = "Editing LFO " + String(active_lfo_index_ + 1) +
              ", " + String(generator->getNumPoints()) + " points";
    if (isPositiveAndBelow(point_index, generator->getNumPoints()))
      summary += ". " + lfoPointDescription(point_index);
    summary += generator->smooth() ? ". Smooth curves on." : ". Smooth curves off.";

    ScopedValueSetter<bool> guard(updating_lfo_mseg_controls_, true);
    const int curve_point = isPositiveAndBelow(point_index, generator->getNumPoints() - 1)
        ? point_index : jmax(0, point_index - 1);
    lfo_mseg_curve_.setSelectedItemIndex(curveIndexForPower(generator->getPower(curve_point), generator->smooth()),
                                         dontSendNotification);
    lfo_mseg_smooth_.setToggleState(generator->smooth(), dontSendNotification);
  }

  lfo_mseg_summary_.setText(summary, dontSendNotification);
  lfo_mseg_summary_.setDescription(summary);
  if (auto* handler = lfo_mseg_summary_.getAccessibilityHandler())
    handler->notifyAccessibilityEvent(AccessibilityEvent::titleChanged);
  lfo_mseg_keyboard_.repaint();
}

void SynthEditor::moveLfoCursor(float delta) {
  lfo_cursor_phase_ = snapLfoPhaseToGrid(jlimit(0.0f, 1.0f, lfo_cursor_phase_ + delta));
  auto* generator = activeLfoGenerator();
  if (generator) {
    const int found = lfoPointIndexAtPhase(lfo_cursor_phase_);
    if (found >= 0)
      lfo_mseg_point_.setSelectedItemIndex(found, dontSendNotification);
  }
  updateLfoMsegSummary();
  postLfoAnnouncement(lfoMsegStatusText());
}

void SynthEditor::moveToLfoPoint(int direction) {
  auto* generator = activeLfoGenerator();
  if (generator == nullptr || generator->getNumPoints() == 0) {
    postLfoAnnouncement("No points");
    return;
  }

  const float tolerance = 0.0005f;
  int next = -1;
  const int current = currentLfoPointIndex();

  if (current >= 0) {
    next = jlimit(0, generator->getNumPoints() - 1, current + direction);
  }
  else if (direction > 0) {
    for (int i = 0; i < generator->getNumPoints(); ++i) {
      if (generator->getPoint(i).first > lfo_cursor_phase_ + tolerance) {
        next = i;
        break;
      }
    }
    if (next < 0)
      next = generator->getNumPoints() - 1;
  }
  else {
    for (int i = generator->getNumPoints() - 1; i >= 0; --i) {
      if (generator->getPoint(i).first < lfo_cursor_phase_ - tolerance) {
        next = i;
        break;
      }
    }
    if (next < 0)
      next = 0;
  }

  lfo_cursor_phase_ = generator->getPoint(next).first;
  if (!lfo_multi_selection_mode_) {
    selected_lfo_point_phases_.clear();
    selected_lfo_point_phases_.push_back(lfo_cursor_phase_);
  }
  lfo_mseg_point_.setSelectedItemIndex(next, sendNotificationSync);
  postLfoAnnouncement(lfoMsegStatusText());
}

void SynthEditor::stepLfoShapePreset(int direction) {
  const int count = jmax(1, lfo_mseg_shape_.getNumItems());
  const int next = jlimit(0, count - 1, lfo_mseg_shape_.getSelectedItemIndex() + direction);
  lfo_mseg_shape_.setSelectedItemIndex(next, dontSendNotification);
  postLfoAnnouncement("Shape preset " + lfo_mseg_shape_.getText());
}

void SynthEditor::stepLfoCycleLength(int direction) {
  const int count = static_cast<int>(msegTimeDivisions().size());
  lfo_cycle_index_ = jlimit(0, count - 1, lfo_cycle_index_ + direction);
  lfo_mseg_cycle_.setSelectedItemIndex(lfo_cycle_index_, dontSendNotification);
  applyLfoCycleLength();
  updateLfoMsegSummary();
}

void SynthEditor::stepLfoGrid(int direction) {
  const int count = static_cast<int>(msegTimeDivisions().size());
  lfo_grid_index_ = jlimit(0, count - 1, lfo_grid_index_ + direction);
  lfo_mseg_grid_.setSelectedItemIndex(lfo_grid_index_, dontSendNotification);
  updateLfoMsegSummary();
  postLfoAnnouncement("Grid " + lfo_mseg_grid_.getText() + ". " + lfoMsegStatusText());
}

void SynthEditor::switchLfoFromShortcut(int lfoIndex) {
  active_lfo_index_ = jlimit(0, vital::kNumLfos - 1, lfoIndex);
  lfo_cursor_phase_ = 0.0f;
  selected_lfo_point_phases_.clear();
  refreshLfoMsegControls();
  postLfoAnnouncement("Editing LFO " + String(active_lfo_index_ + 1));
}

void SynthEditor::clearLfoShape() {
  auto* generator = activeLfoGenerator();
  if (generator == nullptr)
    return;
  synth_.pauseProcessing(true);
  generator->setNumPoints(2);
  generator->setPoint(0, { 0.0f, 0.5f });
  generator->setPoint(1, { 1.0f, 0.5f });
  generator->setPower(0, 0.0f);
  generator->setPower(1, 0.0f);
  generator->setSmooth(false);
  generator->render();
  synth_.pauseProcessing(false);
  lfo_cursor_phase_ = 0.0f;
  selected_lfo_point_phases_.clear();
  refreshLfoMsegControls();
  postLfoAnnouncement("Cleared LFO " + String(active_lfo_index_ + 1));
}

bool SynthEditor::handleLfoMsegShortcut(const KeyPress& key) {
  const juce_wchar character = CharacterFunctions::toLowerCase(key.getTextCharacter());
  const bool shift = key.getModifiers().isShiftDown();
  const bool command = key.getModifiers().isCommandDown();

  const int lfo_index = shiftedDigitIndex(key);
  if (lfo_index >= 0 && lfo_index < vital::kNumLfos) {
    switchLfoFromShortcut(lfo_index);
    return true;
  }

  if (command && (key == KeyPress::upKey || key == KeyPress::downKey)) {
    lfo_mseg_curve_.setSelectedItemIndex((lfo_mseg_curve_.getSelectedItemIndex() + (key == KeyPress::upKey ? 1 : -1) +
                                          lfo_mseg_curve_.getNumItems()) % jmax(1, lfo_mseg_curve_.getNumItems()),
                                         sendNotificationSync);
    return true;
  }
  if (shift && (key == KeyPress::leftKey || key == KeyPress::rightKey)) {
    moveLfoPointTime(key == KeyPress::rightKey ? lfoGridAmount() : -lfoGridAmount());
    return true;
  }
  if (key == KeyPress::leftKey || key == KeyPress::rightKey) {
    moveLfoCursor(key == KeyPress::rightKey ? lfoGridAmount() : -lfoGridAmount());
    return true;
  }
  if (key == KeyPress::upKey || key == KeyPress::downKey) {
    moveLfoPointValue(key == KeyPress::upKey ? (shift ? 0.05f : 0.01f) : (shift ? -0.05f : -0.01f));
    return true;
  }
  if (key.getKeyCode() == KeyPress::backspaceKey) {
    clearLfoShape();
    return true;
  }
  if (shift && character == 'c') {
    copyLfoShape();
    return true;
  }
  if (shift && character == 'v') {
    pasteLfoShape();
    return true;
  }
  if (shift && character == 'd') {
    duplicateLfoShapeToNextSlot();
    return true;
  }
  if (shift && character == 'a') {
    applyLfoShapePreset();
    return true;
  }
  if (character == 'a') {
    addLfoPoint();
    return true;
  }
  if (shift && character == 's') {
    toggleLfoMultiSelectionMode();
    return true;
  }
  if (character == 's') {
    toggleLfoPointSelection();
    return true;
  }
  if (character == 'r') {
    clearLfoPointSelection();
    return true;
  }
  if (character == 'e' || key.getKeyCode() == KeyPress::deleteKey) {
    deleteLfoPoint();
    return true;
  }
  if (character == '[' || character == '{') {
    moveLfoPointValue(shift ? -0.05f : -0.01f);
    return true;
  }
  if (character == ']' || character == '}') {
    moveLfoPointValue(shift ? 0.05f : 0.01f);
    return true;
  }
  if (character == 'i' || character == 'k') {
    const int direction = character == 'i' ? 1 : -1;
    lfo_mseg_curve_.setSelectedItemIndex((lfo_mseg_curve_.getSelectedItemIndex() + direction +
                                          lfo_mseg_curve_.getNumItems()) % jmax(1, lfo_mseg_curve_.getNumItems()),
                                         sendNotificationSync);
    return true;
  }
  if (character == 'j' || character == 'l') {
    moveLfoPointTime(character == 'l' ? lfoGridAmount() : -lfoGridAmount());
    return true;
  }
  if (character == ',') {
    moveToLfoPoint(-1);
    return true;
  }
  if (character == '.') {
    moveToLfoPoint(1);
    return true;
  }
  if (character == 'g' || character == 'h') {
    stepLfoShapePreset(character == 'h' ? 1 : -1);
    return true;
  }
  if (character == ';' || character == '\'') {
    stepLfoCycleLength(character == '\'' ? 1 : -1);
    return true;
  }
  if (character == '-' || character == '_') {
    stepLfoGrid(-1);
    return true;
  }
  if (character == '=' || character == '+') {
    stepLfoGrid(1);
    return true;
  }
  if (character == 'c') {
    copyLfoPoints();
    return true;
  }
  if (character == 'v') {
    pasteLfoPoints();
    return true;
  }
  return false;
}

void SynthEditor::toggleLfoPointSelection() {
  auto* generator = activeLfoGenerator();
  const int index = lfoPointIndexAtPhase(lfo_cursor_phase_);
  if (generator == nullptr || index < 0) {
    postLfoAnnouncement("No point at " + lfoTimeDescription(lfo_cursor_phase_));
    return;
  }

  const float phase = generator->getPoint(index).first;
  const auto existing = std::find_if(selected_lfo_point_phases_.begin(), selected_lfo_point_phases_.end(),
                                     [phase](float selected) { return std::abs(selected - phase) <= 0.0005f; });
  if (existing != selected_lfo_point_phases_.end()) {
    selected_lfo_point_phases_.erase(existing);
    postLfoAnnouncement("Removed point from selection, " + lfoPointDescription(index));
  }
  else {
    if (!lfo_multi_selection_mode_)
      selected_lfo_point_phases_.clear();
    selected_lfo_point_phases_.push_back(phase);
    postLfoAnnouncement("Added point to selection, " + lfoPointDescription(index));
  }
  updateLfoMsegSummary();
}

void SynthEditor::toggleLfoMultiSelectionMode() {
  lfo_multi_selection_mode_ = !lfo_multi_selection_mode_;
  if (!lfo_multi_selection_mode_) {
    selected_lfo_point_phases_.clear();
    const int index = lfoPointIndexAtPhase(lfo_cursor_phase_);
    if (auto* generator = activeLfoGenerator())
      if (isPositiveAndBelow(index, generator->getNumPoints()))
        selected_lfo_point_phases_.push_back(generator->getPoint(index).first);
  }
  postLfoAnnouncement(lfo_multi_selection_mode_ ? "Multi selection on" : "Multi selection off");
  updateLfoMsegSummary();
}

void SynthEditor::clearLfoPointSelection() {
  selected_lfo_point_phases_.clear();
  postLfoAnnouncement("Point selection cleared");
  updateLfoMsegSummary();
}

void SynthEditor::copyLfoPoints() {
  auto* generator = activeLfoGenerator();
  if (generator == nullptr)
    return;

  auto indices = lfo_multi_selection_mode_ ? selectedLfoPointIndices() : std::vector<int>();
  const int cursor_index = lfoPointIndexAtPhase(lfo_cursor_phase_);
  if (indices.empty() && cursor_index >= 0)
    indices.push_back(cursor_index);
  if (indices.empty()) {
    postLfoAnnouncement("No points selected");
    return;
  }

  std::sort(indices.begin(), indices.end());
  copied_lfo_points_.clear();
  for (int index : indices) {
    copied_lfo_points_.push_back({ generator->getPoint(index), generator->getPower(index) });
  }
  postLfoAnnouncement("Copied " + String(static_cast<int>(copied_lfo_points_.size())) + " point" +
                      (copied_lfo_points_.size() == 1 ? "" : "s"));
}

void SynthEditor::pasteLfoPoints() {
  auto* generator = activeLfoGenerator();
  if (generator == nullptr)
    return;
  if (copied_lfo_points_.empty()) {
    postLfoAnnouncement("No points copied");
    return;
  }

  std::vector<CopiedLfoPoint> source = copied_lfo_points_;
  std::sort(source.begin(), source.end(), [](const auto& a, const auto& b) {
    return a.point.first < b.point.first;
  });

  const float source_start = source.front().point.first;
  const float source_end = source.back().point.first;
  const float span = jmax(0.0f, source_end - source_start);
  const float paste_start = span >= 1.0f ? 0.0f : jlimit(0.0f, 1.0f - span, lfo_cursor_phase_);

  std::vector<CopiedLfoPoint> points;
  points.reserve(generator->getNumPoints() + static_cast<int>(source.size()));
  for (int i = 0; i < generator->getNumPoints(); ++i)
    points.push_back({ generator->getPoint(i), generator->getPower(i) });

  selected_lfo_point_phases_.clear();
  for (auto point : source) {
    point.point.first = jlimit(0.0f, 1.0f, paste_start + (point.point.first - source_start));
    points.erase(std::remove_if(points.begin(), points.end(), [&point](const auto& existing) {
      return std::abs(existing.point.first - point.point.first) <= 0.0005f;
    }), points.end());
    points.push_back(point);
    selected_lfo_point_phases_.push_back(point.point.first);
  }

  std::sort(points.begin(), points.end(), [](const auto& a, const auto& b) {
    return a.point.first < b.point.first;
  });
  const int count = jmin(static_cast<int>(points.size()), LineGenerator::kMaxPoints);

  synth_.pauseProcessing(true);
  generator->setNumPoints(count);
  for (int i = 0; i < count; ++i) {
    generator->setPoint(i, points[static_cast<size_t>(i)].point);
    generator->setPower(i, points[static_cast<size_t>(i)].power);
  }
  generator->render();
  synth_.pauseProcessing(false);

  lfo_cursor_phase_ = paste_start;
  refreshLfoMsegControls();
  postLfoAnnouncement("Pasted " + String(static_cast<int>(source.size())) + " point" +
                      (source.size() == 1 ? "" : "s") + " at " + lfoTimeDescription(paste_start));
}

void SynthEditor::copyLfoShape() {
  auto* generator = activeLfoGenerator();
  if (generator == nullptr)
    return;

  copied_lfo_shape_.clear();
  for (int i = 0; i < generator->getNumPoints(); ++i)
    copied_lfo_shape_.push_back({ generator->getPoint(i), generator->getPower(i) });
  postLfoAnnouncement("Copied LFO " + String(active_lfo_index_ + 1));
}

void SynthEditor::pasteLfoShape() {
  auto* generator = activeLfoGenerator();
  if (generator == nullptr)
    return;
  if (copied_lfo_shape_.empty()) {
    postLfoAnnouncement("No pattern copied");
    return;
  }

  const int count = jmin(static_cast<int>(copied_lfo_shape_.size()), LineGenerator::kMaxPoints);
  synth_.pauseProcessing(true);
  generator->setNumPoints(count);
  for (int i = 0; i < count; ++i) {
    generator->setPoint(i, copied_lfo_shape_[static_cast<size_t>(i)].point);
    generator->setPower(i, copied_lfo_shape_[static_cast<size_t>(i)].power);
  }
  generator->render();
  synth_.pauseProcessing(false);

  lfo_cursor_phase_ = 0.0f;
  selected_lfo_point_phases_.clear();
  refreshLfoMsegControls();
  postLfoAnnouncement("Pasted to LFO " + String(active_lfo_index_ + 1));
}

void SynthEditor::duplicateLfoShapeToNextSlot() {
  auto* source = activeLfoGenerator();
  if (source == nullptr)
    return;

  std::vector<CopiedLfoPoint> duplicate;
  for (int i = 0; i < source->getNumPoints(); ++i)
    duplicate.push_back({ source->getPoint(i), source->getPower(i) });

  const int destination = (active_lfo_index_ + 1) % vital::kNumLfos;
  active_lfo_index_ = destination;
  if (auto* target = activeLfoGenerator()) {
    const int count = jmin(static_cast<int>(duplicate.size()), LineGenerator::kMaxPoints);
    synth_.pauseProcessing(true);
    target->setNumPoints(count);
    for (int i = 0; i < count; ++i) {
      target->setPoint(i, duplicate[static_cast<size_t>(i)].point);
      target->setPower(i, duplicate[static_cast<size_t>(i)].power);
    }
    target->render();
    synth_.pauseProcessing(false);
  }
  lfo_cursor_phase_ = 0.0f;
  selected_lfo_point_phases_.clear();
  refreshLfoMsegControls();
  postLfoAnnouncement("Duplicated to LFO " + String(destination + 1));
}

void SynthEditor::applyLfoShapePreset() {
  auto* generator = activeLfoGenerator();
  if (generator == nullptr)
    return;

  synth_.pauseProcessing(true);
  const int shape = lfo_mseg_shape_.getSelectedItemIndex();
  if (shape == 0) {
    synth_.pauseProcessing(false);
    postLfoAnnouncement("Custom shape selected. No change applied.");
    return;
  }
  if (shape == 1)
    generator->initSin();
  else if (shape == 2)
    generator->initTriangle();
  else if (shape == 3)
    generator->initSquare();
  else if (shape == 4)
    generator->initSawUp();
  else if (shape == 5)
    generator->initSawDown();
  else {
    auto setPoints = [generator](std::initializer_list<std::pair<float, float>> points) {
      generator->setNumPoints(static_cast<int>(points.size()));
      int index = 0;
      for (const auto& point : points) {
        generator->setPoint(index, { point.first, 1.0f - point.second });
        generator->setPower(index, 0.0f);
        index++;
      }
      generator->setSmooth(false);
      generator->render();
    };

    if (shape == 6) {
      setPoints({ { 0.0f, 1.0f }, { 0.25f, 0.0f }, { 1.0f, 1.0f } });
    }
    else if (shape == 7) {
      setPoints({ { 0.0f, 0.0f }, { 0.25f, 0.3f }, { 0.5f, 0.7f }, { 0.75f, 1.0f }, { 1.0f, 0.0f } });
    }
    else if (shape == 8) {
      setPoints({ { 0.0f, 0.0f }, { 0.125f, 0.125f }, { 0.25f, 0.25f }, { 0.375f, 0.375f },
                  { 0.5f, 0.5f }, { 0.625f, 0.625f }, { 0.75f, 0.75f }, { 0.875f, 0.875f }, { 1.0f, 1.0f } });
    }
    else if (shape == 9) {
      setPoints({ { 0.0f, 1.0f }, { 0.125f, 0.875f }, { 0.25f, 0.75f }, { 0.375f, 0.625f },
                  { 0.5f, 0.5f }, { 0.625f, 0.375f }, { 0.75f, 0.25f }, { 0.875f, 0.125f }, { 1.0f, 0.0f } });
    }
    else if (shape == 10) {
      setPoints({ { 0.0f, 0.0f }, { 1.0f, 0.0f } });
    }
    else if (shape == 11) {
      setPoints({ { 0.0f, 0.5f }, { 1.0f, 0.5f } });
    }
    else {
      setPoints({ { 0.0f, 1.0f }, { 1.0f, 1.0f } });
    }
  }
  synth_.pauseProcessing(false);

  refreshLfoMsegControls();
  postLfoAnnouncement("Applied shape " + lfo_mseg_shape_.getText());
}

void SynthEditor::addLfoPoint() {
  auto* generator = activeLfoGenerator();
  if (generator == nullptr || generator->getNumPoints() >= LineGenerator::kMaxPoints)
    return;

  lfo_cursor_phase_ = snapLfoPhaseToGrid(lfo_cursor_phase_);
  int insert_index = generator->getNumPoints();
  for (int i = 0; i < generator->getNumPoints(); ++i) {
    const float point_time = generator->getPoint(i).first;
    if (std::abs(point_time - lfo_cursor_phase_) <= 0.0005f) {
      lfo_mseg_point_.setSelectedItemIndex(i, sendNotificationSync);
      postLfoAnnouncement("Point already exists, " + lfoMsegStatusText());
      return;
    }
    if (point_time > lfo_cursor_phase_) {
      insert_index = i;
      break;
    }
  }

  const float output_value = generator->valueAtPhase(lfo_cursor_phase_);
  synth_.pauseProcessing(true);
  generator->addPoint(insert_index, { lfo_cursor_phase_, 1.0f - output_value });
  generator->render();
  synth_.pauseProcessing(false);
  refreshLfoMsegControls();
  lfo_mseg_point_.setSelectedItemIndex(insert_index, dontSendNotification);
  selected_lfo_point_phases_.clear();
  selected_lfo_point_phases_.push_back(lfo_cursor_phase_);
  updateLfoMsegSummary();
  postLfoAnnouncement("Added point, " + lfoPointDescription(insert_index));
}

void SynthEditor::deleteLfoPoint() {
  auto* generator = activeLfoGenerator();
  auto indices = lfo_multi_selection_mode_ ? selectedLfoPointIndices() : std::vector<int>();
  const int cursor_index = lfoPointIndexAtPhase(lfo_cursor_phase_);
  if (indices.empty() && cursor_index >= 0)
    indices.push_back(cursor_index);
  if (generator == nullptr || indices.empty()) {
    postLfoAnnouncement("No point at " + lfoTimeDescription(lfo_cursor_phase_));
    return;
  }
  if (generator->getNumPoints() <= 1) {
    postLfoAnnouncement("No point at " + lfoTimeDescription(lfo_cursor_phase_));
    return;
  }

  std::sort(indices.begin(), indices.end(), std::greater<int>());
  const String deleted = indices.size() == 1 ? lfoPointDescription(indices.front()) : String();
  synth_.pauseProcessing(true);
  for (int index : indices) {
    if (generator->getNumPoints() <= 1)
      break;
    generator->removePoint(index);
  }
  generator->render();
  synth_.pauseProcessing(false);
  selected_lfo_point_phases_.clear();
  refreshLfoMsegControls();
  if (indices.size() == 1)
    postLfoAnnouncement("Deleted point, " + deleted);
  else
    postLfoAnnouncement("Deleted " + String(static_cast<int>(indices.size())) + " selected points");
}

void SynthEditor::moveLfoPointTime(float delta) {
  auto* generator = activeLfoGenerator();
  auto indices = lfo_multi_selection_mode_ ? selectedLfoPointIndices() : std::vector<int>();
  const int cursor_index = lfoPointIndexAtPhase(lfo_cursor_phase_);
  if (indices.empty() && cursor_index >= 0)
    indices.push_back(cursor_index);
  if (generator == nullptr || indices.empty()) {
    postLfoAnnouncement("No point selected. Press A to add a point at " + lfoTimeDescription(lfo_cursor_phase_));
    return;
  }

  std::vector<CopiedLfoPoint> points;
  for (int i = 0; i < generator->getNumPoints(); ++i)
    points.push_back({ generator->getPoint(i), generator->getPower(i) });

  selected_lfo_point_phases_.clear();
  for (int index : indices) {
    points[static_cast<size_t>(index)].point.first =
        snapLfoPhaseToGrid(jlimit(0.0f, 1.0f, points[static_cast<size_t>(index)].point.first + delta));
    selected_lfo_point_phases_.push_back(points[static_cast<size_t>(index)].point.first);
  }
  std::sort(points.begin(), points.end(), [](const auto& a, const auto& b) {
    return a.point.first < b.point.first;
  });
  synth_.pauseProcessing(true);
  for (int i = 0; i < static_cast<int>(points.size()); ++i) {
    generator->setPoint(i, points[static_cast<size_t>(i)].point);
    generator->setPower(i, points[static_cast<size_t>(i)].power);
  }
  generator->render();
  synth_.pauseProcessing(false);
  lfo_cursor_phase_ = selected_lfo_point_phases_.empty() ? lfo_cursor_phase_ : selected_lfo_point_phases_.front();
  refreshLfoMsegControls();
  const int moved_index = lfoPointIndexAtPhase(lfo_cursor_phase_);
  if (indices.size() == 1 && moved_index >= 0)
    postLfoAnnouncement("Moved point, " + lfoPointDescription(moved_index));
  else
    postLfoAnnouncement("Moved " + String(static_cast<int>(indices.size())) + " selected points");
}

void SynthEditor::moveLfoPointValue(float delta) {
  auto* generator = activeLfoGenerator();
  auto indices = lfo_multi_selection_mode_ ? selectedLfoPointIndices() : std::vector<int>();
  const int cursor_index = lfoPointIndexAtPhase(lfo_cursor_phase_);
  if (indices.empty() && cursor_index >= 0)
    indices.push_back(cursor_index);
  if (generator == nullptr || indices.empty()) {
    postLfoAnnouncement("No point selected. Press A to add a point at " + lfoTimeDescription(lfo_cursor_phase_));
    return;
  }

  float output_value = 0.0f;
  synth_.pauseProcessing(true);
  for (int index : indices) {
    const auto point = generator->getPoint(index);
    output_value = jlimit(0.0f, 1.0f, pointOutputValue(generator, index) + delta);
    generator->setPoint(index, { point.first, 1.0f - output_value });
  }
  generator->render();
  synth_.pauseProcessing(false);
  refreshLfoMsegControls();
  if (indices.size() == 1)
    postLfoAnnouncement("Value " + percentString(output_value) + ", " + lfoPointDescription(indices.front()));
  else
    postLfoAnnouncement("Changed " + String(static_cast<int>(indices.size())) +
                        " selected points by " + percentString(std::abs(delta)));
}

void SynthEditor::setLfoPointCurveFromCombo() {
  auto* generator = activeLfoGenerator();
  auto indices = lfo_multi_selection_mode_ ? selectedLfoPointIndices() : std::vector<int>();
  const int point_index = lfoPointIndexAtPhase(lfo_cursor_phase_);
  if (indices.empty() && point_index >= 0)
    indices.push_back(point_index);
  if (generator == nullptr || generator->getNumPoints() < 2)
    return;
  if (indices.empty()) {
    postLfoAnnouncement("No point selected. Press A to add a point at " + lfoTimeDescription(lfo_cursor_phase_));
    return;
  }
  const int curve_index = lfo_mseg_curve_.getSelectedItemIndex();
  synth_.pauseProcessing(true);
  generator->setSmooth(smoothForCurveIndex(curve_index));
  for (int index : indices) {
    const int curve_point = isPositiveAndBelow(index, generator->getNumPoints() - 1)
        ? index : generator->getNumPoints() - 2;
    generator->setPower(curve_point, powerForCurveIndex(curve_index));
  }
  generator->render();
  synth_.pauseProcessing(false);
  updateLfoMsegSummary();
  if (indices.size() == 1)
    postLfoAnnouncement("Curve " + lfo_mseg_curve_.getText() + ", " + lfoPointDescription(indices.front()));
  else
    postLfoAnnouncement("Changed curve for " + String(static_cast<int>(indices.size())) + " selected points");
}

void SynthEditor::setLfoSmoothFromToggle() {
  if (auto* generator = activeLfoGenerator()) {
    synth_.pauseProcessing(true);
    generator->setSmooth(lfo_mseg_smooth_.getToggleState());
    synth_.pauseProcessing(false);
    updateLfoMsegSummary();
    postPluginAnnouncement(generator->smooth() ? "Smooth MSEG on" : "Smooth MSEG off",
                                           AccessibilityHandler::AnnouncementPriority::medium);
  }
}

void SynthEditor::setModulationControlsVisible(bool visible) {
  modulation_controls_visible_ = visible;
  modulation_source_.setVisible(visible);
  modulation_destination_group_.setVisible(visible);
  modulation_destination_.setVisible(visible);
  add_modulation_.setVisible(visible);
  remove_modulation_.setVisible(visible);
  modulation_list_.setVisible(visible);
  resized();
}

void SynthEditor::setRoutingControlsVisible(bool visible) {
  routing_controls_visible_ = visible;
  if (!show_all_sections_) {
    addChildComponent(routing_summary_);
    addChildComponent(routing_mode_);
    addChildComponent(routing_default_);
    addChildComponent(routing_serial_forward_);
    addChildComponent(routing_serial_backward_);
  }
  routing_summary_.setVisible(visible);
  routing_mode_.setVisible(visible);
  routing_default_.setVisible(visible);
  routing_serial_forward_.setVisible(visible);
  routing_serial_backward_.setVisible(visible);
  if (visible)
    refreshRoutingControls();
  resized();
}

void SynthEditor::setEffectChainControlsVisible(bool visible) {
  effect_chain_controls_visible_ = visible;
  if (!show_all_sections_) {
    addChildComponent(effect_chain_summary_);
    addChildComponent(effect_chain_selector_);
    addChildComponent(effect_move_up_);
    addChildComponent(effect_move_down_);
  }
  effect_chain_summary_.setVisible(visible);
  effect_chain_selector_.setVisible(visible);
  post_effect_order_selector_.setVisible(false);
  effect_move_up_.setVisible(visible);
  effect_move_down_.setVisible(visible);
  if (visible)
    refreshEffectChainControls();
  resized();
}

ValueBridge* SynthEditor::parameterBridge(const String& id) const {
  const auto found = parameters_by_id_.find(id);
  return found == parameters_by_id_.end() ? nullptr : found->second;
}

void SynthEditor::setParameterEngineValue(const String& id, float engineValue) {
  if (auto* bridge = parameterBridge(id)) {
    bridge->beginChangeGesture();
    bridge->setValueNotifyingHost(bridge->convertToPluginValue(engineValue));
    bridge->endChangeGesture();
  }
}

void SynthEditor::updateRoutingSummary() {
  auto textFor = [this](const String& id) {
    if (auto* bridge = parameterBridge(id))
      return bridge->getText(bridge->getValue(), 128);
    return String("Unavailable");
  };

  String summary;
  for (int osc = 1; osc <= vital::kNumOscillators; ++osc) {
    if (summary.isNotEmpty())
      summary += "; ";
    summary += "Oscillator " + String(osc) + " to " + textFor("osc_" + String(osc) + "_destination");
  }
  summary +=
      "; Sample to " + textFor("sample_destination") +
      "; Granular to " + textFor("granular_destination") +
      "; Filter 1 to " + textFor("filter_1_destination") +
      "; Filter 2 to " + textFor("filter_2_destination") +
      ". Filter link: " +
      ((parameterBridge("filter_2_filter_input") && parameterBridge("filter_2_filter_input")->getValue() >= 0.5f)
           ? "filter 1 into filter 2"
           : (parameterBridge("filter_1_filter_input") && parameterBridge("filter_1_filter_input")->getValue() >= 0.5f)
                 ? "filter 2 into filter 1"
                 : "parallel filters");
  const bool changed = routing_summary_.getText() != summary;
  routing_summary_.setText(summary, dontSendNotification);
  routing_summary_.setDescription(summary);
  if (changed) {
    if (auto* handler = routing_summary_.getAccessibilityHandler())
      handler->notifyAccessibilityEvent(AccessibilityEvent::titleChanged);
  }
}

void SynthEditor::refreshRoutingControls() {
  updateRoutingSummary();

  int mode = 0;
  if (parameterBridge("filter_2_filter_input") && parameterBridge("filter_2_filter_input")->getValue() >= 0.5f)
    mode = 1;
  else if (parameterBridge("filter_1_filter_input") && parameterBridge("filter_1_filter_input")->getValue() >= 0.5f)
    mode = 2;

  ScopedValueSetter<bool> guard(updating_routing_controls_, true);
  routing_mode_.setSelectedItemIndex(mode, dontSendNotification);
}

void SynthEditor::applyRoutingPreset(int preset) {
  if (preset == 0) {
    setParameterEngineValue("filter_1_filter_input", 0.0f);
    setParameterEngineValue("filter_2_filter_input", 0.0f);
    postPluginAnnouncement("Filters routed in parallel", AccessibilityHandler::AnnouncementPriority::high);
  }
  else if (preset == 1) {
    setParameterEngineValue("filter_1_filter_input", 0.0f);
    setParameterEngineValue("filter_2_filter_input", 1.0f);
    postPluginAnnouncement("Filter 1 routed into filter 2", AccessibilityHandler::AnnouncementPriority::high);
  }
  else if (preset == 2) {
    setParameterEngineValue("filter_2_filter_input", 0.0f);
    setParameterEngineValue("filter_1_filter_input", 1.0f);
    postPluginAnnouncement("Filter 2 routed into filter 1", AccessibilityHandler::AnnouncementPriority::high);
  }
  else if (preset == 3) {
    setParameterEngineValue("osc_1_destination", vital::constants::kFilter1);
    setParameterEngineValue("osc_2_destination", vital::constants::kFilter2);
    setParameterEngineValue("osc_3_destination", vital::constants::kEffects);
    setParameterEngineValue("osc_4_destination", vital::constants::kEffects);
    setParameterEngineValue("sample_destination", vital::constants::kEffects);
    setParameterEngineValue("granular_destination", vital::constants::kEffects);
    setParameterEngineValue("filter_1_destination", vital::constants::kEffects);
    setParameterEngineValue("filter_2_destination", vital::constants::kEffects);
    setParameterEngineValue("filter_1_filter_input", 0.0f);
    setParameterEngineValue("filter_2_filter_input", 0.0f);
    postPluginAnnouncement("Default routing applied", AccessibilityHandler::AnnouncementPriority::high);
  }
  refreshRoutingControls();
  timerCallback();
}

void SynthEditor::readEffectChainOrder(const String& sectionName, int* order) const {
  const String prefix = effectChainPrefixForSection(sectionName);
  bool slots_are_default = true;
  bool slots_are_valid = true;
  bool seen[vital::constants::kNumEffects] = {};

  for (int i = 0; i < vital::constants::kNumEffects; ++i) {
    const String slot_id = prefix + "effect_chain_slot_" + String(i + 1);
    auto* slot = parameterBridge(slot_id);
    if (slot == nullptr) {
      slots_are_valid = false;
      break;
    }

    const int effect = jlimit(0, vital::constants::kNumEffects - 1,
                              static_cast<int>(std::round(slot->convertToEngineValue(slot->getValue()))));
    order[i] = effect;
    slots_are_default = slots_are_default && effect == i;
    if (seen[effect])
      slots_are_valid = false;
    seen[effect] = true;
  }

  if (slots_are_valid && !slots_are_default)
    return;

  for (int i = 0; i < vital::constants::kNumEffects; ++i)
    order[i] = i;

  if (auto* bridge = parameterBridge(prefix + "effect_chain_order")) {
    const float max_order = vital::utils::factorial(vital::constants::kNumReorderableEffects) - 1;
    const float chain_order = jlimit(0.0f, max_order, std::round(bridge->convertToEngineValue(bridge->getValue())));
    vital::utils::decodeFloatToOrder(order, chain_order, vital::constants::kNumReorderableEffects);
  }

  if (auto* post_bridge = parameterBridge(prefix + "post_effect_order")) {
    if (post_bridge->convertToEngineValue(post_bridge->getValue()) >= 0.5f)
      std::swap(order[vital::constants::kFrequencyShifter], order[vital::constants::kLimiter]);
  }
}

void SynthEditor::writeEffectChainOrder(const String& sectionName, const int* order) {
  const String prefix = effectChainPrefixForSection(sectionName);
  bool legacy_seen[vital::constants::kNumReorderableEffects] = {};
  bool can_sync_legacy_order = true;
  for (int i = 0; i < vital::constants::kNumReorderableEffects; ++i) {
    if (!isPositiveAndBelow(order[i], vital::constants::kNumReorderableEffects) || legacy_seen[order[i]]) {
      can_sync_legacy_order = false;
      break;
    }
    legacy_seen[order[i]] = true;
  }
  for (int i = vital::constants::kNumReorderableEffects; i < vital::constants::kNumEffects; ++i)
    can_sync_legacy_order = can_sync_legacy_order && order[i] == i;

  if (can_sync_legacy_order) {
    int legacy_order[vital::constants::kNumReorderableEffects];
    for (int i = 0; i < vital::constants::kNumReorderableEffects; ++i)
      legacy_order[i] = order[i];
    const float encoded_order = vital::utils::encodeOrderToFloat(legacy_order,
                                                                 vital::constants::kNumReorderableEffects);
    if (auto* chain_order = parameterBridge(prefix + "effect_chain_order")) {
      if (chain_order->convertToEngineValue(chain_order->getValue()) != encoded_order)
        setParameterEngineValue(prefix + "effect_chain_order", encoded_order);
    }
  }

  for (int i = 0; i < vital::constants::kNumEffects; ++i) {
    const String slot_id = prefix + "effect_chain_slot_" + String(i + 1);
    auto* slot = parameterBridge(slot_id);
    if (slot == nullptr)
      continue;

    const int current = static_cast<int>(std::round(slot->convertToEngineValue(slot->getValue())));
    if (current != order[i])
      setParameterEngineValue(slot_id, static_cast<float>(order[i]));
  }

  if (auto* post_order = parameterBridge(prefix + "post_effect_order")) {
    if (post_order->convertToEngineValue(post_order->getValue()) != 0.0f)
      setParameterEngineValue(prefix + "post_effect_order", 0.0f);
  }
}

void SynthEditor::populateEffectChainSelector(ComboBox& selector, Label* summary, const String& sectionName) const {
  int order[vital::constants::kNumEffects];
  readEffectChainOrder(sectionName, order);

  const int previous = selector.getSelectedItemIndex();
  const bool use_pending_selection = pending_effect_chain_selected_index_ >= 0 &&
                                     pending_effect_chain_section_ == sectionName;
  selector.clear(dontSendNotification);
  String summary_text = "Current effects order: ";
  for (int i = 0; i < vital::constants::kNumEffects; ++i) {
    const String name = effectName(order[i]);
    selector.addItem(String(i + 1) + ". " + name, i + 1);
    summary_text += (i == 0 ? "" : ", ") + name;
  }

  selector.setSelectedItemIndex(jlimit(0, vital::constants::kNumEffects - 1,
                                       use_pending_selection ? pending_effect_chain_selected_index_ : previous),
                                dontSendNotification);
  if (use_pending_selection) {
    pending_effect_chain_section_.clear();
    pending_effect_chain_selected_index_ = -1;
  }
  if (summary != nullptr) {
    summary->setText(summary_text, dontSendNotification);
    summary->setDescription(summary_text);
    if (auto* handler = summary->getAccessibilityHandler())
      handler->notifyAccessibilityEvent(AccessibilityEvent::titleChanged);
  }
}

void SynthEditor::refreshEffectChainControls() {
  populateEffectChainSelector(effect_chain_selector_, &effect_chain_summary_, last_section_name);
}

void SynthEditor::rebuildSectionsAfterEffectOrderChange(const String& preferredSection, const String& focusedTitle,
                                                        const String& focusedParameter) {
  const String section = preferredSection.isNotEmpty() ? preferredSection : last_section_name;
  const String accessible_title = focusedTitle.isNotEmpty() ? focusedTitle : focusedAccessibleTitle();
  const String parameter_id = focusedParameter.isNotEmpty() ? focusedParameter : focusedParameterId();
  auto* persistent_focus = persistentFocusedComponent();
  buildSections();

  if (show_all_sections_)
    showAllSections(false);
  else if (section_names_.contains(section))
    selectSectionByName(section, false);
  else if (section_names_.size() > 0)
    selectSectionByName(section_names_[0], false);

  restoreFocusAfterRebuild(parameter_id, persistent_focus, accessible_title, section);
}

void SynthEditor::moveSelectedEffect(int direction) {
  const int selected = effect_chain_selector_.getSelectedItemIndex();
  const int next = selected + direction;
  if (!isPositiveAndBelow(selected, vital::constants::kNumEffects) ||
      !isPositiveAndBelow(next, vital::constants::kNumEffects))
    return;

  int order[vital::constants::kNumEffects];
  readEffectChainOrder(last_section_name, order);
  std::swap(order[selected], order[next]);
  const String moved = effectName(order[next]);
  writeEffectChainOrder(last_section_name, order);
  pending_effect_chain_section_ = last_section_name;
  pending_effect_chain_selected_index_ = next;
  const String focused_title = focusedAccessibleTitle();
  MessageManager::callAsync([this, section = last_section_name, focused_title, next] {
    rebuildSectionsAfterEffectOrderChange(section, focused_title);
    refreshEffectChainControls();
    effect_chain_selector_.setSelectedItemIndex(next, dontSendNotification);
  });
  postPluginAnnouncement(moved + " moved to position " + String(next + 1),
                                         AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::connectModulationAndPromptForAmount(const String& sourceId, const String& destinationId,
                                                      Component& target) {
  if (sourceId.isEmpty() || destinationId.isEmpty())
    return;
  if (isInvalidModulationPair(sourceId, destinationId)) {
    postPluginAnnouncement(modulationSourceLabelForId(sourceId) + " cannot modulate its own controls",
                                           AccessibilityHandler::AnnouncementPriority::high);
    return;
  }

  const bool created = synth_.connectModulation(sourceId.toStdString(), destinationId.toStdString());
  const int macro_index = macroIndexForControlId(sourceId);
  if (macro_index >= 0) {
    const int connection = synth_.getConnectionIndex(sourceId.toStdString(), destinationId.toStdString());
    if (connection >= 0)
      setParameterEngineValue("modulation_" + String(connection + 1) + "_bipolar",
                              isMacroBipolar(macro_index) ? 1.0f : 0.0f);
  }
  refreshModulationRoutes();
  for (int row = 0; row < static_cast<int>(modulation_routes_.size()); ++row) {
    const auto* route = modulation_routes_[row];
    if (route != nullptr && route->source_name == sourceId.toStdString() &&
        route->destination_name == destinationId.toStdString()) {
      modulation_list_.selectRow(row);
      break;
    }
  }

  promptForInitialModulationAmount(sourceId, destinationId, created, target);
}

void SynthEditor::promptForInitialModulationAmount(const String& sourceId, const String& destinationId,
                                                   bool created, Component& target) {
  parameter_value_prompt_visible_ = false;
  pending_parameter_value_id_.clear();
  pending_custom_value_title_.clear();
  pending_custom_value_apply_ = nullptr;
  pending_parameter_value_focus_ = &target;
  pending_modulation_source_ = sourceId;
  pending_modulation_destination_ = destinationId;
  pending_modulation_created_ = created;
  modulation_amount_prompt_visible_ = true;
  modulation_amount_prompt_.setText("Peak value for " + modulationSourceLabelForId(sourceId) + " to " +
                                    modulationDestinationLabelForId(destinationId),
                                    dontSendNotification);
  modulation_amount_prompt_.setTitle("Initial modulation amount");
  modulation_amount_prompt_.setDescription("Type the destination value this modulation should peak at");
  modulation_amount_editor_.setTitle("Modulation peak value");
  modulation_amount_editor_.setDescription("Type the destination value this modulation should peak at");
  modulation_amount_editor_.setHelpText("Type the real destination value, such as 12 semitones, 50%, 1000 Hz, or C4. Type raw followed by a number to enter a normalized amount.");
  modulation_amount_ok_.setTitle("Set modulation amount");
  modulation_amount_ok_.setDescription("Apply the typed initial modulation amount");
  modulation_amount_cancel_.setTitle("Cancel modulation amount");
  modulation_amount_cancel_.setDescription("Leave the modulation connected without changing the amount");
  modulation_amount_prompt_.setVisible(true);
  modulation_amount_editor_.setVisible(true);
  modulation_amount_ok_.setVisible(true);
  modulation_amount_cancel_.setVisible(true);
  String peak_value = "1.0";
  if (auto* bridge = parameterBridge(destinationId)) {
    peak_value = accessibleParameterText(*bridge, bridge->getValue());
    const int connection = synth_.getConnectionIndex(sourceId.toStdString(), destinationId.toStdString());
    if (connection >= 0) {
      if (auto* amount_bridge = parameterBridge("modulation_" + String(connection + 1) + "_amount")) {
        const auto& details = bridge->getDetails();
        const float current_engine = bridge->convertToEngineValue(bridge->getValue());
        const float amount = amount_bridge->convertToEngineValue(amount_bridge->getValue());
        const float peak_engine = jlimit(details.min, details.max,
                                         current_engine + amount * (details.max - details.min));
        peak_value = accessibleParameterText(*bridge, bridge->convertToPluginValue(peak_engine));
      }
    }
  }
  modulation_amount_editor_.setText(peak_value, dontSendNotification);
  rebuildFocusOrder();
  resized();
  modulation_amount_editor_.grabKeyboardFocus();
  modulation_amount_editor_.selectAll();
  if (auto* handler = modulation_amount_editor_.getAccessibilityHandler())
    handler->grabFocus();
  postPluginAnnouncement("Type modulation peak value",
                                         AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::promptForParameterValue(const String& parameterId, Component& target) {
  auto* bridge = parameterBridge(parameterId);
  if (bridge == nullptr)
    return;

  modulation_amount_prompt_visible_ = false;
  pending_modulation_source_.clear();
  pending_modulation_destination_.clear();
  pending_modulation_created_ = false;
  parameter_value_prompt_visible_ = true;
  pending_parameter_value_id_ = parameterId;
  pending_custom_value_title_.clear();
  pending_custom_value_apply_ = nullptr;
  pending_parameter_value_focus_ = &target;

  const String name = bridge->getName(128);
  String value = accessibleParameterText(*bridge, bridge->getValue());
  const int macro_index = macroIndexForControlId(parameterId);
  if (macro_index >= 0 && isMacroBipolar(macro_index))
    value = String(roundToInt((bridge->getValue() * 2.0f - 1.0f) * 100.0f)) + "%";
  modulation_amount_prompt_.setText("Set " + name + ". Current value " + value, dontSendNotification);
  modulation_amount_prompt_.setTitle("Set " + name);
  modulation_amount_prompt_.setDescription("Type a value for " + name);
  modulation_amount_editor_.setTitle(name + " value");
  modulation_amount_editor_.setDescription("Type a value for " + name);
  modulation_amount_editor_.setHelpText("Type a value and press Return. Units like percent, ms, seconds, dB, note names, and named options are accepted.");
  if (isTransposeQuantizeParameter(parameterId)) {
    modulation_amount_editor_.setDescription("Type a scale name such as major or minor, custom notes such as C D E F G A B, or add global for global snap");
    modulation_amount_editor_.setHelpText("Examples: major, minor, global major, C D E F G A B, C D E flat F G A flat B flat, chromatic, or off.");
  }
  modulation_amount_ok_.setTitle("Set " + name);
  modulation_amount_ok_.setDescription("Apply the typed value for " + name);
  modulation_amount_cancel_.setTitle("Cancel value entry");
  modulation_amount_cancel_.setDescription("Cancel value entry for " + name);

  modulation_amount_prompt_.setVisible(true);
  modulation_amount_editor_.setVisible(true);
  modulation_amount_ok_.setVisible(true);
  modulation_amount_cancel_.setVisible(true);
  modulation_amount_editor_.setText(value, dontSendNotification);
  rebuildFocusOrder();
  resized();
  modulation_amount_editor_.grabKeyboardFocus();
  modulation_amount_editor_.selectAll();
  if (auto* handler = modulation_amount_editor_.getAccessibilityHandler())
    handler->grabFocus();
  postPluginAnnouncement("Type value for " + name,
                                         AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::promptForMacroName(int macroIndex, Component& target) {
  if (!isPositiveAndBelow(macroIndex, vital::kNumMacros))
    return;

  Component::SafePointer<Component> target_pointer(&target);
  promptForCustomValue("Macro " + String(macroIndex + 1) + " name", synth_.getMacroName(macroIndex), target,
                       [this, macroIndex, target_pointer](const String& text) {
                         String name = text.trim();
                         if (name.isEmpty())
                           name = "MACRO " + String(macroIndex + 1);

                         synth_.setMacroName(macroIndex, name);
                         if (target_pointer != nullptr) {
                           target_pointer->setTitle(name);
                           target_pointer->setDescription("Adjust " + name);
                         }
                       });
}

void SynthEditor::promptForCustomValue(const String& title, const String& value, Component& target,
                                       std::function<void(const String&)> apply) {
  if (!apply)
    return;

  modulation_amount_prompt_visible_ = false;
  pending_modulation_source_.clear();
  pending_modulation_destination_.clear();
  pending_modulation_created_ = false;
  parameter_value_prompt_visible_ = true;
  pending_parameter_value_id_.clear();
  pending_custom_value_title_ = title;
  pending_custom_value_apply_ = std::move(apply);
  pending_parameter_value_focus_ = &target;

  modulation_amount_prompt_.setText("Set " + title + ". Current value " + value, dontSendNotification);
  modulation_amount_prompt_.setTitle("Set " + title);
  modulation_amount_prompt_.setDescription("Type a value for " + title);
  modulation_amount_editor_.setTitle(title + " value");
  modulation_amount_editor_.setDescription("Type a value for " + title);
  modulation_amount_editor_.setHelpText("Type a value and press Return or OK");
  modulation_amount_ok_.setTitle("Set " + title);
  modulation_amount_ok_.setDescription("Apply the typed value for " + title);
  modulation_amount_cancel_.setTitle("Cancel value entry");
  modulation_amount_cancel_.setDescription("Cancel value entry for " + title);

  modulation_amount_prompt_.setVisible(true);
  modulation_amount_editor_.setVisible(true);
  modulation_amount_ok_.setVisible(true);
  modulation_amount_cancel_.setVisible(true);
  modulation_amount_editor_.setText(value, dontSendNotification);
  rebuildFocusOrder();
  resized();
  modulation_amount_editor_.grabKeyboardFocus();
  modulation_amount_editor_.selectAll();
  if (auto* handler = modulation_amount_editor_.getAccessibilityHandler())
    handler->grabFocus();
  postPluginAnnouncement("Type value for " + title,
                                         AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::applyInlineTextPrompt() {
  if (parameter_value_prompt_visible_)
    applyParameterValue();
  else if (modulation_amount_prompt_visible_)
    applyInitialModulationAmount();
}

void SynthEditor::applyInitialModulationAmount() {
  const String source_id = pending_modulation_source_;
  const String destination_id = pending_modulation_destination_;
  const float amount = modulationAmountFromPeakText(destination_id, modulation_amount_editor_.getText());
  const int connection = synth_.getConnectionIndex(source_id.toStdString(), destination_id.toStdString());
  if (connection >= 0) {
    setParameterEngineValue("modulation_" + String(connection + 1) + "_amount", amount);
    refreshModulationRoutes();
    showSelectedModulationParameters();
  }

  modulation_amount_prompt_visible_ = false;
  modulation_amount_prompt_.setVisible(false);
  modulation_amount_editor_.setVisible(false);
  modulation_amount_ok_.setVisible(false);
  modulation_amount_cancel_.setVisible(false);
  pending_modulation_source_.clear();
  pending_modulation_destination_.clear();
  rebuildFocusOrder();
  resized();

  if (pending_parameter_value_focus_ != nullptr)
    pending_parameter_value_focus_->grabKeyboardFocus();
  pending_parameter_value_focus_ = nullptr;

  postPluginAnnouncement(modulationSourceLabelForId(source_id) + " assigned to " +
                                         modulationDestinationLabelForId(destination_id) +
                                         ", peak " + modulation_amount_editor_.getText().trim(),
                                         AccessibilityHandler::AnnouncementPriority::high);
}

float SynthEditor::modulationAmountFromPeakText(const String& destinationId, const String& text) const {
  const String trimmed = text.trim();
  const String lower = trimmed.toLowerCase();
  if (lower.startsWith("raw ") || lower.startsWith("normalized ")) {
    const String value = lower.startsWith("raw ") ? trimmed.fromFirstOccurrenceOf(" ", false, false)
                                                  : trimmed.fromFirstOccurrenceOf(" ", false, false);
    return jlimit(-1.0f, 1.0f, static_cast<float>(value.getDoubleValue()));
  }

  auto* bridge = parameterBridge(destinationId);
  if (bridge == nullptr)
    return jlimit(-1.0f, 1.0f, static_cast<float>(trimmed.getDoubleValue()));

  const float target_normalized = static_cast<float>(accessibleParameterValueForText(*bridge, trimmed));
  const float target_engine = bridge->convertToEngineValue(target_normalized);
  const float current_engine = bridge->convertToEngineValue(bridge->getValue());
  const auto& details = bridge->getDetails();
  const float range = details.max - details.min;
  if (range <= 0.0f)
    return 0.0f;

  return jlimit(-1.0f, 1.0f, (target_engine - current_engine) / range);
}

void SynthEditor::applyParameterValue() {
  const String parameter_id = pending_parameter_value_id_;
  const String custom_title = pending_custom_value_title_;
  if (parameter_id.isEmpty() && pending_custom_value_apply_) {
    auto apply = std::move(pending_custom_value_apply_);
    apply(modulation_amount_editor_.getText());

    parameter_value_prompt_visible_ = false;
    modulation_amount_prompt_.setVisible(false);
    modulation_amount_editor_.setVisible(false);
    modulation_amount_ok_.setVisible(false);
    modulation_amount_cancel_.setVisible(false);
    pending_custom_value_title_.clear();
    rebuildFocusOrder();
    resized();

    if (pending_parameter_value_focus_ != nullptr)
      pending_parameter_value_focus_->grabKeyboardFocus();
    pending_parameter_value_focus_ = nullptr;

    postPluginAnnouncement(custom_title + " " + modulation_amount_editor_.getText(),
                                           AccessibilityHandler::AnnouncementPriority::high);
    return;
  }

  auto* bridge = parameterBridge(parameter_id);
  if (bridge != nullptr) {
    float value;
    if (fine_tune_sample_loop_ && isSampleLoopPointParameter(parameter_id)) {
      const int length = sampleLoopPointLength();
      const double normalized = length > 0
          ? modulation_amount_editor_.getText().trim().getDoubleValue() / length
          : bridge->getValue();
      value = static_cast<float>(jlimit(0.0, 1.0, normalized));
    }
    else {
      value = static_cast<float>(accessibleParameterValueForText(*bridge, modulation_amount_editor_.getText()));
    }
    const int macro_index = macroIndexForControlId(parameter_id);
    if (macro_index >= 0) {
      if (isMacroBipolar(macro_index)) {
        const String text = modulation_amount_editor_.getText().trim();
        float percent = text.getFloatValue();
        if (text.contains("%") || std::abs(percent) > 1.0f)
          percent *= 0.01f;
        value = jlimit(0.0f, 1.0f, (jlimit(-1.0f, 1.0f, percent) + 1.0f) * 0.5f);
      }
      else if (bridge->convertToEngineValue(value) < 0.0f) {
        value = bridge->convertToPluginValue(0.0f);
      }
    }
    if (isTransposeQuantizeParameter(parameter_id)) {
      const int current_value = static_cast<int>(std::round(bridge->convertToEngineValue(bridge->getValue())));
      int quantize_value = current_value;
      if (parseTransposeQuantizeText(modulation_amount_editor_.getText(), current_value, quantize_value)) {
        value = bridge->convertToPluginValue(static_cast<float>(quantize_value));
        if (parameter_id.startsWith("osc_")) {
          const String prefix = parameter_id.upToLastOccurrenceOf("transpose_quantize", false, false) +
                                "transpose_quantize_";
          if (auto* scale_bridge = parameterBridge(prefix + "scale")) {
            scale_bridge->beginChangeGesture();
            scale_bridge->setValueNotifyingHost(scale_bridge->convertToPluginValue(14.0f));
            scale_bridge->endChangeGesture();
          }
        }
      }
    }
    bridge->beginChangeGesture();
    bridge->setValueNotifyingHost(value);
    bridge->endChangeGesture();
  }

  parameter_value_prompt_visible_ = false;
  modulation_amount_prompt_.setVisible(false);
  modulation_amount_editor_.setVisible(false);
  modulation_amount_ok_.setVisible(false);
  modulation_amount_cancel_.setVisible(false);
  pending_parameter_value_id_.clear();
  pending_custom_value_title_.clear();
  pending_custom_value_apply_ = nullptr;
  rebuildFocusOrder();
  resized();

  if (pending_parameter_value_focus_ != nullptr)
    pending_parameter_value_focus_->grabKeyboardFocus();
  pending_parameter_value_focus_ = nullptr;

  if (bridge != nullptr) {
    const int macro_index = macroIndexForControlId(parameter_id);
    if (macro_index >= 0 && isMacroBipolar(macro_index)) {
      postPluginAnnouncement(synth_.getMacroName(macro_index) + " " +
                                             String(roundToInt((bridge->getValue() * 2.0f - 1.0f) * 100.0f)) + "%",
                                             AccessibilityHandler::AnnouncementPriority::high);
    }
    else {
      postPluginAnnouncement(bridge->getName(128) + " " + accessibleParameterText(*bridge, bridge->getValue()),
                                             AccessibilityHandler::AnnouncementPriority::high);
    }
  }
}

bool SynthEditor::isMacroBipolar(int macroIndex) const {
  if (!isPositiveAndBelow(macroIndex, vital::kNumMacros))
    return false;

  if (auto* bridge = parameterBridge(macroBipolarParameterId(macroIndex)))
    return bridge->getValue() >= 0.5f;
  return false;
}

void SynthEditor::setMacroBipolar(int macroIndex, bool bipolar) {
  if (!isPositiveAndBelow(macroIndex, vital::kNumMacros))
    return;

  if (auto* bridge = parameterBridge(macroBipolarParameterId(macroIndex))) {
    bridge->beginChangeGesture();
    bridge->setValueNotifyingHost(bipolar ? 1.0f : 0.0f);
    bridge->endChangeGesture();
  }

  const String source_id = "macro_control_" + String(macroIndex + 1);
  for (auto* route : synth_.getModulationConnections()) {
    if (route == nullptr || route->source_name != source_id.toStdString() || route->destination_name.empty())
      continue;

    const int connection = synth_.getConnectionIndex(route->source_name, route->destination_name);
    if (connection >= 0)
      setParameterEngineValue("modulation_" + String(connection + 1) + "_bipolar", bipolar ? 1.0f : 0.0f);
  }
  refreshModulationRoutes();
}

bool SynthEditor::moveEffectInSection(const String& sectionName, const String& effectId, int direction,
                                      const String& focusedTitle) {
  if (sectionName.isEmpty() || effectId.isEmpty())
    return false;

  int order[vital::constants::kNumEffects];
  readEffectChainOrder(sectionName, order);

  int selected = -1;
  for (int i = 0; i < vital::constants::kNumEffects; ++i) {
    if (strings::kEffectOrder[order[i]] == effectId) {
      selected = i;
      break;
    }
  }

  const int next = selected + direction;
  if (!isPositiveAndBelow(selected, vital::constants::kNumEffects) ||
      !isPositiveAndBelow(next, vital::constants::kNumEffects))
    return true;

  std::swap(order[selected], order[next]);
  const String moved = effectName(order[next]);
  const String parameter_id = focusedParameterId();
  writeEffectChainOrder(sectionName, order);
  String chain_section = isEffectChainSection(sectionName) ? sectionName : String(kEffectsChainSection);
  for (int bus = 1; bus <= vital::kNumBuses; ++bus) {
    const String bus_prefix = "Bus " + String(bus) + " - ";
    if (sectionName.startsWith(bus_prefix)) {
      chain_section = "Bus " + String(bus) + " - " + kEffectsChainSection;
      break;
    }
  }
  pending_effect_chain_section_ = chain_section;
  pending_effect_chain_selected_index_ = next;
  MessageManager::callAsync([this, sectionName, focusedTitle, parameter_id] {
    rebuildSectionsAfterEffectOrderChange(sectionName, focusedTitle, parameter_id);
  });
  postPluginAnnouncement(moved + " moved to position " + String(next + 1),
                                         AccessibilityHandler::AnnouncementPriority::high);
  return true;
}

bool SynthEditor::handleEffectShortcut(const String& sectionName, const KeyPress& key, Component& target) {
  const String effect_id = effectIdForSection(sectionName);
  if (effect_id.isEmpty())
    return false;

  if (isEffectMoveEarlierKey(key))
    return moveEffectInSection(sectionName, effect_id, -1, target.getTitle());
  if (isEffectMoveLaterKey(key))
    return moveEffectInSection(sectionName, effect_id, 1, target.getTitle());
  return false;
}

bool SynthEditor::handleMacroShortcut(const String& parameterId, const KeyPress& key, Component& target) {
  const int macro_index = macroIndexForControlId(parameterId);
  if (macro_index < 0)
    return false;

  if (key.getModifiers().isShiftDown() && key.getKeyCode() == KeyPress::returnKey) {
    promptForMacroName(macro_index, target);
    return true;
  }

  const bool is_b_key = CharacterFunctions::toLowerCase(key.getTextCharacter()) == 'b' ||
                        key.getKeyCode() == 'b' || key.getKeyCode() == 'B';
  if (key.getModifiers().isAnyModifierKeyDown() || !is_b_key) {
    return false;
  }

  const bool bipolar = !isMacroBipolar(macro_index);
  setMacroBipolar(macro_index, bipolar);

  if (auto* bridge = parameterBridge(parameterId)) {
    if (auto* slider = dynamic_cast<Slider*>(&target)) {
      const double interval = slider->getInterval();
      slider->setRange(0.0, 1.0, interval);
      slider->setValue(jlimit(0.0, 1.0, static_cast<double>(bridge->getValue())), dontSendNotification);
      if (auto* handler = slider->getAccessibilityHandler())
        handler->notifyAccessibilityEvent(AccessibilityEvent::valueChanged);
    }
  }

  postPluginAnnouncement(synth_.getMacroName(macro_index) +
                                         (bipolar ? " bipolar, minus 100 to 100"
                                                  : " unipolar, 0 to 100"),
                                         AccessibilityHandler::AnnouncementPriority::high);
  return true;
}

void SynthEditor::cancelInlineTextPrompt() {
  if (parameter_value_prompt_visible_) {
    parameter_value_prompt_visible_ = false;
    modulation_amount_prompt_.setVisible(false);
    modulation_amount_editor_.setVisible(false);
    modulation_amount_ok_.setVisible(false);
    modulation_amount_cancel_.setVisible(false);
    pending_parameter_value_id_.clear();
    pending_custom_value_title_.clear();
    pending_custom_value_apply_ = nullptr;
    rebuildFocusOrder();
    resized();
    if (pending_parameter_value_focus_ != nullptr)
      pending_parameter_value_focus_->grabKeyboardFocus();
    pending_parameter_value_focus_ = nullptr;
    postPluginAnnouncement("Value entry cancelled",
                                           AccessibilityHandler::AnnouncementPriority::medium);
    return;
  }

  if (modulation_amount_prompt_visible_)
    cancelInitialModulationAmount();
}

void SynthEditor::cancelInitialModulationAmount() {
  const String source_id = pending_modulation_source_;
  const String destination_id = pending_modulation_destination_;
  const bool created = pending_modulation_created_;
  modulation_amount_prompt_visible_ = false;
  modulation_amount_prompt_.setVisible(false);
  modulation_amount_editor_.setVisible(false);
  modulation_amount_ok_.setVisible(false);
  modulation_amount_cancel_.setVisible(false);
  pending_modulation_source_.clear();
  pending_modulation_destination_.clear();
  rebuildFocusOrder();
  resized();

  if (pending_parameter_value_focus_ != nullptr)
    pending_parameter_value_focus_->grabKeyboardFocus();
  pending_parameter_value_focus_ = nullptr;

  postPluginAnnouncement(modulationSourceLabelForId(source_id) + " assigned to " +
                                         modulationDestinationLabelForId(destination_id) +
                                         (created ? "" : ", already assigned"),
                                         AccessibilityHandler::AnnouncementPriority::high);
}

PopupMenu SynthEditor::createModulationSourceSubmenu(const String& destinationId, std::map<int, String>& choices,
                                                     int firstItemId) {
  struct SourceChoice {
    String id;
    String label;
  };

  auto addSource = [](std::map<String, std::vector<SourceChoice>>& groups,
                      const String& group, const String& id) {
    groups[group].push_back({ id, readableId(id) });
  };

  std::map<String, std::vector<SourceChoice>> groups;
  for (const auto& id : modulation_source_ids_) {
    if (isInvalidModulationPair(id, destinationId))
      continue;

    if (id.startsWith("lfo_"))
      addSource(groups, "LFOs", id);
    else if (id.startsWith("env_"))
      addSource(groups, "Envelopes", id);
    else if (id.startsWith("random_"))
      addSource(groups, "Random", id);
    else if (id.startsWith("macro_"))
      addSource(groups, "Macros", id);
    else if (id == "aftertouch" || id == "velocity" || id == "lift" || id == "mod_wheel" ||
             id == "pitch_wheel" || id == "slide" || id == "note" || id == "note_in_octave" ||
             id == "stereo")
      addSource(groups, "MIDI and expression", id);
  }

  PopupMenu menu;
  int item_id = firstItemId;
  for (const auto& group_name : StringArray{ "LFOs", "Envelopes", "Random", "Macros",
                                             "MIDI and expression" }) {
    const auto found = groups.find(group_name);
    if (found == groups.end() || found->second.empty())
      continue;

    auto sources = found->second;
    std::stable_sort(sources.begin(), sources.end(), [](const SourceChoice& a, const SourceChoice& b) {
      const int rank_a = modulationSourceSortRank(a.id);
      const int rank_b = modulationSourceSortRank(b.id);
      if (rank_a != rank_b)
        return rank_a < rank_b;
      return a.label.compareNatural(b.label) < 0;
    });

    PopupMenu source_menu;
    for (const auto& source : sources) {
      choices[item_id] = source.id;
      const int macro_index = macroIndexForControlId(source.id);
      const String label = macro_index >= 0 ? synth_.getMacroName(macro_index)
                                            : modulationSourceLabelForId(source.id);
      source_menu.addItem(item_id++, label);
    }
    menu.addSubMenu(group_name, source_menu, true);
  }

  if (choices.empty())
    menu.addItem(firstItemId, "No modulation sources available", false);

  return menu;
}

int SynthEditor::getNumRows() { return static_cast<int>(modulation_routes_.size()); }

String SynthEditor::getNameForRow(int row) {
  if (!isPositiveAndBelow(row, static_cast<int>(modulation_routes_.size())))
    return {};
  const auto* route = modulation_routes_[row];
  const int index = synth_.getConnectionIndex(route->source_name, route->destination_name);
  const String destination = vital::Parameters::isParameter(route->destination_name)
      ? vital::Parameters::getDisplayName(route->destination_name) : readableId(route->destination_name);
  return "Modulation " + numberWord(index + 1) + ", " +
         modulationSourceLabelForId(route->source_name) + " to " + destination;
}

void SynthEditor::paintListBoxItem(int row, Graphics& graphics, int width, int height, bool selected) {
  if (selected)
    graphics.fillAll(Colour(0xff315f8f));
  graphics.setColour(Colours::white);
  graphics.drawText(getNameForRow(row), 8, 0, width - 16, height, Justification::centredLeft);
}

void SynthEditor::selectedRowsChanged(int) { showSelectedModulationParameters(); }

void SynthEditor::refreshModulationRoutes() {
  modulation_routes_.clear();
  for (auto* route : synth_.getModulationConnections()) {
    if (!route->source_name.empty() && !route->destination_name.empty())
      modulation_routes_.push_back(route);
  }
  std::stable_sort(modulation_routes_.begin(), modulation_routes_.end(), [this](auto* a, auto* b) {
    return synth_.getConnectionIndex(a->source_name, a->destination_name) <
           synth_.getConnectionIndex(b->source_name, b->destination_name);
  });
  modulation_list_.updateContent();
  if (!modulation_routes_.empty() && modulation_list_.getSelectedRow() < 0)
    modulation_list_.selectRow(0);
  if (modulation_routes_.empty())
    showSelectedModulationParameters();
  refreshVisibleModulationLabels();
}

void SynthEditor::refreshVisibleModulationLabels() {
  for (auto& header : section_headers_) {
    auto* section = dynamic_cast<AccessibleSectionHeader*>(header.get());
    if (section == nullptr || !section->getProperties().contains("ModulationSlot"))
      continue;

    const int slot = static_cast<int>(section->getProperties()["ModulationSlot"]);
    section->setHeaderTitle(modulationSlotTitle(slot));
  }

  for (auto& row : rows_) {
    if (!row)
      continue;

    const String id = row->parameterId();
    if (!id.startsWith("modulation_"))
      continue;

    const String title = modulationControlTitle(id);
    row->setAccessibleName(title, "Adjust " + title);
  }

  modulation_list_.updateContent();
  if (auto* handler = modulation_list_.getAccessibilityHandler())
    handler->notifyAccessibilityEvent(AccessibilityEvent::titleChanged);
}

void SynthEditor::showSelectedModulationParameters() {
  if (!modulation_controls_visible_)
    return;
  rows_.clear();
  rows_container_.removeAllChildren();
  const int row = modulation_list_.getSelectedRow();
  if (!isPositiveAndBelow(row, static_cast<int>(modulation_routes_.size()))) {
    rows_container_.setSize(jmax(620, viewport_.getMaximumVisibleWidth()), 0);
    return;
  }
  const auto* route = modulation_routes_[row];
  const int connection = synth_.getConnectionIndex(route->source_name, route->destination_name);
  if (connection < 0)
    return;
  const String prefix = "modulation_" + String(connection + 1) + "_";
  int y = 0;
  for (auto* parameter : sections_["Modulation routing"]) {
    const auto* bridge = dynamic_cast<ValueBridge*>(parameter);
    if (bridge == nullptr || !bridge->getParameterId().startsWith(prefix))
      continue;
    auto control = std::make_unique<AccessibleParameterRow>(*parameter);
    control->setModulationSourceSubmenuCallback([this](const String& destinationId,
                                                       std::map<int, String>& choices, int firstItemId) {
      return createModulationSourceSubmenu(destinationId, choices, firstItemId);
    });
    control->setModulationAssignCallback([this](const String& sourceId, const String& destinationId,
                                                Component& target) {
      connectModulationAndPromptForAmount(sourceId, destinationId, target);
    });
    control->setModulationEditCallback([this](const String& sourceId, const String& destinationId,
                                              Component& target) {
      promptForInitialModulationAmount(sourceId, destinationId, false, target);
    });
    control->setModulationDestinationPredicate([this](const String& id) { return isModulationDestinationId(id); });
    control->setModulationRemovalCallbacks(
        [this](const String& destinationId) { return modulationSourcesForDestination(destinationId); },
        [this](const String& sourceId, const String& destinationId, Component& target) {
          removeModulationFromParameter(sourceId, destinationId, target);
        });
    control->setMidiLearnCallback([this](const String& parameterId, Component&, bool clear) {
      if (clear) {
        synth_.clearMidiLearn(parameterId.toStdString());
        postPluginAnnouncement("Cleared MIDI learn for " + modulationDestinationLabelForId(parameterId),
                                               AccessibilityHandler::AnnouncementPriority::high);
      }
      else {
        synth_.armMidiLearn(parameterId.toStdString());
        postPluginAnnouncement("MIDI learn armed for " + modulationDestinationLabelForId(parameterId),
                                               AccessibilityHandler::AnnouncementPriority::high);
      }
    });
    control->setValueEntryCallback([this](const String& parameterId, Component& target) {
      promptForParameterValue(parameterId, target);
    });
    control->setExtraCommandCallback([this](const String& parameterId, const KeyPress& key, Component& target) {
      return handleMacroShortcut(parameterId, key, target);
    });
    control->setAccessibleName(modulationControlTitle(bridge->getParameterId()));
    control->setBounds(0, y, jmax(620, viewport_.getMaximumVisibleWidth()), 48);
    rows_container_.addAndMakeVisible(control.get());
    rows_.push_back(std::move(control));
    y += 48;
  }
  rows_container_.setSize(jmax(620, viewport_.getMaximumVisibleWidth()), y);
  viewport_.setViewPosition(0, 0);
}

void SynthEditor::configureWavetableActions(const String& section_name) {
  active_oscillator_ = -1;
  for (int osc = 1; osc <= vital::kNumOscillators; ++osc) {
    if (section_name == "Oscillator " + String(osc)) {
      active_oscillator_ = osc - 1;
      break;
    }
  }
  const bool visible = active_oscillator_ >= 0;
  wavetable_name_.setVisible(visible);
  load_wavetable_.setVisible(visible);
  reset_wavetable_.setVisible(visible);
  oscillator_summary_.setVisible(visible);
  oscillator_octave_.setVisible(visible);
  oscillator_semitone_.setVisible(visible);
  oscillator_fine_tune_.setVisible(visible);
  oscillator_wave_frame_.setVisible(visible);
  oscillator_scale_key_.setVisible(visible);
  oscillator_scale_type_.setVisible(visible);
  oscillator_scale_mode_.setVisible(visible);
  if (visible) {
    if (auto* creator = synth_.getWavetableCreator(active_oscillator_))
      wavetable_name_.setText("Current wavetable: " + String(creator->getName()), dontSendNotification);
    refreshOscillatorDirectControls();
  }
  resized();
}

void SynthEditor::refreshOscillatorDirectControls() {
  if (active_oscillator_ < 0)
    return;

  const String prefix = "osc_" + String(active_oscillator_ + 1) + "_";
  auto engineValue = [this, &prefix](const String& suffix) {
    if (auto* bridge = parameterBridge(prefix + suffix))
      return bridge->convertToEngineValue(bridge->getValue());
    return 0.0f;
  };

  const int transpose = roundToInt(engineValue("transpose"));
  int octave = transpose / 12;
  int semitone = transpose - octave * 12;
  octave = jlimit(-4, 4, octave);
  semitone = jlimit(-11, 11, semitone);
  const float fine = engineValue("tune") * 100.0f;
  const float wave_frame = engineValue("wave_frame");

  ScopedValueSetter<bool> guard(updating_oscillator_controls_, true);
  oscillator_octave_.setSelectedId(octave + 5, dontSendNotification);
  oscillator_semitone_.setSelectedId(semitone + 12, dontSendNotification);
  oscillator_fine_tune_.setValue(fine, dontSendNotification);
  oscillator_wave_frame_.setValue(wave_frame, dontSendNotification);
  refreshOscillatorScaleControls();

  const String summary = "Oscillator " + String(active_oscillator_ + 1) +
                         ": octave " + String(octave) +
                         ", semitone " + String(semitone) +
                         ", fine tune " + String(roundToInt(fine)) + " cents" +
                         ", wave frame " + String(roundToInt(wave_frame)) +
                         ", transpose quantize " + oscillator_scale_key_.getText() + " " +
                         oscillator_scale_type_.getText() + ", " + oscillator_scale_mode_.getText();
  oscillator_summary_.setText(summary, dontSendNotification);
  oscillator_summary_.setDescription(summary);
  if (auto* handler = oscillator_summary_.getAccessibilityHandler())
    handler->notifyAccessibilityEvent(AccessibilityEvent::titleChanged);
}

void SynthEditor::refreshOscillatorScaleControls() {
  if (active_oscillator_ < 0)
    return;

  const String prefix = "osc_" + String(active_oscillator_ + 1) + "_transpose_quantize_";
  auto engineValue = [this, &prefix](const String& suffix) {
    if (auto* bridge = parameterBridge(prefix + suffix))
      return bridge->convertToEngineValue(bridge->getValue());
    return 0.0f;
  };

  const int key = jlimit(0, vital::kNotesPerOctave - 1, roundToInt(engineValue("key")));
  const int scale_index = jlimit(0, static_cast<int>(transposeQuantizeScales().size()) - 1,
                                 roundToInt(engineValue("scale")));
  const int mode = jlimit(0, 1, roundToInt(engineValue("mode")));
  oscillator_scale_key_.setSelectedId(key + 1, dontSendNotification);
  oscillator_scale_type_.setSelectedId(scale_index + 1, dontSendNotification);
  oscillator_scale_mode_.setSelectedId(mode + 1, dontSendNotification);
}

void SynthEditor::setOscillatorTransposeFromDirectControls() {
  if (active_oscillator_ < 0)
    return;
  const int octave = oscillator_octave_.getSelectedId() - 5;
  const int semitone = oscillator_semitone_.getSelectedId() - 12;
  const int transpose = jlimit(-48, 48, octave * 12 + semitone);
  setParameterEngineValue("osc_" + String(active_oscillator_ + 1) + "_transpose", static_cast<float>(transpose));
  refreshOscillatorDirectControls();
  postPluginAnnouncement("Oscillator " + String(active_oscillator_ + 1) +
                                         " transpose " + String(transpose) + " semitones",
                                         AccessibilityHandler::AnnouncementPriority::medium);
}

void SynthEditor::setOscillatorScaleFromDirectControls() {
  if (active_oscillator_ < 0)
    return;

  const int key = jlimit(0, vital::kNotesPerOctave - 1, oscillator_scale_key_.getSelectedId() - 1);
  const int scale_index = oscillator_scale_type_.getSelectedId() - 1;
  const auto& scales = transposeQuantizeScales();
  if (!isPositiveAndBelow(scale_index, static_cast<int>(scales.size())))
    return;

  const bool global = oscillator_scale_mode_.getSelectedId() == 2;
  const String prefix = "osc_" + String(active_oscillator_ + 1) + "_transpose_quantize_";
  setParameterEngineValue(prefix + "key", static_cast<float>(key));
  setParameterEngineValue(prefix + "scale", static_cast<float>(scale_index));
  setParameterEngineValue(prefix + "mode", global ? 1.0f : 0.0f);
  refreshOscillatorDirectControls();
  postPluginAnnouncement("Oscillator " + String(active_oscillator_ + 1) +
                                         " transpose quantize " + transposeKeyName(key) + " " +
                                         scales[scale_index].name + ", " + (global ? "global" : "local"),
                                         AccessibilityHandler::AnnouncementPriority::medium);
}

void SynthEditor::chooseWavetableFile() {
  chooseWavetableFile(WavetableCreator::kWavetableSplice);
}

void SynthEditor::chooseWavetableFile(int audioLoadStyle) {
  if (active_oscillator_ < 0)
    return;
  wavetable_chooser_ = std::make_unique<FileChooser>("Load Vital wavetable or audio file",
                                                     File(), vital::kWavetableExtensionsList);
  wavetable_chooser_->launchAsync(FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles,
                                  [this, audioLoadStyle](const FileChooser& chooser) {
    const auto file = chooser.getResult();
    if (file.existsAsFile())
      loadWavetableFile(file, audioLoadStyle);
    wavetable_chooser_.reset();
  });
}

void SynthEditor::loadWavetableFile(const File& file) {
  loadWavetableFile(file, WavetableCreator::kWavetableSplice);
}

void SynthEditor::loadWavetableFile(const File& file, int audioLoadStyle) {
  if (active_oscillator_ < 0)
    return;

  bool processing_paused = false;
  try {
    if (auto* creator = synth_.getWavetableCreator(active_oscillator_)) {
      const auto extension = file.getFileExtension().toLowerCase();
      const bool is_audio_file = extension == ".wav" || extension == ".flac" ||
                                 extension == ".aif" || extension == ".aiff";
      String wavetable_string;
      AudioSampleBuffer sample_buffer;
      int sample_rate = 0;
      json data;

      if (is_audio_file) {
        auto input_stream = std::make_unique<FileInputStream>(file);
        if (!input_stream->openedOk())
          throw std::runtime_error("Unable to open audio file");

        wavetable_string = getWavetableDataString(*input_stream);
        sample_rate = loadAudioFile(sample_buffer, input_stream.release());
        if (sample_rate == 0 || sample_buffer.getNumSamples() == 0)
          throw std::runtime_error("Unable to decode audio file");
      }
      else {
        data = json::parse(file.loadFileAsString().toStdString(), nullptr, false);
        if (data.is_discarded())
          throw std::runtime_error("Unable to parse wavetable JSON");
      }

      synth_.pauseProcessing(true);
      processing_paused = true;
      if (is_audio_file) {
        creator->initFromAudioFile(sample_buffer.getReadPointer(0), sample_buffer.getNumSamples(),
                                   sample_rate, static_cast<WavetableCreator::AudioFileLoadStyle>(audioLoadStyle),
                                   getFadeStyleFromWavetableString(wavetable_string));
        creator->setName(file.getFileNameWithoutExtension().toStdString());
        creator->setAuthor(getAuthorFromWavetableString(wavetable_string).toStdString());
      }
      else {
        // Do not preflight with WavetableCreator::isValidJson() here. Older factory
        // Vital tables may omit version/name fields but jsonToState() can still
        // migrate and load them through WavetableCreator::updateJson().
        creator->jsonToState(data);
        if (creator->getName().empty())
          creator->setName(file.getFileNameWithoutExtension().toStdString());
      }
      creator->setFileLoaded(file.getFullPathName().toStdString());
      synth_.pauseProcessing(false);
      processing_paused = false;
      wavetable_name_.setText("Current wavetable: " + String(creator->getName()), dontSendNotification);
      const String import_suffix = is_audio_file ? " as " + wavetableAudioLoadStyleName(audioLoadStyle) : "";
      postPluginAnnouncement("Loaded " + file.getFileNameWithoutExtension() + import_suffix +
                                               " in oscillator " + String(active_oscillator_ + 1),
                                               AccessibilityHandler::AnnouncementPriority::high);
    }
  }
  catch (const std::exception& error) {
    if (processing_paused)
      synth_.pauseProcessing(false);
    NativeMessageBox::showMessageBoxAsync(MessageBoxIconType::WarningIcon,
                                          "Unable to load wavetable", error.what());
    postPluginAnnouncement("Unable to load wavetable",
                                             AccessibilityHandler::AnnouncementPriority::high);
  }
}

void SynthEditor::saveWavetableFile(int oscillator) {
  if (!isPositiveAndBelow(oscillator, vital::kNumOscillators))
    return;

  auto* creator = synth_.getWavetableCreator(oscillator);
  if (creator == nullptr) {
    postPluginAnnouncement("No wavetable available", AccessibilityHandler::AnnouncementPriority::high);
    return;
  }

  String name = String(creator->getName()).trim();
  if (name.isEmpty())
    name = "Oscillator " + String(oscillator + 1) + " Wavetable";
  name = name.removeCharacters("\\/:*?\"<>|");
  if (name.isEmpty())
    name = "Wavetable";

  wavetable_chooser_ = std::make_unique<FileChooser>("Export Atlas wavetable",
                                                     LoadSave::getUserWavetableDirectory().getChildFile(name),
                                                     String("*.") + vital::kWavetableExtension);
  wavetable_chooser_->launchAsync(FileBrowserComponent::saveMode | FileBrowserComponent::canSelectFiles |
                                      FileBrowserComponent::warnAboutOverwriting,
                                  [this, oscillator](const FileChooser& chooser) {
    const File file = chooser.getResult();
    if (file != File()) {
      if (auto* creator = synth_.getWavetableCreator(oscillator)) {
        const File destination = file.withFileExtension(vital::kWavetableExtension);
        const bool saved = destination.replaceWithText(String(creator->stateToJson().dump(2)));
        if (saved)
          postPluginAnnouncement("Exported wavetable " + destination.getFileNameWithoutExtension(),
                                                 AccessibilityHandler::AnnouncementPriority::high);
        else
          NativeMessageBox::showMessageBoxAsync(MessageBoxIconType::WarningIcon, "Unable to export wavetable",
                                                "The wavetable file could not be written.");
      }
    }
    wavetable_chooser_.reset();
  });
}

int SynthEditor::wavetableFrameCount(int oscillator) const {
  if (!isPositiveAndBelow(oscillator, vital::kNumOscillators))
    return 1;

  if (auto* creator = synth_.getWavetableCreator(oscillator)) {
    if (auto* wavetable = creator->getWavetable())
      return jmax(1, wavetable->numFrames());
  }
  return 1;
}

String SynthEditor::wavetableEditorSummary(int oscillator) const {
  if (!isPositiveAndBelow(oscillator, vital::kNumOscillators))
    return "Wavetable editor";

  String name = "unknown";
  if (auto* creator = synth_.getWavetableCreator(oscillator)) {
    if (!creator->getName().empty())
      name = String(creator->getName());
  }

  return "Wavetable editor, " + name + ", " + String(wavetableFrameCount(oscillator)) + " frames";
}

void SynthEditor::setWavetableEditorVisible(int oscillator, bool visible) {
  if (!isPositiveAndBelow(oscillator, vital::kNumOscillators))
    return;
  if (wavetable_editor_visible_[oscillator] == visible)
    return;

  const String parameter_id = focusedParameterId();
  const String focused_title = focusedAccessibleTitle();
  String restore_title = focused_title;
  if (focused_title == "Show wavetable editor" && visible)
    restore_title = "Hide wavetable editor";
  else if (focused_title == "Hide wavetable editor" && !visible)
    restore_title = "Show wavetable editor";

  wavetable_editor_visible_[oscillator] = visible;
  const String section = "Oscillator " + String(oscillator + 1);
  Component::SafePointer<SynthEditor> safe_this(this);
  MessageManager::callAsync([safe_this, parameter_id, restore_title, section, visible] {
    if (safe_this == nullptr)
      return;

    if (safe_this->show_all_sections_)
      safe_this->showAllSections(false);
    else
      safe_this->selectSectionByName(section, false);
    safe_this->restoreFocusAfterRebuild(parameter_id, nullptr, restore_title, section);

    postPluginAnnouncement(visible ? "Wavetable editor shown" : "Wavetable editor hidden",
                                           AccessibilityHandler::AnnouncementPriority::medium);
  });
}

void SynthEditor::applyWavetableHarmonicEdit(int oscillator, int frame, bool allFrames,
                                             int harmonic, float levelPercent, float phaseDegrees) {
  if (!isPositiveAndBelow(oscillator, vital::kNumOscillators))
    return;

  auto* creator = synth_.getWavetableCreator(oscillator);
  auto* source = ensureEditableWaveSource(creator);
  if (creator == nullptr || source == nullptr || source->numFrames() == 0)
    return;

  harmonic = jlimit(1, vital::WaveFrame::kNumRealComplex - 2, harmonic);
  const int start = allFrames ? 0 : jlimit(0, source->numFrames() - 1, frame);
  const int end = allFrames ? source->numFrames() - 1 : start;
  const float amplitude = jlimit(0.0f, 4.0f, levelPercent / 100.0f) *
                          (vital::WaveFrame::kWaveformSize * 0.5f);
  const float phase = phaseDegrees * vital::kPi / 180.0f;

  synth_.pauseProcessing(true);
  for (int i = start; i <= end; ++i) {
    auto* wave_frame = source->getKeyframe(i)->wave_frame();
    wave_frame->frequency_domain[harmonic] = std::polar(amplitude, phase);
    wave_frame->toTimeDomain();
    wave_frame->normalize(true);
    wave_frame->toFrequencyDomain();
  }
  creator->render();
  synth_.pauseProcessing(false);

  postPluginAnnouncement("Set harmonic " + String(harmonic) +
                                         (allFrames ? " on all frames" : " on frame " + String(start + 1)),
                                         AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::clearWavetableHarmonic(int oscillator, int frame, bool allFrames, int harmonic) {
  if (!isPositiveAndBelow(oscillator, vital::kNumOscillators))
    return;

  auto* creator = synth_.getWavetableCreator(oscillator);
  auto* source = ensureEditableWaveSource(creator);
  if (creator == nullptr || source == nullptr || source->numFrames() == 0)
    return;

  harmonic = jlimit(1, vital::WaveFrame::kNumRealComplex - 2, harmonic);
  const int start = allFrames ? 0 : jlimit(0, source->numFrames() - 1, frame);
  const int end = allFrames ? source->numFrames() - 1 : start;

  synth_.pauseProcessing(true);
  for (int i = start; i <= end; ++i) {
    auto* wave_frame = source->getKeyframe(i)->wave_frame();
    wave_frame->frequency_domain[harmonic] = 0.0f;
    wave_frame->toTimeDomain();
    wave_frame->toFrequencyDomain();
  }
  creator->render();
  synth_.pauseProcessing(false);

  postPluginAnnouncement("Cleared harmonic " + String(harmonic) +
                                         (allFrames ? " on all frames" : " on frame " + String(start + 1)),
                                         AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::normalizeWavetableFrame(int oscillator, int frame, bool allFrames) {
  if (!isPositiveAndBelow(oscillator, vital::kNumOscillators))
    return;

  auto* creator = synth_.getWavetableCreator(oscillator);
  auto* source = ensureEditableWaveSource(creator);
  if (creator == nullptr || source == nullptr || source->numFrames() == 0)
    return;

  const int start = allFrames ? 0 : jlimit(0, source->numFrames() - 1, frame);
  const int end = allFrames ? source->numFrames() - 1 : start;

  synth_.pauseProcessing(true);
  for (int i = start; i <= end; ++i) {
    auto* wave_frame = source->getKeyframe(i)->wave_frame();
    wave_frame->normalize(true);
    wave_frame->toFrequencyDomain();
  }
  creator->render();
  synth_.pauseProcessing(false);

  postPluginAnnouncement(allFrames ? "Normalized all wavetable frames" :
                                                     "Normalized wavetable frame " + String(start + 1),
                                         AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::removeWavetableFundamental(int oscillator, int frame, bool allFrames) {
  clearWavetableHarmonic(oscillator, frame, allFrames, 1);
}

void SynthEditor::refreshWavetableBrowserCache() {
  const auto roots = browserRootsWithAdditionalFolders(LoadSave::kWavetableFolderName,
                                                       LoadSave::kAdditionalWavetableFoldersName);
  refreshBrowserFileCache(cached_wavetable_browser_files_, roots, vital::kWavetableExtensionsList);
  wavetable_browser_cache_valid_ = true;
}

void SynthEditor::refreshSampleBrowserCache() {
  const auto roots = browserRootsWithAdditionalFolders(LoadSave::kSampleFolderName,
                                                       LoadSave::kAdditionalSampleFoldersName);
  refreshBrowserFileCache(cached_sample_browser_files_, roots, vital::kSampleExtensionsList);
  sample_browser_cache_valid_ = true;
}

void SynthEditor::primeBrowserFileCaches() {
  if (wavetable_browser_cache_valid_ && sample_browser_cache_valid_)
    return;

  const auto wavetable_roots = browserRootsWithAdditionalFolders(LoadSave::kWavetableFolderName,
                                                                 LoadSave::kAdditionalWavetableFoldersName);
  const auto sample_roots = browserRootsWithAdditionalFolders(LoadSave::kSampleFolderName,
                                                              LoadSave::kAdditionalSampleFoldersName);
  Thread::launch([safe_this = Component::SafePointer<SynthEditor>(this),
                  wavetable_roots, sample_roots] {
    std::vector<File> wavetable_files;
    std::vector<File> sample_files;
    refreshBrowserFileCache(wavetable_files, wavetable_roots, vital::kWavetableExtensionsList);
    refreshBrowserFileCache(sample_files, sample_roots, vital::kSampleExtensionsList);
    MessageManager::callAsync([safe_this,
                               wavetable_files = std::move(wavetable_files),
                               sample_files = std::move(sample_files)]() mutable {
      if (safe_this == nullptr)
        return;
      if (!safe_this->wavetable_browser_cache_valid_) {
        safe_this->cached_wavetable_browser_files_ = std::move(wavetable_files);
        safe_this->wavetable_browser_cache_valid_ = true;
      }
      if (!safe_this->sample_browser_cache_valid_) {
        safe_this->cached_sample_browser_files_ = std::move(sample_files);
        safe_this->sample_browser_cache_valid_ = true;
      }
    });
  });
}

void SynthEditor::loadShiftedWavetable(int oscillator, int direction) {
  if (!isPositiveAndBelow(oscillator, vital::kNumOscillators))
    return;

  auto* creator = synth_.getWavetableCreator(oscillator);
  const File current_file(creator != nullptr ? creator->getLastFileLoaded() : std::string());
  if (!wavetable_browser_cache_valid_)
    refreshWavetableBrowserCache();

  File wavetable_file = shiftedBrowserFile(cached_wavetable_browser_files_, current_file, direction);
  if (wavetable_file.existsAsFile()) {
    active_oscillator_ = oscillator;
    loadWavetableFile(wavetable_file);
    return;
  }

  postPluginAnnouncement("No wavetable file found",
                                         AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::showWavetableBrowserMenu(int oscillator, Component& target) {
  if (!isPositiveAndBelow(oscillator, vital::kNumOscillators))
    return;

  const auto roots = browserRootsWithAdditionalFolders(LoadSave::kWavetableFolderName,
                                                       LoadSave::kAdditionalWavetableFoldersName);
  if (!wavetable_browser_cache_valid_)
    refreshWavetableBrowserCache();

  auto choices = std::make_shared<std::vector<File>>();
  PopupMenu menu = createFileBrowserMenu(roots, LoadSave::kWavetableFolderName,
                                         cached_wavetable_browser_files_, choices);
  menu.addSeparator();
  menu.addItem(kBrowserRescanMenuId, "Rescan wavetable folders");
  Component::SafePointer<Component> target_pointer(&target);
  menu.showMenuAsync(PopupMenu::Options().withTargetComponent(&target),
                     [this, oscillator, choices, target_pointer](int result) {
    if (result == kBrowserRescanMenuId) {
      cached_wavetable_browser_files_.clear();
      wavetable_browser_cache_valid_ = false;
      if (target_pointer != nullptr)
        showWavetableBrowserMenu(oscillator, *target_pointer);
      return;
    }
    if (!isPositiveAndBelow(result - 1, static_cast<int>(choices->size())))
      return;

    active_oscillator_ = oscillator;
    loadWavetableFile((*choices)[static_cast<size_t>(result - 1)]);
  });
}

void SynthEditor::chooseSampleFile(bool granular) {
  sample_chooser_ = std::make_unique<FileChooser>(granular ? "Load granular sample" : "Load audio sample",
                                                  File(), vital::kSampleExtensionsList);
  sample_chooser_->launchAsync(FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles,
                               [this, granular](const FileChooser& chooser) {
    const auto file = chooser.getResult();
    if (file.existsAsFile())
      loadSampleFile(file, granular);
    sample_chooser_.reset();
  });
}

void SynthEditor::loadSampleFile(const File& file, bool granular) {
  static constexpr int kMaxFileSamples = 17640000;

  try {
    AudioFormatManager format_manager;
    format_manager.registerBasicFormats();
    std::unique_ptr<AudioFormatReader> format_reader(format_manager.createReaderFor(file));
    if (format_reader == nullptr)
      throw std::runtime_error("Unable to decode audio file");

    const int num_samples = static_cast<int>(std::min<long long>(format_reader->lengthInSamples, kMaxFileSamples));
    if (num_samples <= 0)
      throw std::runtime_error("Audio file is empty");

    AudioSampleBuffer sample_buffer;
    sample_buffer.setSize(static_cast<int>(format_reader->numChannels), num_samples);
    format_reader->read(&sample_buffer, 0, num_samples, 0, true, true);

    auto* sample = granular ? synth_.getGranularSample() : synth_.getSample();
    if (sample == nullptr)
      throw std::runtime_error("Sample engine is unavailable");

    synth_.pauseProcessing(true);
    if (sample_buffer.getNumChannels() > 1)
      sample->loadSample(sample_buffer.getReadPointer(0), sample_buffer.getReadPointer(1),
                         num_samples, static_cast<int>(format_reader->sampleRate));
    else
      sample->loadSample(sample_buffer.getReadPointer(0), num_samples, static_cast<int>(format_reader->sampleRate));
    sample->setName(file.getFileNameWithoutExtension().toStdString());
    sample->setLastBrowsedFile(file.getFullPathName().toStdString());
    synth_.pauseProcessing(false);

    if (granular)
      setParameterEngineValue("granular_on", 1.0f);

    postPluginAnnouncement("Loaded sample " + file.getFileNameWithoutExtension(),
                                           AccessibilityHandler::AnnouncementPriority::high);
  }
  catch (const std::exception& error) {
    synth_.pauseProcessing(false);
    NativeMessageBox::showMessageBoxAsync(MessageBoxIconType::WarningIcon,
                                          "Unable to load sample", error.what());
    postPluginAnnouncement("Unable to load sample",
                                           AccessibilityHandler::AnnouncementPriority::high);
  }
}

void SynthEditor::loadShiftedSample(int direction, bool granular) {
  auto* sample = granular ? synth_.getGranularSample() : synth_.getSample();
  const File current_file(sample != nullptr ? sample->getLastBrowsedFile() : std::string());
  if (!sample_browser_cache_valid_)
    refreshSampleBrowserCache();

  File sample_file = shiftedBrowserFile(cached_sample_browser_files_, current_file, direction);
  if (sample_file.existsAsFile()) {
    loadSampleFile(sample_file, granular);
    return;
  }

  postPluginAnnouncement("No sample file found",
                                         AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::showSampleBrowserMenu(Component& target, bool granular) {
  const auto roots = browserRootsWithAdditionalFolders(LoadSave::kSampleFolderName,
                                                       LoadSave::kAdditionalSampleFoldersName);
  if (!sample_browser_cache_valid_)
    refreshSampleBrowserCache();

  auto choices = std::make_shared<std::vector<File>>();
  PopupMenu menu = createFileBrowserMenu(roots, LoadSave::kSampleFolderName,
                                         cached_sample_browser_files_, choices);
  menu.addSeparator();
  menu.addItem(kBrowserRescanMenuId, "Rescan sample folders");
  Component::SafePointer<Component> target_pointer(&target);
  menu.showMenuAsync(PopupMenu::Options().withTargetComponent(&target),
                     [this, choices, granular, target_pointer](int result) {
    if (result == kBrowserRescanMenuId) {
      cached_sample_browser_files_.clear();
      sample_browser_cache_valid_ = false;
      if (target_pointer != nullptr)
        showSampleBrowserMenu(*target_pointer, granular);
      return;
    }
    if (!isPositiveAndBelow(result - 1, static_cast<int>(choices->size())))
      return;

    loadSampleFile((*choices)[static_cast<size_t>(result - 1)], granular);
  });
}

void SynthEditor::chooseLfoPresetFile(int lfoIndex) {
  if (!isPositiveAndBelow(lfoIndex, vital::kNumLfos))
    return;

  lfo_chooser_ = std::make_unique<FileChooser>("Import Atlas LFO preset",
                                               LoadSave::getUserLfoDirectory(), vital::kLfoExtensionsList);
  lfo_chooser_->launchAsync(FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles,
                            [this, lfoIndex](const FileChooser& chooser) {
    const File file = chooser.getResult();
    if (file.existsAsFile())
      loadLfoPresetFile(lfoIndex, file);
    lfo_chooser_.reset();
  });
}

void SynthEditor::loadLfoPresetFile(int lfoIndex, const File& file) {
  if (!isPositiveAndBelow(lfoIndex, vital::kNumLfos) || !file.existsAsFile())
    return;

  json data = json::parse(file.loadFileAsString().toStdString(), nullptr, false);
  if (data.is_discarded() || !LineGenerator::isValidJson(data)) {
    NativeMessageBox::showMessageBoxAsync(MessageBoxIconType::WarningIcon, "Unable to import LFO preset",
                                          "The selected file is not a valid Atlas LFO preset.");
    postPluginAnnouncement("Unable to import LFO preset", AccessibilityHandler::AnnouncementPriority::high);
    return;
  }

  if (auto* generator = lfoGeneratorForIndex(lfoIndex)) {
    synth_.pauseProcessing(true);
    generator->jsonToState(data);
    generator->setLastBrowsedFile(file.getFullPathName().toStdString());
    synth_.pauseProcessing(false);
    if (active_lfo_index_ == lfoIndex)
      refreshLfoMsegControls();
    postPluginAnnouncement("Loaded LFO preset " + file.getFileNameWithoutExtension() +
                                           " into LFO " + String(lfoIndex + 1),
                                           AccessibilityHandler::AnnouncementPriority::high);
  }
}

void SynthEditor::saveLfoPresetFile(int lfoIndex) {
  if (!isPositiveAndBelow(lfoIndex, vital::kNumLfos))
    return;

  auto* generator = lfoGeneratorForIndex(lfoIndex);
  if (generator == nullptr)
    return;

  String name = String(generator->getName()).trim();
  if (name.isEmpty())
    name = "LFO " + String(lfoIndex + 1);
  name = name.removeCharacters("\\/:*?\"<>|");
  if (name.isEmpty())
    name = "LFO";

  lfo_chooser_ = std::make_unique<FileChooser>("Save Atlas LFO preset",
                                               LoadSave::getUserLfoDirectory().getChildFile(name),
                                               String("*.") + vital::kLfoExtension);
  lfo_chooser_->launchAsync(FileBrowserComponent::saveMode | FileBrowserComponent::canSelectFiles |
                                FileBrowserComponent::warnAboutOverwriting,
                            [this, lfoIndex](const FileChooser& chooser) {
    const File file = chooser.getResult();
    if (file != File()) {
      if (auto* generator = lfoGeneratorForIndex(lfoIndex)) {
        const File destination = file.withFileExtension(vital::kLfoExtension);
        const bool saved = destination.replaceWithText(String(generator->stateToJson().dump(2)));
        if (saved)
          postPluginAnnouncement("Saved LFO preset " + destination.getFileNameWithoutExtension(),
                                                 AccessibilityHandler::AnnouncementPriority::high);
        else
          NativeMessageBox::showMessageBoxAsync(MessageBoxIconType::WarningIcon, "Unable to save LFO preset",
                                                "The LFO preset could not be written.");
      }
    }
    lfo_chooser_.reset();
  });
}

void SynthEditor::chooseEffectPresetFile(const String& sectionName) {
  if (effectIdForSection(sectionName).isEmpty())
    return;

  fx_chooser_ = std::make_unique<FileChooser>("Import Atlas FX preset",
                                              LoadSave::getUserFxDirectory(),
                                              String("*.") + vital::kFxExtension);
  fx_chooser_->launchAsync(FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles,
                           [this, sectionName](const FileChooser& chooser) {
    const File file = chooser.getResult();
    if (file.existsAsFile())
      loadEffectPresetFile(sectionName, file);
    fx_chooser_.reset();
  });
}

void SynthEditor::loadEffectPresetFile(const String& sectionName, const File& file) {
  const String effect_id = effectIdForSection(sectionName);
  if (effect_id.isEmpty() || !file.existsAsFile())
    return;

  json data = json::parse(file.loadFileAsString().toStdString(), nullptr, false);
  if (data.is_discarded() || !data.count("parameters") || !data["parameters"].is_object()) {
    NativeMessageBox::showMessageBoxAsync(MessageBoxIconType::WarningIcon, "Unable to import FX preset",
                                          "The selected file is not a valid Atlas FX preset.");
    postPluginAnnouncement("Unable to import FX preset", AccessibilityHandler::AnnouncementPriority::high);
    return;
  }

  if (data.count("effect") && data["effect"].is_string() &&
      String(data["effect"].get<std::string>()) != effect_id) {
    NativeMessageBox::showMessageBoxAsync(MessageBoxIconType::WarningIcon, "Wrong FX preset type",
                                          "This preset was saved for " +
                                          effectPresetDisplayName(String(data["effect"].get<std::string>())) +
                                          ", not " + accessibleSectionTitle(sectionName) + ".");
    return;
  }

  const String chain_prefix = effectChainPrefixForSection(sectionName);
  int changed = 0;
  for (auto it = data["parameters"].begin(); it != data["parameters"].end(); ++it) {
    if (!it.value().is_number())
      continue;
    const String suffix = it.key();
    if (!suffix.startsWith(effect_id + "_"))
      continue;
    const String target_id = chain_prefix + suffix;
    if (parameterBridge(target_id) == nullptr)
      continue;
    setParameterEngineValue(target_id, it.value().get<float>());
    ++changed;
  }

  if (changed == 0) {
    postPluginAnnouncement("No matching FX parameters found", AccessibilityHandler::AnnouncementPriority::high);
    return;
  }

  timerCallback();
  postPluginAnnouncement("Loaded " + accessibleSectionTitle(sectionName) + " FX preset " +
                                         file.getFileNameWithoutExtension(),
                                         AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::saveEffectPresetFile(const String& sectionName) {
  const String effect_id = effectIdForSection(sectionName);
  if (effect_id.isEmpty())
    return;

  const String chain_prefix = effectChainPrefixForSection(sectionName);
  json parameters = json::object();
  for (const auto& parameter : parameters_by_id_) {
    const String id = parameter.first;
    if (!id.startsWith(chain_prefix + effect_id + "_"))
      continue;
    if (parameter.second == nullptr)
      continue;
    const String suffix = id.fromFirstOccurrenceOf(chain_prefix, false, false);
    parameters[suffix.toStdString()] = parameter.second->convertToEngineValue(parameter.second->getValue());
  }

  if (parameters.empty()) {
    postPluginAnnouncement("No FX parameters found", AccessibilityHandler::AnnouncementPriority::high);
    return;
  }

  String name = accessibleSectionTitle(sectionName).removeCharacters("\\/:*?\"<>|");
  if (name.isEmpty())
    name = "FX Preset";
  fx_chooser_ = std::make_unique<FileChooser>("Save Atlas FX preset",
                                              LoadSave::getUserFxDirectory().getChildFile(name),
                                              String("*.") + vital::kFxExtension);
  fx_chooser_->launchAsync(FileBrowserComponent::saveMode | FileBrowserComponent::canSelectFiles |
                               FileBrowserComponent::warnAboutOverwriting,
                           [this, effect_id, sectionName, parameters](const FileChooser& chooser) {
    const File file = chooser.getResult();
    if (file != File()) {
      json data;
      data["type"] = "Atlas FX Preset";
      data["version"] = 1;
      data["effect"] = effect_id.toStdString();
      data["name"] = accessibleSectionTitle(sectionName).toStdString();
      data["parameters"] = parameters;
      const File destination = file.withFileExtension(vital::kFxExtension);
      const bool saved = destination.replaceWithText(String(data.dump(2)));
      if (saved)
        postPluginAnnouncement("Saved FX preset " + destination.getFileNameWithoutExtension(),
                                               AccessibilityHandler::AnnouncementPriority::high);
      else
        NativeMessageBox::showMessageBoxAsync(MessageBoxIconType::WarningIcon, "Unable to save FX preset",
                                              "The FX preset could not be written.");
    }
    fx_chooser_.reset();
  });
}

void SynthEditor::moveToSection(int direction) {
  const int count = section_names_.size();
  if (count == 0)
    return;
  int current = section_names_.indexOf(last_section_name);
  if (current < 0)
    current = 0;
  const int next = (current + direction + count) % count;
  selectSectionByName(section_names_[next], true);
  section_selector_.grabKeyboardFocus();
}

bool SynthEditor::focusSectionShortcut(const String& section) {
  if (!section_names_.contains(section))
    return false;

  selectSectionByName(section, true);
  return true;
}

bool SynthEditor::focusGroupShortcut(const String& group, const String& fallbackSection) {
  if (!group_names_.contains(group)) {
    if (fallbackSection.isNotEmpty())
      return focusSectionShortcut(fallbackSection);
    return false;
  }

  if (show_all_sections_) {
    if (row_focus_order_.empty())
      showAllSections(false);
    for (auto* component : row_focus_order_) {
      if (component != nullptr && component->getTitle() == group) {
        ensureComponentVisible(component);
        component->grabKeyboardFocus();
        const int group_index = group_names_.indexOf(group);
        if (group_index >= 0)
          group_selector_.setSelectedItemIndex(group_index, dontSendNotification);
        postPluginAnnouncement(group, AccessibilityHandler::AnnouncementPriority::high);
        return true;
      }
    }
  }

  const auto found = group_sections_.find(group);
  if (found == group_sections_.end() || found->second.isEmpty())
    return false;

  selectSectionByName(found->second[0], true);
  return true;
}

bool SynthEditor::focusShortcutTarget(const KeyPress& key) {
  if (dynamic_cast<TextEditor*>(Component::getCurrentlyFocusedComponent()) != nullptr)
    return false;

  const juce_wchar character = CharacterFunctions::toLowerCase(key.getTextCharacter());
  const ModifierKeys modifiers = key.getModifiers();

  if (modifiers.isCtrlDown() && !modifiers.isCommandDown() && !modifiers.isAltDown()) {
    if (key.getKeyCode() == '1') return focusGroupShortcut("Bus 1");
    if (key.getKeyCode() == '2') return focusGroupShortcut("Bus 2");
    if (key.getKeyCode() == '3') return focusGroupShortcut("Bus 3");
  }

  // if (modifiers.isAltDown() && !modifiers.isCommandDown() && !modifiers.isCtrlDown() && character == 'm')
  //   return focusGroupShortcut("Modulation", "Modulation routing");

  if (modifiers.isAnyModifierKeyDown())
    return false;

  switch (character) {
    case 'o': return focusGroupShortcut("Oscillators");
    case 'x': return focusGroupShortcut("Mixer and routing", kSignalRoutingSection);
    case 'f': return focusGroupShortcut("Filters");
    case 'e': return focusGroupShortcut("Envelopes");
    case 'l': return focusGroupShortcut("LFOs");
    case 'm': return focusSectionShortcut("Modulation routing");
    case 'p': return focusGroupShortcut("Effects", kEffectsChainSection);
    case 'r': return focusSectionShortcut(kSignalRoutingSection);
    case 'z': return focusGroupShortcut("Zones");
    case 'g': return focusSectionShortcut("Master and global");
    default: break;
  }

  if (key.getKeyCode() >= '1' && key.getKeyCode() <= '9') {
    const int index = key.getKeyCode() - '1';
    const String group = group_selector_.getText();
    const auto found = group_sections_.find(group);
    if (found != group_sections_.end() && isPositiveAndBelow(index, found->second.size())) {
      selectSectionByName(found->second[index], true);
      return true;
    }
  }

  return false;
}

namespace {
  // Surge-style single-octave QWERTY layout: keyboard key code -> semitone offset from the
  // base note. Letters use their uppercase ASCII code (matching KeyPress::isKeyCurrentlyDown);
  // the four punctuation keys extend the top of the range.
  struct QwertyKeyMapping {
    int key_code;
    int offset;
  };

  const QwertyKeyMapping kQwertyKeys[] = {
    { 'A', 0 },  { 'W', 1 },  { 'S', 2 },  { 'E', 3 },  { 'D', 4 },  { 'F', 5 },
    { 'T', 6 },  { 'G', 7 },  { 'Y', 8 },  { 'H', 9 },  { 'U', 10 }, { 'J', 11 },
    { 'K', 12 }, { 'O', 13 }, { 'L', 14 }, { 'P', 15 },
    { ';', 16 }, { '[', 17 }, { '\'', 18 }, { ']', 19 },
  };

  bool isQwertyNoteKeyCode(int key_code) {
    for (const QwertyKeyMapping& mapping : kQwertyKeys) {
      if (mapping.key_code == key_code)
        return true;
    }
    return false;
  }
} // namespace

bool SynthEditor::isQwertyKeyboardActive() const {
  return qwerty_keyboard_on_ &&
         dynamic_cast<TextEditor*>(Component::getCurrentlyFocusedComponent()) == nullptr;
}

void SynthEditor::toggleQwertyKeyboard() {
  qwerty_keyboard_on_ = !qwerty_keyboard_on_;
  if (!qwerty_keyboard_on_)
    allQwertyNotesOff();
  postPluginAnnouncement(qwerty_keyboard_on_ ? "QWERTY keyboard on" : "QWERTY keyboard off",
                         AccessibilityHandler::AnnouncementPriority::high);
}

void SynthEditor::updateQwertyNotes() {
  MidiKeyboardState* state = getSynth()->getKeyboardState();
  if (state == nullptr)
    return;

  const float velocity = qwerty_velocity_ / 127.0f;
  for (const QwertyKeyMapping& mapping : kQwertyKeys) {
    const int note = 12 * qwerty_octave_ + mapping.offset;
    if (note < 0 || note > 127)
      continue;

    const bool key_down = KeyPress::isKeyCurrentlyDown(mapping.key_code);
    const bool tracked = qwerty_notes_down_.count(note) != 0;
    if (key_down && !tracked) {
      state->noteOn(1, note, velocity);
      qwerty_notes_down_.insert(note);
    }
    else if (!key_down && tracked) {
      state->noteOff(1, note, 0.0f);
      qwerty_notes_down_.erase(note);
    }
  }
}

void SynthEditor::allQwertyNotesOff() {
  MidiKeyboardState* state = getSynth()->getKeyboardState();
  if (state != nullptr) {
    for (int note : qwerty_notes_down_)
      state->noteOff(1, note, 0.0f);
  }
  qwerty_notes_down_.clear();
}

bool SynthEditor::keyStateChanged(bool isKeyDown) {
  if (!isQwertyKeyboardActive())
    return false;

  updateQwertyNotes();
  return true;
}

bool SynthEditor::keyPressed(const KeyPress& key) {
  if ((modulation_amount_prompt_visible_ || parameter_value_prompt_visible_) &&
      key.getKeyCode() == KeyPress::escapeKey) {
    cancelInlineTextPrompt();
    return true;
  }
  {
    const ModifierKeys modifiers = key.getModifiers();
    if (modifiers.isAltDown() && !modifiers.isCommandDown() && !modifiers.isCtrlDown() &&
        key.getKeyCode() == 'K') {
      toggleQwertyKeyboard();
      return true;
    }
  }
  if (isQwertyKeyboardActive()) {
    const ModifierKeys modifiers = key.getModifiers();
    const int code = key.getKeyCode();
    if (code == 'X' || code == 'C') {
      allQwertyNotesOff();
      qwerty_octave_ = jlimit(0, 9, qwerty_octave_ + (code == 'C' ? 1 : -1));
      postPluginAnnouncement("Octave " + String(qwerty_octave_),
                             AccessibilityHandler::AnnouncementPriority::high);
      return true;
    }
    if (code == 'V' || code == 'B') {
      int step = modifiers.isShiftDown() ? 1 : 10;
      if (code == 'V')
        step = -step;
      qwerty_velocity_ = jlimit(1, 127, qwerty_velocity_ + step);
      postPluginAnnouncement("Velocity " + String(qwerty_velocity_),
                             AccessibilityHandler::AnnouncementPriority::high);
      return true;
    }
    // Consume mapped note keys so single-letter navigation is suppressed; the note on/off
    // itself is generated in keyStateChanged.
    if (isQwertyNoteKeyCode(code))
      return true;
  }
  if ((key.getKeyCode() == KeyPress::returnKey || key.getKeyCode() == KeyPress::spaceKey) &&
      preset_controls_visible_ && preset_selector_.hasKeyboardFocus(true)) {
    loadSelectedPreset();
    return true;
  }
  if (lfo_mseg_controls_visible_ && handleLfoMsegShortcut(key))
    return true;
  if (!isQwertyKeyboardActive() && focusShortcutTarget(key))
    return true;
  if (effect_chain_controls_visible_) {
    if (isEffectMoveEarlierKey(key)) {
      moveSelectedEffect(-1);
      return true;
    }
    if (isEffectMoveLaterKey(key)) {
      moveSelectedEffect(1);
      return true;
    }
  }
  if (!isQwertyKeyboardActive()) {
    if (key.getTextCharacter() == ',') {
      moveToSection(-1);
      return true;
    }
    if (key.getTextCharacter() == '.') {
      moveToSection(1);
      return true;
    }
  }
  return AudioProcessorEditor::keyPressed(key);
}

void SynthEditor::timerCallback() {
  if (refreshFilterRowsIfNeeded())
    return;
  if (refreshZoneCrossfadeRowIfNeeded())
    return;
  if (routing_controls_visible_)
    refreshRoutingControls();
  for (auto& row : rows_)
    row->refresh();
  if (modulation_controls_visible_)
    refreshVisibleModulationLabels();
  if (active_oscillator_ >= 0 && !updating_oscillator_controls_)
    refreshOscillatorDirectControls();
}

void SynthEditor::updateFullGui() {
  timerCallback();
  synth_.updateHostDisplay();
}

void SynthEditor::notifyMidiLearned(const std::string& name, int midi_id) {
  postPluginAnnouncement(modulationDestinationLabelForId(String(name)) + " MIDI learn set to " +
                             midiControlName(midi_id),
                         AccessibilityHandler::AnnouncementPriority::high);
}
