"""Convert the ESP's mono PCM16 to Whisper's actual sample rate."""
import av
import numpy as np

ASR_RATE = 16000


def pcm16_for_asr(pcm: bytes, source_rate: int) -> np.ndarray:
    if source_rate <= 0 or len(pcm) % 2:
        raise ValueError("ASR requires a positive sample rate and complete PCM16 samples")
    if not pcm:
        return np.empty(0, dtype=np.float32)
    samples = np.frombuffer(pcm, dtype="<i2").astype(np.int16, copy=False)
    if source_rate == ASR_RATE:
        return samples.astype(np.float32) / 32768.0
    frame = av.AudioFrame.from_ndarray(samples.reshape(1, -1), format="s16", layout="mono")
    frame.sample_rate = source_rate
    # libswresample includes an anti-alias filter; just relabelling the rate or
    # decimating samples would distort speech. Flush the filter's buffered tail.
    resampler = av.AudioResampler(format="fltp", layout="mono", rate=ASR_RATE)
    frames = resampler.resample(frame) + resampler.resample(None)
    if not frames:
        return np.empty(0, dtype=np.float32)
    audio = np.concatenate([f.to_ndarray().reshape(-1) for f in frames])
    return np.ascontiguousarray(audio, dtype=np.float32)
