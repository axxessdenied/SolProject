#include "waveform.hpp"

#include <algorithm>

namespace forge {

std::vector<WaveformColumn>
waveformEnvelope(std::span<const std::int16_t> samples, std::uint32_t channelCount, std::size_t columns)
{
    const std::uint32_t stride = channelCount;
    if (columns == 0 || stride == 0 || samples.empty()) {
        return {};
    }
    const std::size_t frames = samples.size() / stride;
    if (frames == 0) {
        return {};
    }

    std::vector<WaveformColumn> out(columns);
    for (std::size_t column = 0; column < columns; ++column) {
        // ⚑ The span is computed from the column index rather than accumulated,
        // so rounding cannot drift across a long cue and leave the last column
        // short - the same argument `textureSetRowPosition` makes about
        // absolute positions beating per-step deltas.
        const std::size_t begin = (column * frames) / columns;
        std::size_t end = ((column + 1) * frames) / columns;
        // ⚑⚑ AT LEAST ONE FRAME PER COLUMN, WHICH IS THE SHORT-CUE CASE AND NOT
        // A DEFENSIVE HABIT. `ui_click` is 2,205 frames; asked for more columns
        // than that - an ordinary panel width on a wide window - the integer
        // division gives begin == end for most of them, and every one of those
        // would draw as silence. A short cue would look like a mostly empty
        // panel with a few spikes, which is a picture of the arithmetic rather
        // than of the sound.
        if (end <= begin) {
            end = std::min(begin + 1, frames);
        }

        float low = 0.0f;
        float high = 0.0f;
        bool first = true;
        for (std::size_t frame = begin; frame < end; ++frame) {
            float folded = 0.0f;
            for (std::uint32_t channel = 0; channel < stride; ++channel) {
                folded += static_cast<float>(samples[(frame * stride) + channel]);
            }
            folded /= static_cast<float>(stride);
            // 32768 rather than 32767: int16 runs -32768..32767, so the
            // loudest possible sample is a TROUGH, and dividing by the positive
            // limit would put it past -1. `reportSound` already records this
            // asymmetry for `peak`; the drawing has to agree with it or the
            // number and the picture disagree at exactly the interesting value.
            folded /= 32768.0f;
            if (first) {
                low = folded;
                high = folded;
                first = false;
            } else {
                low = std::min(low, folded);
                high = std::max(high, folded);
            }
        }
        out[column].low = low;
        out[column].high = high;
    }
    return out;
}

} // namespace forge
