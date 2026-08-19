#ifndef BMX_AUDIO_PACING_H
#define BMX_AUDIO_PACING_H

#include <limits.h>
#include <stdint.h>

namespace bmc64 {

inline unsigned AudioGeneratorSampleRate(unsigned output_sample_rate,
                                         unsigned machine_cycles_per_sec,
                                         unsigned output_cycles_per_sec) {
  if (output_sample_rate == 0U || machine_cycles_per_sec == 0U ||
      output_cycles_per_sec == 0U) {
    return output_sample_rate;
  }

  const uint64_t scaled =
      static_cast<uint64_t>(output_sample_rate) * machine_cycles_per_sec;
  const uint64_t rounded =
      (scaled + output_cycles_per_sec / 2U) / output_cycles_per_sec;
  return rounded > UINT_MAX ? UINT_MAX : static_cast<unsigned>(rounded);
}

}  // namespace bmc64

#endif
