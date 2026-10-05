"""Reproducible prototype frontend. This is NOT the microWakeWord frontend.

Port these exact features (and the capture high-pass) before using its TFLite
model on ESP32. DSP constants are saved with each exported model.
"""
import hashlib
import io
import math
import wave
import numpy as np
from scipy.signal import resample_poly, lfilter

RATE = 16000
SAMPLES = 38400  # 2.4 seconds, sliding inference every 100 ms
FRAME, HOP, FFT, MELS = 480, 160, 512, 40
FRAMES = 1 + (SAMPLES - FRAME) // HOP
FRONTEND = dict(rate=RATE, samples=SAMPLES, frame=FRAME, hop=HOP, fft=FFT,
                mels=MELS, low_hz=125, high_hz=7500, power_divisor=512,
                log_floor=1e-10, log_offset=10, log_scale=5, clip=[-3, 3],
                window="numpy.hanning(480), symmetric", feature_version="bender-logmel-v1",
                capture="60 Hz DC-block before PCM16 saturation; native capture 24 kHz")


def seed_for(text):
    return int.from_bytes(hashlib.sha256(text.encode()).digest()[:8], "little")


def load_wav(body):
    with wave.open(io.BytesIO(body), 'rb') as wav:
        if wav.getnchannels() != 1 or wav.getsampwidth() != 2 or wav.getcomptype() != 'NONE':
            raise ValueError('Expected PCM16 mono')
        rate = wav.getframerate()
        x = np.frombuffer(wav.readframes(wav.getnframes()), dtype='<i2').astype(np.float32) / 32768
    if rate != RATE:
        divisor = math.gcd(rate, RATE)
        x = resample_poly(x, RATE // divisor, rate // divisor).astype(np.float32)
    return x


def highpass(x):
    # Synthetic examples get the same 60 Hz cutoff as microphone capture.
    alpha = np.exp(-2 * np.pi * 60 / RATE)
    return lfilter([alpha, -alpha], [1, -alpha], x).astype(np.float32)


def speech_bounds(x):
    """Energy trim for positive training crops only. Streaming evaluation never trims."""
    block = 160
    # The microphone has isolated clicks: 10 ms peaks mistake them for speech
    # and select all four seconds. Smooth energy over 100 ms before thresholding.
    energy = np.convolve(x*x, np.ones(1600,np.float32)/1600, mode='same')
    envelope = np.sqrt(np.maximum(0,energy[::block]))
    floor = float(np.percentile(envelope, 30))
    threshold = max(.003, floor * 1.9, float(np.percentile(envelope, 95)) * .2)
    active = np.flatnonzero(envelope > threshold)
    if not len(active):
        return 0, len(x)
    # A separate tap seconds after the word must not stretch its training crop.
    # Join short within-phrase gaps, then retain the strongest speech region.
    regions=np.split(active,np.flatnonzero(np.diff(active)>20)+1)
    region=max(regions,key=lambda r:float(np.sum(np.maximum(0,envelope[r[0]:r[-1]+1]**2-floor**2))))
    return max(0, int(region[0]) * block - 1600), min(len(x), int(region[-1] + 1) * block + 2400)


def fit_window(x, rng, positive):
    if positive:
        start, stop = speech_bounds(x)
        x = x[start:stop]
        if len(x) > SAMPLES:
            # Preserve the complete phrase, rather than labelling half a word positive.
            x = resample_poly(x, SAMPLES - 1600, len(x)).astype(np.float32)
    if len(x) > SAMPLES:
        start = int(rng.integers(len(x) - SAMPLES + 1))
        return x[start:start + SAMPLES].copy()
    out = np.zeros(SAMPLES, np.float32)
    start = int(rng.integers(SAMPLES - len(x) + 1))
    out[start:start + len(x)] = x
    return out


def mel_matrix():
    hz_to_mel = lambda f: 2595 * np.log10(1 + f / 700)
    points = 700 * (10 ** (np.linspace(hz_to_mel(125), hz_to_mel(7500), MELS + 2) / 2595) - 1)
    hz = np.arange(FFT // 2 + 1) * RATE / FFT
    matrix = np.zeros((FFT // 2 + 1, MELS), np.float32)
    for i in range(MELS):
        matrix[:, i] = np.maximum(0, np.minimum((hz-points[i])/(points[i+1]-points[i]),
                                                (points[i+2]-hz)/(points[i+2]-points[i+1])))
    return matrix


MEL = mel_matrix()
WINDOW = np.hanning(FRAME).astype(np.float32)


def features(x):
    if x.shape != (SAMPLES,):
        raise ValueError(f'Expected {SAMPLES} PCM samples')
    frames = np.lib.stride_tricks.sliding_window_view(x, FRAME)[::HOP]
    spectrum = np.fft.rfft(frames * WINDOW, n=FFT, axis=1)
    power = (spectrum.real ** 2 + spectrum.imag ** 2) / FFT
    logmel = np.log(np.maximum(power @ MEL, 1e-10))
    return np.clip((logmel + 10) / 5, -3, 3).astype(np.float32)[..., None]


def stream_training_window(x, rng, positive):
    """Causal context at capture level; positives retain the complete detected phrase.

    Negative prefixes include startup padding seen by the streaming evaluator.
    Return None rather than silently cut a positive phrase longer than the window.
    """
    if positive:
        start, stop = speech_bounds(x)
        if stop - start > SAMPLES:
            return None
        first = stop
        last = min(len(x), start + SAMPLES)
        end = int(rng.integers(first, last + 1))
    else:
        end = int(rng.integers(min(1600, len(x)), len(x) + 1))
    chunk = x[max(0, end - SAMPLES):end]
    window = np.zeros(SAMPLES, np.float32)
    window[-len(chunk):] = chunk
    window *= float(10 ** rng.uniform(-.3, .3))
    return np.clip(window, -.98, .98)


def augment(x, rng, positive, backgrounds):
    speed = float(rng.uniform(.88, 1.12))
    x = resample_poly(x, 100, round(100 * speed)).astype(np.float32)
    x = fit_window(x, rng, positive)
    rms = np.sqrt(np.mean(x*x)) + 1e-8
    # Both classes get the same level distribution; loudness is not the label.
    x *= float(10 ** rng.uniform(-2.05, -1.05)) / rms
    if rng.random() < .5:
        delay = int(rng.integers(320, 1600))
        x[delay:] += float(rng.uniform(.04, .25)) * x[:-delay].copy()
    if backgrounds and rng.random() < .85:
        noise = fit_window(backgrounds[int(rng.integers(len(backgrounds)))], rng, False)
        nrms = np.sqrt(np.mean(noise*noise)) + 1e-8
        snr = float(rng.uniform(8, 28))
        x += noise / nrms * (np.sqrt(np.mean(x*x)) / 10 ** (snr / 20))
    x += rng.normal(0, 10 ** rng.uniform(-4.5, -3), SAMPLES).astype(np.float32)
    return np.clip(x, -.98, .98)
