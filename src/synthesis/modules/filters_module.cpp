/* Copyright 2013-2019 Matt Tytel
 *
 * vital is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * vital is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with vital.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "filters_module.h"
#include "filter_module.h"
#include "synth_constants.h"

namespace vital {

  FiltersModule::FiltersModule() : SynthModule(kNumInputs, kNumOutputs), filter_1_(nullptr), filter_2_(nullptr),
                                   filter_1_filter_input_(nullptr), filter_2_filter_input_(nullptr),
                                   filter_1_destination_(nullptr), filter_2_destination_(nullptr) {
    filter_1_input_ = std::make_shared<Output>();
    filter_2_input_ = std::make_shared<Output>();
  }

  void FiltersModule::init() {
    filter_1_filter_input_ = createBaseControl("filter_1_filter_input");
    filter_1_destination_ = createBaseControl("filter_1_destination");
    filter_1_ = new FilterModule("filter_1");
    addSubmodule(filter_1_);
    addProcessor(filter_1_);

    filter_1_->plug(filter_1_input_.get(), FilterModule::kAudio);
    filter_1_->useInput(input(kReset), FilterModule::kReset);
    filter_1_->useInput(input(kKeytrack), FilterModule::kKeytrack);
    filter_1_->useInput(input(kMidi), FilterModule::kMidi);

    filter_2_filter_input_ = createBaseControl("filter_2_filter_input");
    filter_2_destination_ = createBaseControl("filter_2_destination");
    filter_2_ = new FilterModule("filter_2");
    addSubmodule(filter_2_);
    addProcessor(filter_2_);

    filter_2_->plug(filter_2_input_.get(), FilterModule::kAudio);
    filter_2_->useInput(input(kReset), FilterModule::kReset);
    filter_2_->useInput(input(kKeytrack), FilterModule::kKeytrack);
    filter_2_->useInput(input(kMidi), FilterModule::kMidi);

    SynthModule::init();
  }

  void FiltersModule::processParallel(int num_samples) {
    filter_1_input_->buffer = input(kFilter1Input)->source->buffer;
    filter_2_input_->buffer = input(kFilter2Input)->source->buffer;

    getLocalProcessor(filter_1_)->process(num_samples);
    getLocalProcessor(filter_2_)->process(num_samples);

    routeFilterOutputs(num_samples);
  }

  void FiltersModule::processSerialForward(int num_samples) {
    filter_1_input_->buffer = input(kFilter1Input)->source->buffer;
    filter_2_input_->buffer = filter_2_input_->owned_buffer.get();

    getLocalProcessor(filter_1_)->process(num_samples);

    poly_float* filter_2_input_buffer = filter_2_input_->buffer;
    const poly_float* filter_1_output_buffer = filter_1_->output()->buffer;
    const poly_float* filter_2_straight_input = input(kFilter2Input)->source->buffer;

    for (int i = 0; i < num_samples; ++i)
      filter_2_input_buffer[i] = filter_1_output_buffer[i] + filter_2_straight_input[i];

    getLocalProcessor(filter_2_)->process(num_samples);
    routeFilterOutputs(num_samples);
  }

  void FiltersModule::processSerialBackward(int num_samples) {
    filter_1_input_->buffer = filter_1_input_->owned_buffer.get();
    filter_2_input_->buffer = input(kFilter2Input)->source->buffer;

    getLocalProcessor(filter_2_)->process(num_samples);

    poly_float* filter_1_input_buffer = filter_1_input_->buffer;
    const poly_float* filter_2_output_buffer = filter_2_->output()->buffer;
    const poly_float* filter_1_straight_input = input(kFilter1Input)->source->buffer;

    for (int i = 0; i < num_samples; ++i)
      filter_1_input_buffer[i] = filter_2_output_buffer[i] + filter_1_straight_input[i];

    getLocalProcessor(filter_1_)->process(num_samples);
    routeFilterOutputs(num_samples);
  }

  void FiltersModule::routeFilterOutputs(int num_samples) {
    poly_float* main_output = output(kMainOut)->buffer;
    poly_float* bus1_output = output(kBus1Out)->buffer;
    poly_float* bus2_output = output(kBus2Out)->buffer;
    poly_float* bus3_output = output(kBus3Out)->buffer;
    poly_float* direct_output = output(kDirectOut)->buffer;
    utils::zeroBuffer(main_output, num_samples);
    utils::zeroBuffer(bus1_output, num_samples);
    utils::zeroBuffer(bus2_output, num_samples);
    utils::zeroBuffer(bus3_output, num_samples);
    utils::zeroBuffer(direct_output, num_samples);

    auto route = [num_samples, main_output, bus1_output, bus2_output, bus3_output, direct_output](
                     const poly_float* source, int destination) {
      if (destination == constants::kBus1)
        utils::addBuffers(bus1_output, bus1_output, source, num_samples);
      else if (destination == constants::kBus2)
        utils::addBuffers(bus2_output, bus2_output, source, num_samples);
      else if (destination == constants::kBus3)
        utils::addBuffers(bus3_output, bus3_output, source, num_samples);
      else if (destination == constants::kDirectOut)
        utils::addBuffers(direct_output, direct_output, source, num_samples);
      else
        utils::addBuffers(main_output, main_output, source, num_samples);
    };

    route(filter_1_->output()->buffer, filter_1_destination_->value());
    route(filter_2_->output()->buffer, filter_2_destination_->value());
  }

  void FiltersModule::process(int num_samples) {
    if (filter_1_filter_input_->value() && filter_1_->getOnValue()->value())
      processSerialBackward(num_samples);
    else if (filter_2_filter_input_->value() && filter_2_->getOnValue()->value())
      processSerialForward(num_samples);
    else
      processParallel(num_samples);
  }
} // namespace vital
