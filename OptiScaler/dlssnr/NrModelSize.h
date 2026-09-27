#pragma once

namespace DlssNr {
// Only a size we are already resampling to is rounded. A native-size pass (WorkingScale 1.0, or a scale
// that rounds back to native) is left alone: rounding 1080 to 1088 would turn a 1:1 pass into a
// resample of the frame, which is worse than a ragged border window.
inline unsigned int AlignWorkSize(unsigned int size, unsigned int native)
{
    constexpr unsigned int kGrid = 16;

    if (size == native)
        return size;

    unsigned int aligned = (size + kGrid / 2) / kGrid * kGrid;

    if (aligned < kGrid)
        aligned = kGrid;

    // Shrinking never rounds up past the native size (that would enlarge what was meant to be reduced).
    if (size < native && aligned > native)
        aligned = native;

    return aligned;
}

}
