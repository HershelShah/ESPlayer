#!/usr/bin/env python3
"""
Capture audio from ESP32 via bluealsa and run full quality analysis.

Usage:
    # Capture 10s and analyze:
    sudo python capture_and_compare.py

    # Compare two captures (e.g. before/after DSP change):
    sudo python capture_and_compare.py --a before.wav --b after.wav

    # Capture with a label (saves to labeled file):
    sudo python capture_and_compare.py --label flat
    sudo python capture_and_compare.py --label edm
    sudo python capture_and_compare.py --a capture_flat.wav --b capture_edm.wav

Requires: bluealsa running, ESP32 connected and streaming.
Must run as root (bluealsa D-Bus permissions).
"""

import argparse
import subprocess
import sys
import os
import wave
import time

import numpy as np
from scipy import signal

# Add the measure directory to path
sys.path.insert(0, os.path.dirname(__file__))
from metrics import measure_thd_n, measure_aliasing, detect_idle_tones

ESP32_MAC = "A4:F0:0F:5E:A4:9A"
BLUEALSA_DEV = f"bluealsa:DEV={ESP32_MAC},PROFILE=a2dp"
CAPTURE_DIR = os.path.dirname(os.path.abspath(__file__))


def capture(filename: str, duration: int = 10) -> str:
    """Record from bluealsa to WAV. Returns full path."""
    path = os.path.join(CAPTURE_DIR, filename)
    print(f"Recording {duration}s from ESP32 → {path}")
    print(f"  Device: {BLUEALSA_DEV}")

    # Kill any existing bluealsa-aplay that might hold the PCM
    subprocess.run(["killall", "bluealsa-aplay"], capture_output=True)
    time.sleep(0.5)

    result = subprocess.run(
        ["arecord", "-D", BLUEALSA_DEV, "-f", "cd", "-d", str(duration),
         "-t", "wav", path],
        capture_output=True, text=True, timeout=duration + 10
    )
    if result.returncode != 0:
        print(f"  ERROR: arecord failed: {result.stderr.strip()}")
        sys.exit(1)

    size = os.path.getsize(path)
    print(f"  Captured: {size / 1024:.0f} KB")
    return path


def load_wav(path: str) -> tuple:
    """Load WAV, return (left_channel_float64, sample_rate)."""
    f = wave.open(path, "rb")
    p = f.getparams()
    raw = f.readframes(p.nframes)
    f.close()

    samples = np.frombuffer(raw, dtype=np.int16).astype(np.float64) / 32768.0
    left = samples[0::p.nchannels]
    return left, p.framerate


def analyze(path: str, label: str = "") -> dict:
    """Run full analysis on a WAV capture."""
    left, fs = load_wav(path)
    duration = len(left) / fs

    print(f"\n{'=' * 60}")
    print(f"  {label or path}")
    print(f"  Duration: {duration:.1f}s  Sample rate: {fs} Hz")
    print(f"{'=' * 60}")

    results = {}

    # --- Signal levels ---
    peak = np.max(np.abs(left))
    rms = np.sqrt(np.mean(left ** 2))
    peak_db = 20 * np.log10(peak + 1e-10)
    rms_db = 20 * np.log10(rms + 1e-10)
    print(f"\n  Signal Level:")
    print(f"    Peak: {peak_db:.1f} dBFS")
    print(f"    RMS:  {rms_db:.1f} dBFS")
    results['peak_dbfs'] = peak_db
    results['rms_dbfs'] = rms_db

    # --- Spectral analysis (band energies) ---
    print(f"\n  Frequency Band Energy:")
    bands = [
        ("Sub-bass",   20,    60),
        ("Bass",       60,   200),
        ("Low-mid",   200,   500),
        ("Mid",       500,  2000),
        ("Upper-mid", 2000,  4000),
        ("Presence",  4000,  8000),
        ("Treble",    8000, 16000),
    ]

    # Use middle 80% of signal to avoid edge effects
    margin = int(len(left) * 0.1)
    seg = left[margin:-margin] if margin > 0 else left
    window = signal.windows.blackmanharris(len(seg))
    spectrum = np.abs(np.fft.rfft(seg * window))
    freqs = np.fft.rfftfreq(len(seg), d=1.0 / fs)

    band_energies = {}
    for name, lo, hi in bands:
        mask = (freqs >= lo) & (freqs < hi)
        if np.any(mask):
            energy = 20 * np.log10(np.mean(spectrum[mask]) + 1e-30)
            band_energies[name] = energy
            print(f"    {name:12s} ({lo:5d}-{hi:5d} Hz): {energy:6.1f} dB")

    results['band_energies'] = band_energies

    # --- Dropout detection ---
    print(f"\n  Dropout Detection:")
    win_samples = int(fs * 0.005)  # 5ms windows
    n_windows = len(left) // win_samples
    energies = np.zeros(n_windows)
    for i in range(n_windows):
        chunk = left[i * win_samples:(i + 1) * win_samples]
        energies[i] = np.sqrt(np.mean(chunk ** 2))

    threshold = 10 ** (-50 / 20)  # -50 dBFS
    is_dropout = energies < threshold
    changes = np.diff(is_dropout.astype(int))
    starts = np.where(changes == 1)[0] + 1
    ends = np.where(changes == -1)[0] + 1
    if len(is_dropout) > 0 and is_dropout[0]:
        starts = np.concatenate(([0], starts))
    if len(is_dropout) > 0 and is_dropout[-1]:
        ends = np.concatenate((ends, [len(is_dropout)]))

    n = min(len(starts), len(ends))
    lengths = ends[:n] - starts[:n]
    sig_dropouts = lengths[lengths > 2]  # >10ms

    total_dropout_ms = np.sum(sig_dropouts) * 5.0
    total_ms = n_windows * 5.0
    dropout_pct = total_dropout_ms / total_ms * 100 if total_ms > 0 else 0

    print(f"    Dropouts (>10ms): {len(sig_dropouts)}")
    print(f"    Total dropout: {total_dropout_ms:.0f}ms / {total_ms:.0f}ms ({dropout_pct:.2f}%)")
    for i in range(min(5, len(sig_dropouts))):
        t = starts[:n][lengths > 2][i] * 0.005
        d = sig_dropouts[i] * 5.0
        print(f"      at {t:.2f}s, {d:.0f}ms")

    results['dropouts'] = len(sig_dropouts)
    results['dropout_pct'] = dropout_pct

    # --- Stereo analysis ---
    left_full, _ = load_wav(path)
    f2 = wave.open(path, "rb")
    p2 = f2.getparams()
    raw2 = f2.readframes(p2.nframes)
    f2.close()
    all_samples = np.frombuffer(raw2, dtype=np.int16).astype(np.float64) / 32768.0

    if p2.nchannels == 2:
        right = all_samples[1::2]
        left_ch = all_samples[0::2]
        # Stereo correlation
        corr = np.corrcoef(left_ch[:len(right)], right[:len(left_ch)])[0, 1]
        print(f"\n  Stereo Analysis:")
        print(f"    L/R correlation: {corr:.4f}")
        print(f"    {'Mono-ish' if corr > 0.95 else 'Stereo' if corr > 0.5 else 'Wide stereo'}")
        results['stereo_correlation'] = corr

    # --- Crest factor (dynamic range indicator) ---
    crest = peak / (rms + 1e-10)
    crest_db = 20 * np.log10(crest)
    print(f"\n  Dynamics:")
    print(f"    Crest factor: {crest_db:.1f} dB")
    print(f"    {'Heavily compressed' if crest_db < 6 else 'Normal' if crest_db < 14 else 'High dynamic range'}")
    results['crest_factor_db'] = crest_db

    return results


