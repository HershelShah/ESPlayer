"""
Quantitative audio quality metrics for ESPlayer DSP pipeline.
All functions operate on mono float64 arrays in [-1.0, 1.0] range.
"""

import numpy as np
from scipy import signal

# ---------------------------------------------------------------------------
# Signal generation
# ---------------------------------------------------------------------------

def gen_sine(freq_hz: float, amplitude: float, duration_s: float,
             fs: int = 44100) -> np.ndarray:
    """Generate a mono sine wave. amplitude in linear (0.0 to 1.0)."""
    t = np.arange(int(fs * duration_s)) / fs
    return amplitude * np.sin(2 * np.pi * freq_hz * t)


def gen_stereo_sine(freq_hz: float, amplitude: float, duration_s: float,
                    fs: int = 44100) -> np.ndarray:
    """Generate interleaved stereo sine (L=R). Returns int16 array."""
    mono = gen_sine(freq_hz, amplitude, duration_s, fs)
    stereo = np.empty(len(mono) * 2, dtype=np.int16)
    int_vals = np.clip(mono * 32767, -32768, 32767).astype(np.int16)
    stereo[0::2] = int_vals
    stereo[1::2] = int_vals
    return stereo


def gen_fade(freq_hz: float, start_db: float, end_db: float,
             duration_s: float, fs: int = 44100) -> np.ndarray:
    """Generate a sine with linear dB fade."""
    N = int(fs * duration_s)
    t = np.arange(N) / fs
    level_db = np.linspace(start_db, end_db, N)
    amplitude = 10 ** (level_db / 20.0)
    return amplitude * np.sin(2 * np.pi * freq_hz * t)


# ---------------------------------------------------------------------------
# Quantization functions (for testing dither improvements)
# ---------------------------------------------------------------------------

def truncate_16bit(x: np.ndarray) -> np.ndarray:
    """Current ESPlayer behavior: (int16_t)(x * 32767.0f)."""
    scaled = x * 32768.0
    quantized = np.floor(scaled).astype(np.float64)
    return np.clip(quantized, -32768, 32767) / 32768.0


def tpdf_dither_16bit(x: np.ndarray) -> np.ndarray:
    """TPDF dither: triangular noise with amplitude of 1 LSB."""
    scaled = x * 32768.0
    dither = (np.random.uniform(-1, 1, len(x)) +
              np.random.uniform(-1, 1, len(x)))
    quantized = np.round(scaled + dither)
    return np.clip(quantized, -32768, 32767) / 32768.0


def noise_shaped_dither_16bit(x: np.ndarray) -> np.ndarray:
    """1st-order noise-shaped dither (matches C implementation).
    NTF(z) = 1 - z^-1 → highpass noise shape.
    Subtracts previous error to push quantization noise above 10kHz."""
    scaled = x * 32768.0
    output = np.zeros_like(scaled)
    error = 0.0
    for i in range(len(scaled)):
        dither = np.random.uniform(-1, 1) + np.random.uniform(-1, 1)
        shaped = scaled[i] - error + dither  # SUBTRACT error
        quantized = np.round(shaped)
        quantized = np.clip(quantized, -32768, 32767)
        error = quantized - shaped  # error of SHAPED signal
        output[i] = quantized
    return output / 32768.0


# ---------------------------------------------------------------------------
# THD+N (Total Harmonic Distortion + Noise)
# ---------------------------------------------------------------------------

