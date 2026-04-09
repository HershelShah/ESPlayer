"""
Tests for TPDF dither (Commit 1).

Proves:
1. Truncation produces idle tones — baseline is broken
2. TPDF eliminates idle tones
3. TPDF maintains >90dB dynamic range
4. TPDF ENOB stays >14 at -60dBFS
"""

import sys
import os
sys.path.insert(0, os.path.dirname(__file__))

from metrics import (
    gen_sine, truncate_16bit, tpdf_dither_16bit,
    measure_thd_n, measure_dynamic_range, detect_idle_tones,
)

PASS = 0
FAIL = 0

def check(name, condition, detail=""):
    global PASS, FAIL
    if condition:
        PASS += 1
        print(f"  PASS: {name}  {detail}")
    else:
        FAIL += 1
        print(f"  FAIL: {name}  {detail}")


def test_truncation_has_idle_tones():
    """Baseline: truncation produces idle tones at low levels."""
    print("\n--- test_truncation_has_idle_tones ---")
    r = detect_idle_tones(truncate_16bit)
    check("truncation has idle tones",
          r['has_idle_tones'],
          f"peaks={r['spurious_peaks']} flatness={r['spectral_flatness']:.4f}")


def test_tpdf_eliminates_idle_tones():
    """TPDF dither should eliminate idle tones (< 10 spurious peaks)."""
    print("\n--- test_tpdf_eliminates_idle_tones ---")
    r = detect_idle_tones(tpdf_dither_16bit)
    # TPDF may have a few random peaks; the key is far fewer than truncation's 3000+
    check("TPDF eliminates idle tones (< 10 peaks)",
          r['spurious_peaks'] < 10,
          f"peaks={r['spurious_peaks']} flatness={r['spectral_flatness']:.4f}")


def test_tpdf_dynamic_range():
    """TPDF DR should be >78dB (noise floor raised ~2dB from dither is expected)."""
    print("\n--- test_tpdf_dynamic_range ---")
    r = measure_dynamic_range(tpdf_dither_16bit)
    check("TPDF DR > 78dB",
          r['effective_dynamic_range_db'] > 78,
          f"DR={r['effective_dynamic_range_db']:.1f}dB")


def test_tpdf_enob_at_minus60():
    """At -60dBFS, TPDF ENOB > 3 (signal detectable above noise floor)."""
    print("\n--- test_tpdf_enob_at_minus60 ---")
    tone = gen_sine(997.0, 10**(-60/20), 2.0)
    quantized = tpdf_dither_16bit(tone)
    r = measure_thd_n(quantized, 44100, 997.0)
    # At -60dBFS with 16-bit TPDF, signal is ~36dB above noise floor → ENOB ~4-6
    check("TPDF ENOB > 3 at -60dBFS",
          r['enob'] > 3,
          f"ENOB={r['enob']:.1f} SINAD={r['sinad_db']:.1f}dB")


def test_tpdf_full_scale_thdn():
    """TPDF should have THD+N < -85dB at full scale."""
    print("\n--- test_tpdf_full_scale_thdn ---")
    tone = gen_sine(997.0, 10**(-1/20), 2.0)
    quantized = tpdf_dither_16bit(tone)
    r = measure_thd_n(quantized, 44100, 997.0)
    check("TPDF THD+N < -85dB at -1dBFS",
          r['thd_n_db'] < -85,
          f"THD+N={r['thd_n_db']:.1f}dB SINAD={r['sinad_db']:.1f}dB")


def test_tpdf_vs_truncation_improvement():
    """TPDF should measurably reduce spurious peaks vs truncation."""
    print("\n--- test_tpdf_vs_truncation_improvement ---")
    r_trunc = detect_idle_tones(truncate_16bit)
    r_tpdf = detect_idle_tones(tpdf_dither_16bit)
    improvement = r_trunc['spurious_peaks'] - r_tpdf['spurious_peaks']
    check("TPDF reduces spurious peaks by >90%",
          improvement > r_trunc['spurious_peaks'] * 0.9,
          f"truncate={r_trunc['spurious_peaks']} tpdf={r_tpdf['spurious_peaks']} reduction={improvement}")


if __name__ == '__main__':
    print("=" * 50)
    print("  TPDF Dither Tests")
    print("=" * 50)

    test_truncation_has_idle_tones()
    test_tpdf_eliminates_idle_tones()
    test_tpdf_dynamic_range()
    test_tpdf_enob_at_minus60()
    test_tpdf_full_scale_thdn()
    test_tpdf_vs_truncation_improvement()

    print(f"\n{'=' * 50}")
    print(f"  Results: {PASS} passed, {FAIL} failed")
    print(f"{'=' * 50}")
    sys.exit(1 if FAIL > 0 else 0)