def compare(results_a: dict, results_b: dict, label_a: str, label_b: str):
    """Print side-by-side comparison."""
    print(f"\n{'=' * 60}")
    print(f"  COMPARISON: {label_a} vs {label_b}")
    print(f"{'=' * 60}")

    # Levels
    print(f"\n  {'Metric':<25s} {'A':>10s} {'B':>10s} {'Diff':>10s}")
    print(f"  {'-' * 55}")

    for key in ['peak_dbfs', 'rms_dbfs', 'crest_factor_db', 'dropout_pct']:
        if key in results_a and key in results_b:
            a = results_a[key]
            b = results_b[key]
            diff = b - a
            arrow = '↑' if diff > 0.5 else '↓' if diff < -0.5 else '='
            print(f"  {key:<25s} {a:>9.1f} {b:>9.1f} {arrow}{abs(diff):>8.1f}")

    if 'stereo_correlation' in results_a and 'stereo_correlation' in results_b:
        a = results_a['stereo_correlation']
        b = results_b['stereo_correlation']
        diff = b - a
        arrow = '↑' if diff > 0.01 else '↓' if diff < -0.01 else '='
        print(f"  {'stereo_correlation':<25s} {a:>9.4f} {b:>9.4f} {arrow}{abs(diff):>8.4f}")

    # Band energies
    if 'band_energies' in results_a and 'band_energies' in results_b:
        print(f"\n  {'Band':<25s} {'A':>10s} {'B':>10s} {'Diff':>10s}")
        print(f"  {'-' * 55}")
        for band in results_a['band_energies']:
            if band in results_b['band_energies']:
                a = results_a['band_energies'][band]
                b = results_b['band_energies'][band]
                diff = b - a
                bar = '+' * int(abs(diff)) if diff > 0 else '-' * int(abs(diff))
                print(f"  {band:<25s} {a:>9.1f} {b:>9.1f} {diff:>+9.1f} {bar}")


def main():
    parser = argparse.ArgumentParser(description='ESP32 Audio Capture & Analysis')
    parser.add_argument('--a', type=str, help='First WAV file (or capture with --label)')
    parser.add_argument('--b', type=str, help='Second WAV file for comparison')
    parser.add_argument('--label', type=str, help='Label for capture (e.g. "flat", "edm")')
    parser.add_argument('--duration', type=int, default=10, help='Capture duration (seconds)')
    parser.add_argument('--no-capture', action='store_true', help='Skip capture, analyze existing files')
    args = parser.parse_args()

    if args.a and args.b:
        # Compare two existing files
        r_a = analyze(args.a, label_a := os.path.basename(args.a))
        r_b = analyze(args.b, label_b := os.path.basename(args.b))
        compare(r_a, r_b, label_a, label_b)

    elif args.label:
        # Capture with label
        filename = f"capture_{args.label}.wav"
        path = capture(filename, args.duration)
        analyze(path, args.label)
        print(f"\nSaved as {filename}")
        print(f"To compare: sudo python {__file__} --a capture_X.wav --b capture_Y.wav")

    elif not args.no_capture:
        # Single capture + analysis
        path = capture("capture_latest.wav", args.duration)
        analyze(path, "latest capture")

    else:
        parser.print_help()


if __name__ == '__main__':
    main()