def measure_thd_n(samples: np.ndarray, fs: int = 44100,
                  fundamental_hz: float = 997.0) -> dict:
    """
    Measure THD+N from a mono float64 signal.
    Uses Blackman-Harris window for >90dB sidelobe suppression.
    """
    N = len(samples)
    window = signal.windows.blackmanharris(N)
    windowed = samples * window
    spectrum = np.abs(np.fft.rfft(windowed)) / N
    freqs = np.fft.rfftfreq(N, d=1.0 / fs)

    # Fundamental bin ± mainlobe width
    fund_bin = int(round(fundamental_hz * N / fs))
    hw = 5  # half-width for mainlobe
    fund_lo = max(0, fund_bin - hw)
    fund_hi = min(len(spectrum), fund_bin + hw + 1)

    fund_power = np.sum(spectrum[fund_lo:fund_hi] ** 2)
    total_power = np.sum(spectrum[1:] ** 2)  # exclude DC
    nd_power = total_power - fund_power

    thd_n_ratio = np.sqrt(nd_power / (fund_power + 1e-30))
    sinad_db = 10 * np.log10(fund_power / (nd_power + 1e-30))
    enob = (sinad_db - 1.76) / 6.02

    # In-band noise (20-4000 Hz) — key metric for noise-shaped dither
    inband_mask = (freqs >= 20) & (freqs <= 4000)
    fund_mask_inband = (freqs >= fundamental_hz - 10) & (freqs <= fundamental_hz + 10)
    inband_power = np.sum(spectrum[inband_mask & ~fund_mask_inband] ** 2)
    inband_noise_db = 10 * np.log10(inband_power + 1e-30)

    return {
        'thd_n_db': 20 * np.log10(thd_n_ratio + 1e-20),
        'thd_n_percent': thd_n_ratio * 100,
        'sinad_db': sinad_db,
        'enob': enob,
        'inband_noise_db': inband_noise_db,
    }


# ---------------------------------------------------------------------------
# Aliasing measurement
# ---------------------------------------------------------------------------

def measure_aliasing(samples_after: np.ndarray, fs: int,
                     fundamental_hz: float, n_harmonics: int = 10) -> dict:
    """
    Measure aliasing from nonlinear processing.
    Checks for energy at frequencies where harmonics fold back from above Nyquist.
    """
    N = len(samples_after)
    window = signal.windows.blackmanharris(N)
    spectrum = np.abs(np.fft.rfft(samples_after * window)) / N
    freqs = np.fft.rfftfreq(N, d=1.0 / fs)
    nyquist = fs / 2.0

    harmonic_power = 0.0
    alias_power = 0.0
    alias_freqs = []

    for k in range(2, n_harmonics + 1):
        true_freq = fundamental_hz * k
        if true_freq < nyquist:
            b = int(round(true_freq * N / fs))
            lo, hi = max(0, b - 3), min(len(spectrum), b + 4)
            harmonic_power += np.sum(spectrum[lo:hi] ** 2)
        else:
            aliased = abs(true_freq % fs)
            if aliased > nyquist:
                aliased = fs - aliased
            if 20 < aliased < nyquist - 100:
                b = int(round(aliased * N / fs))
                lo, hi = max(0, b - 3), min(len(spectrum), b + 4)
                alias_power += np.sum(spectrum[lo:hi] ** 2)
                alias_freqs.append((k, true_freq, aliased))

    alias_db = 10 * np.log10(alias_power + 1e-30)
    harmonic_db = 10 * np.log10(harmonic_power + 1e-30)

    return {
        'alias_power_db': alias_db,
        'harmonic_power_db': harmonic_db,
        'alias_to_harmonic_ratio_db': alias_db - harmonic_db,
        'aliased_frequencies': alias_freqs,
    }


# ---------------------------------------------------------------------------
# Frequency response
# ---------------------------------------------------------------------------

def measure_frequency_response(process_fn, fs: int = 44100,
                               n_fft: int = 65536) -> tuple:
    """
    Measure frequency response via impulse response method.
    process_fn: callable(np.ndarray) -> np.ndarray (mono float64)
    Returns (freqs, magnitude_db, phase_rad).
    """
    impulse = np.zeros(n_fft)
    impulse[0] = 1.0
    ir = process_fn(impulse)

    H = np.fft.rfft(ir)
    freqs = np.fft.rfftfreq(n_fft, d=1.0 / fs)
    mag_db = 20 * np.log10(np.abs(H) + 1e-30)
    phase = np.unwrap(np.angle(H))
    return freqs, mag_db, phase


