#!/usr/bin/env python3
"""
Master measurement runner for ESPlayer DSP improvements.

Usage:
    python run_measurements.py --baseline          # Record baseline numbers
    python run_measurements.py --compare baseline.json  # Compare against baseline
    python run_measurements.py                     # Just run and print results
"""

import json
import sys
import os
import argparse

sys.path.insert(0, os.path.dirname(__file__))
from metrics import (
    gen_sine, truncate_16bit, tpdf_dither_16bit, noise_shaped_dither_16bit,
    measure_thd_n, measure_dynamic_range, detect_idle_tones, measure_aliasing,
)


def run_all_measurements() -> dict:
    """Run all measurements and return results dict."""
    results = {}

    # --- THD+N of quantization methods ---
    print("\n=== THD+N @ 997Hz -1dBFS ===")
    tone = gen_sine(997.0, 10 ** (-1 / 20), 2.0)

    for name, fn in [('truncate', truncate_16bit),
                     ('tpdf', tpdf_dither_16bit),
                     ('noise_shaped', noise_shaped_dither_16bit)]:
        quantized = fn(tone)
        r = measure_thd_n(quantized, 44100, 997.0)
        key = f'thd_n_{name}'
        results[key] = r
        print(f"  {name:15s}: THD+N={r['thd_n_db']:.1f}dB  SINAD={r['sinad_db']:.1f}dB  ENOB={r['enob']:.1f}  inband_noise={r['inband_noise_db']:.1f}dB")

    # --- Dynamic Range ---
    print("\n=== Dynamic Range ===")
    for name, fn in [('truncate', truncate_16bit),
                     ('tpdf', tpdf_dither_16bit),
                     ('noise_shaped', noise_shaped_dither_16bit)]:
        r = measure_dynamic_range(fn)
        key = f'dynamic_range_{name}'
        results[key] = r
        print(f"  {name:15s}: DR={r['effective_dynamic_range_db']:.1f}dB")

    # --- Idle Tone Detection ---
    print("\n=== Idle Tones @ -80dBFS ===")
    for name, fn in [('truncate', truncate_16bit),
                     ('tpdf', tpdf_dither_16bit),
                     ('noise_shaped', noise_shaped_dither_16bit)]:
        r = detect_idle_tones(fn)
        key = f'idle_tones_{name}'
        results[key] = r
        print(f"  {name:15s}: flatness={r['spectral_flatness']:.4f}  peaks={r['spurious_peaks']}  idle_tones={r['has_idle_tones']}")

    # --- Aliasing (nonlinear processing simulation) ---
    print("\n=== Aliasing (simulated soft-clip @ 7kHz) ===")
    tone_7k = gen_sine(7000.0, 0.95, 2.0)
    # Simulate limiter: soft clip
    import numpy as np
    clipped = np.tanh(tone_7k * 2.0) / np.tanh(2.0)
    r = measure_aliasing(clipped, 44100, 7000.0)
    results['aliasing_softclip_7k'] = r
    print(f"  alias_power={r['alias_power_db']:.1f}dB  harmonic_power={r['harmonic_power_db']:.1f}dB  ratio={r['alias_to_harmonic_ratio_db']:.1f}dB")
    for k, true_f, alias_f in r['aliased_frequencies']:
        print(f"    harmonic {k}: {true_f:.0f}Hz → aliases to {alias_f:.0f}Hz")

    return results


def main():
    parser = argparse.ArgumentParser(description='ESPlayer DSP Measurement Runner')
    parser.add_argument('--baseline', action='store_true',
                        help='Save results as baseline.json')
    parser.add_argument('--compare', type=str, default=None,
                        help='Compare against a baseline JSON file')
    args = parser.parse_args()

    results = run_all_measurements()

    # Save baseline
    if args.baseline:
        path = os.path.join(os.path.dirname(__file__), 'baseline.json')
        # Convert numpy types for JSON serialization
        def sanitize(obj):
            if isinstance(obj, dict):
                return {k: sanitize(v) for k, v in obj.items()}
            elif isinstance(obj, (list, tuple)):
                return [sanitize(v) for v in obj]
            elif isinstance(obj, (float, int, bool, str, type(None))):
                return obj
            else:
                return float(obj)

        with open(path, 'w') as f:
            json.dump(sanitize(results), f, indent=2)
        print(f"\nBaseline saved to {path}")

    # Compare
    if args.compare:
        with open(args.compare) as f:
            baseline = json.load(f)

        print("\n=== Comparison vs Baseline ===")
        for key in sorted(results.keys()):
            if key not in baseline:
                continue
            curr = results[key]
            base = baseline[key]
            if isinstance(curr, dict):
                for metric in curr:
                    if metric in base and isinstance(curr[metric], (int, float)):
                        diff = curr[metric] - base[metric]
                        direction = '↑' if diff > 0 else '↓' if diff < 0 else '='
                        print(f"  {key}.{metric}: {base[metric]:.2f} → {curr[metric]:.2f} ({direction}{abs(diff):.2f})")

    print("\nDone.")


if __name__ == '__main__':
    main()
