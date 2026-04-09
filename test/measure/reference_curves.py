"""
Reference curves for audio quality measurements.
ISO 226:2003 equal-loudness contours and Harman 2019 target curves.
"""

import numpy as np

# ---------------------------------------------------------------------------
# ISO 226:2003 Equal-Loudness Contours
# Threshold of hearing and loudness levels at standard frequencies.
# Values: SPL in dB required to perceive N phon loudness.
# ---------------------------------------------------------------------------

ISO226_FREQS = np.array([
    20, 25, 31.5, 40, 50, 63, 80, 100, 125, 160, 200, 250, 315, 400,
    500, 630, 800, 1000, 1250, 1600, 2000, 2500, 3150, 4000, 5000,
    6300, 8000, 10000, 12500
])

# SPL values for each phon level (rows = phon levels, columns = frequencies)
ISO226_PHON_LEVELS = [20, 40, 60, 80]

ISO226_SPL = {
    20: np.array([
        78.5, 68.7, 59.5, 51.1, 44.0, 37.5, 31.5, 26.5, 22.1, 17.9,
        14.4, 11.4, 8.6, 6.2, 4.4, 3.0, 2.2, 2.4, 3.5, 1.7, -1.3,
        -4.2, -6.0, -5.4, -1.5, 6.0, 12.6, 13.9, 12.3
    ]),
    40: np.array([
        99.9, 93.9, 88.2, 82.6, 77.8, 73.1, 68.5, 64.4, 60.6, 56.7,
        53.4, 50.4, 47.6, 45.0, 43.1, 41.3, 40.1, 40.0, 41.8, 42.5,
        39.2, 36.5, 35.6, 36.7, 40.0, 45.8, 51.8, 54.3, 51.5
    ]),
    60: np.array([
        113.9, 109.5, 105.4, 101.2, 97.6, 94.1, 90.6, 87.5, 84.6, 81.6,
        79.0, 76.7, 74.5, 72.5, 71.0, 69.7, 68.7, 68.3, 69.5, 70.7,
        68.5, 66.2, 65.2, 65.6, 68.1, 72.7, 77.5, 79.4, 77.4
    ]),
    80: np.array([
        126.0, 122.7, 119.5, 116.3, 113.5, 110.8, 108.1, 105.7, 103.4,
        101.2, 99.2, 97.3, 95.5, 93.9, 92.6, 91.5, 90.6, 90.0, 90.7,
        92.0, 90.3, 88.6, 87.5, 87.4, 89.0, 92.2, 96.0, 97.5, 96.2
    ]),
}


def iso226_compensation(volume_phon: float, reference_phon: float = 80.0) -> tuple:
    """
    Compute the loudness compensation curve for a given listening level.
    Returns (frequencies, gain_db) where gain_db is the boost needed
    relative to the reference level.

    At reference_phon, gain is 0 (no compensation).
    At lower volume, bass and treble are boosted.
    """
    # Interpolate between available phon levels
    levels = sorted(ISO226_SPL.keys())
    if volume_phon <= levels[0]:
        vol_spl = ISO226_SPL[levels[0]]
    elif volume_phon >= levels[-1]:
        vol_spl = ISO226_SPL[levels[-1]]
    else:
        # Linear interpolation between bracketing phon levels
        for i in range(len(levels) - 1):
            if levels[i] <= volume_phon <= levels[i + 1]:
                t = (volume_phon - levels[i]) / (levels[i + 1] - levels[i])
                vol_spl = (1 - t) * ISO226_SPL[levels[i]] + t * ISO226_SPL[levels[i + 1]]
                break

    if reference_phon <= levels[0]:
        ref_spl = ISO226_SPL[levels[0]]
    elif reference_phon >= levels[-1]:
        ref_spl = ISO226_SPL[levels[-1]]
    else:
        for i in range(len(levels) - 1):
            if levels[i] <= reference_phon <= levels[i + 1]:
                t = (reference_phon - levels[i]) / (levels[i + 1] - levels[i])
                ref_spl = (1 - t) * ISO226_SPL[levels[i]] + t * ISO226_SPL[levels[i + 1]]
                break

    # Compensation = how much more SPL is needed at low volume vs reference
    # Positive = needs boost
    compensation_db = vol_spl - ref_spl
    # Normalize so 1kHz is always 0dB compensation
    idx_1k = np.argmin(np.abs(ISO226_FREQS - 1000))
    compensation_db -= compensation_db[idx_1k]

    return ISO226_FREQS, compensation_db


# ---------------------------------------------------------------------------
# Harman Target Curves (2019)
# ---------------------------------------------------------------------------

# Harman 2019 Over-Ear target (dB relative to 1 kHz)
HARMAN_OE_2019_FREQS = np.array([
    20, 30, 50, 80, 100, 200, 300, 500, 700, 1000,
    1500, 2000, 3000, 4000, 5000, 6000, 8000, 10000, 12000, 16000, 20000
])
HARMAN_OE_2019_DB = np.array([
    6.0, 4.5, 2.5, 1.5, 1.0, 0.5, 0.0, 0.0, 0.0, 0.0,
    -0.5, -1.0, -2.0, -3.5, -4.0, -5.0, -6.0, -7.5, -8.5, -6.0, -3.0
])

# Harman 2019 In-Ear target (dB relative to 1 kHz)
HARMAN_IE_2019_FREQS = np.array([
    20, 30, 50, 80, 100, 200, 300, 500, 700, 1000,
    1500, 2000, 3000, 4000, 5000, 6000, 8000, 10000, 12000, 16000, 20000
])
HARMAN_IE_2019_DB = np.array([
    8.0, 6.5, 4.0, 2.5, 2.0, 1.0, 0.5, 0.0, 0.0, 0.0,
    1.0, 0.0, -2.0, -4.0, -3.5, -4.0, -7.0, -9.0, -10.0, -8.0, -5.0
])