def frequency_response_error(freqs, measured_db, target_freqs, target_db,
                             freq_range=(20, 20000)) -> dict:
    """Compare measured response against a target curve."""
    target_interp = np.interp(freqs, target_freqs, target_db)
    mask = (freqs >= freq_range[0]) & (freqs <= freq_range[1])
    error = measured_db[mask] - target_interp[mask]

    return {
        'rms_error_db': float(np.sqrt(np.mean(error ** 2))),
        'max_error_db': float(np.max(np.abs(error))),
        'mean_error_db': float(np.mean(error)),
    }


# ---------------------------------------------------------------------------
# Group delay (phase linearity)
# ---------------------------------------------------------------------------

def measure_group_delay(process_fn, fs: int = 44100,
                        n_fft: int = 65536) -> dict:
    """Measure group delay deviation across audible band."""
    freqs, _, phase = measure_frequency_response(process_fn, fs, n_fft)
    omega = 2 * np.pi * freqs
    d_omega = np.diff(omega)
    d_phase = np.diff(phase)

    # Avoid division by zero
    valid = d_omega > 0
    group_delay = np.zeros_like(d_omega)
    group_delay[valid] = -d_phase[valid] / d_omega[valid]

    gd_freqs = (freqs[:-1] + freqs[1:]) / 2
    mask = (gd_freqs >= 20) & (gd_freqs <= 20000)
    gd = group_delay[mask]

    mean_gd = np.mean(gd)
    deviation = gd - mean_gd

    return {
        'mean_group_delay_samples': float(mean_gd),
        'max_deviation_samples': float(np.max(np.abs(deviation))),
        'rms_deviation_samples': float(np.sqrt(np.mean(deviation ** 2))),
        'max_deviation_ms': float(np.max(np.abs(deviation)) / fs * 1000),
    }


# ---------------------------------------------------------------------------
# Dynamic range
# ---------------------------------------------------------------------------

def measure_dynamic_range(quantize_fn, fs: int = 44100) -> dict:
    """
    Measure effective dynamic range by fading a sine to silence.
    quantize_fn: callable(float_signal) -> float_signal
    """
    duration = 4.0
    N = int(fs * duration)
    t = np.arange(N) / fs
    level_db = np.linspace(0, -120, N)
    amplitude = 10 ** (level_db / 20.0)
    tone = amplitude * np.sin(2 * np.pi * 997.0 * t)

    quantized = quantize_fn(tone)

    window_size = int(0.1 * fs)
    hop = window_size // 2
    effective_dr = 0.0

    for start in range(0, N - window_size, hop):
        chunk = quantized[start:start + window_size]
        center_db = level_db[start + window_size // 2]
        if center_db > -110:
            try:
                r = measure_thd_n(chunk, fs, 997.0)
                if r['sinad_db'] > 6.0:
                    effective_dr = abs(center_db)
            except Exception:
                pass

    return {'effective_dynamic_range_db': float(effective_dr)}


# ---------------------------------------------------------------------------
# Idle tone detection
# ---------------------------------------------------------------------------

def detect_idle_tones(quantize_fn, fs: int = 44100) -> dict:
    """
    Detect idle tones from quantization at very low signal levels.
    Idle tones = correlated distortion from truncation without dither.
    """
    N = 2 * fs
    t = np.arange(N) / fs
    tone = 10 ** (-80 / 20) * np.sin(2 * np.pi * 997.0 * t)
    quantized = quantize_fn(tone)

    window = signal.windows.blackmanharris(N)
    spectrum = np.abs(np.fft.rfft(quantized * window))
    freqs = np.fft.rfftfreq(N, d=1.0 / fs)

    ps = spectrum[(freqs > 100) & (freqs < 20000)] ** 2
    ps_db = 10 * np.log10(ps + 1e-30)

    geometric_mean = np.exp(np.mean(np.log(ps + 1e-30)))
    arithmetic_mean = np.mean(ps)
    spectral_flatness = geometric_mean / (arithmetic_mean + 1e-30)

    noise_floor = np.median(ps_db)
    spurious_peaks = int(np.sum(ps_db > noise_floor + 15))

    return {
        'spectral_flatness': float(spectral_flatness),
        'noise_floor_db': float(noise_floor),
        'spurious_peaks': spurious_peaks,
        'has_idle_tones': spurious_peaks > 5,
    }
