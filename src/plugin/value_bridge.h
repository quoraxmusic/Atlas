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

#pragma once

#include "JuceHeader.h"
#include "value.h"
#include "synth_parameters.h"

class ValueBridge : public AudioProcessorParameter {
  public:
    class Listener {
      public:
        virtual ~Listener() { }
        virtual void parameterChanged(std::string name, vital::mono_float value) = 0;
    };

    ValueBridge() = delete;

    ValueBridge(std::string name, vital::Value* value) :
        AudioProcessorParameter(std::max(1, vital::Parameters::getDetails(name).version_added)),
        name_(name), value_(value), listener_(nullptr),
        source_changed_(false) {
      details_ = vital::Parameters::getDetails(name);
      span_ = details_.max - details_.min;
      if (details_.value_scale == vital::ValueDetails::kIndexed)
        span_ = std::round(span_);
    }

    float getValue() const override {
      return convertToPluginValue(value_->value());
    }

    void setValue(float value) override {
      if (listener_ && !source_changed_) {
        source_changed_ = true;
        vital::mono_float synth_value = convertToEngineValue(value);
        listener_->parameterChanged(name_.toStdString(), synth_value);
        source_changed_ = false;
      }
    }

    void setListener(Listener* listener) {
      listener_ = listener;
    }

    float getDefaultValue() const override {
      return convertToPluginValue(details_.default_value);
    }

    String getName(int maximumStringLength) const override {
      return String(details_.display_name).substring(0, maximumStringLength);
    }

    String getLabel() const override {
      return "";
    }

    String getText(float value, int maximumStringLength) const override {
      float adjusted = convertToEngineValue(value);
      String result = "";
      if (details_.string_lookup)
        result = details_.string_lookup[std::max<int>(0, std::min(adjusted, details_.max))];
      else {
        float display_value = details_.display_multiply * skewValue(adjusted) + details_.post_offset;
        result = formatDisplayValue(display_value);
      }
      return result.substring(0, maximumStringLength).trim();
    }

    float getValueForText(const String &text) const override {
      const String trimmed = text.trim();
      if (details_.string_lookup) {
        for (int i = 0; i <= static_cast<int>(span_); ++i) {
          if (trimmed.equalsIgnoreCase(String(details_.string_lookup[i])))
            return convertToPluginValue(details_.min + i);
        }
      }

      float display_value = trimmed.getFloatValue();
      if (trimmed.toLowerCase().contains("ms"))
        display_value *= 0.001f;
      else if (usesPercentDisplay() && trimmed.contains("%") && details_.display_multiply == 1.0f)
        display_value /= 100.0f;
      else if (usesPercentDisplay() && details_.display_multiply == 1.0f && std::abs(display_value) > 1.0f)
        display_value /= 100.0f;

      float scaled_value = (display_value - details_.post_offset) /
                           (details_.display_multiply == 0.0f ? 1.0f : details_.display_multiply);
      if (usesPercentDisplay() && details_.display_multiply == 1.0f)
        scaled_value = display_value;

      float engine_value = unskewValue(scaled_value);
      return convertToPluginValue(std::max(details_.min, std::min(engine_value, details_.max)));
    }

    bool isAutomatable() const override {
      return true;
    }

    int getNumSteps() const override {
      if (isDiscrete())
        return 1 + (int)span_;
      return AudioProcessorParameter::getNumSteps();
    }

    bool isDiscrete() const override {
      static constexpr int kMaxIndexedSteps = 300;
      return details_.value_scale == vital::ValueDetails::kIndexed && span_ < kMaxIndexedSteps;
    }

    bool isBoolean() const override {
      return isDiscrete() && span_ == 1.0f;
    }

    const String& getParameterId() const { return name_; }
    const vital::ValueDetails& getDetails() const { return details_; }

    // Converts internal value to value from 0.0 to 1.0.
    float convertToPluginValue(vital::mono_float synth_value) const {
      return (synth_value - details_.min) / span_;
    }

    // Converts from value from 0.0 to 1.0 to internal engine value.
    float convertToEngineValue(vital::mono_float plugin_value) const {
      float value = plugin_value * span_ + details_.min;

      if (details_.value_scale == vital::ValueDetails::kIndexed)
        return std::round(value);

      return value;
    }

    void setValueNotifyHost(float new_value) {
      if (!source_changed_) {
        source_changed_ = true;
        setValueNotifyingHost(new_value);
        source_changed_ = false;
      }
    }

  private:
    float getSkewedValue() const {
      return skewValue(value_->value());
    }

    bool usesPercentDisplay() const {
      return details_.display_units == "%" || name_.endsWith("_mix") || name_.endsWith("_dry_wet");
    }

    static String compactFloat(float value, int decimals) {
      String result(value, decimals);
      if (result.containsChar('.')) {
        while (result.endsWithChar('0'))
          result = result.dropLastCharacters(1);
        if (result.endsWithChar('.'))
          result = result.dropLastCharacters(1);
      }
      return result;
    }

    String formatDisplayValue(float display_value) const {
      if (usesPercentDisplay()) {
        float percent = details_.display_units == "%" ? display_value : display_value * 100.0f;
        return String(static_cast<int>(std::round(percent))) + "%";
      }

      const String units(details_.display_units);
      const String trimmed_units = units.trim();
      if (trimmed_units == "secs") {
        if (std::abs(display_value) < 1.0f)
          return compactFloat(display_value * 1000.0f, std::abs(display_value) < 0.01f ? 2 : 1) + " ms";
        return compactFloat(display_value, display_value < 10.0f ? 2 : 1) + " secs";
      }

      if (trimmed_units == "dB")
        return compactFloat(display_value, 1) + " dB";
      if (trimmed_units == "semitones" || trimmed_units == "cents" || trimmed_units == "voices")
        return String(static_cast<int>(std::round(display_value))) + " " + trimmed_units;
      if (trimmed_units == "Hz")
        return compactFloat(display_value, std::abs(display_value) < 10.0f ? 2 : 1) + " Hz";

      const int decimals = std::abs(display_value) < 10.0f ? 3 : (std::abs(display_value) < 100.0f ? 2 : 1);
      return compactFloat(display_value, decimals) + units;
    }

    float skewValue(float value) const {
      switch (details_.value_scale) {
        case vital::ValueDetails::kQuadratic:
          return value * value;
        case vital::ValueDetails::kCubic:
          return value * value * value;
        case vital::ValueDetails::kQuartic:
          value *= value;
          return value * value;
        case vital::ValueDetails::kExponential:
          if (details_.display_invert)
            return 1.0f / powf(2.0f, value);
          return powf(2.0f, value);
        case vital::ValueDetails::kSquareRoot:
          return sqrtf(value);
        default:
          return value;
      }
    }

    float unskewValue(float value) const {
      switch (details_.value_scale) {
        case vital::ValueDetails::kQuadratic:
          return sqrtf(value);
        case vital::ValueDetails::kCubic:
          return powf(value, 1.0f / 3.0f);
        case vital::ValueDetails::kQuartic:
          return powf(value, 1.0f / 4.0f);
        case vital::ValueDetails::kSquareRoot:
          return value * value;
        case vital::ValueDetails::kExponential:
          if (details_.display_invert)
            return log2(1.0f / value);
          return log2(value);
        default:
          return value;
      }
    }

    String name_;
    vital::ValueDetails details_;
    vital::mono_float span_;
    vital::Value* value_;
    Listener* listener_;
    bool source_changed_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ValueBridge)
};
