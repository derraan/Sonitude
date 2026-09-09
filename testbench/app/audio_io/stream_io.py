"""Block-wise audio I/O so multi-hour captures never sit entirely in RAM."""

from __future__ import annotations

from collections.abc import Iterator
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import numpy as np
import soundfile as sf
from scipy import signal as sps

from app.storage.models import AudioMetadata

PLOT_MAX_POINTS = 8_000
SPEC_WINDOWS = 256
SPEC_NPERSEG = 256
INCORE_PCM_BYTES = 24 * 1024 * 1024
COPY_BYTES_LIMIT = 32 * 1024 * 1024
DEFAULT_BLOCK_FRAMES = 8192


def estimated_pcm_bytes(metadata: AudioMetadata) -> int:
    return int(metadata.num_samples) * int(metadata.channels) * 4


def should_stream_batch(metadata: AudioMetadata) -> bool:
    """True when loading the whole file (and C++ wav_replay copies) would OOM."""
    if metadata.duration_s >= 30.0:
        return True
    return estimated_pcm_bytes(metadata) > INCORE_PCM_BYTES


def iter_mapped_blocks(
    path: str | Path,
    channel_map: list[int],
    *,
    block_frames: int = DEFAULT_BLOCK_FRAMES,
) -> Iterator[np.ndarray]:
    """Yield float32 blocks of shape (frames, len(channel_map))."""
    required = max(channel_map) + 1 if channel_map else 0
    with sf.SoundFile(str(path)) as reader:
        if reader.channels < required:
            raise ValueError(
                f"{Path(path).name} has {reader.channels} channels; "
                f"active_channel_map needs at least {required}"
            )
        while True:
            block = reader.read(block_frames, dtype="float32", always_2d=True)
            if len(block) == 0:
                break
            yield np.ascontiguousarray(block[:, channel_map], dtype=np.float32)


@dataclass
class FileOverview:
    waveform: np.ndarray
    rms_dbfs: np.ndarray
    spectrogram_db: np.ndarray | None
    spectrogram_freqs: np.ndarray | None
    sample_rate_hz: int
    duration_s: float


def load_file_overview(
    path: str | Path,
    *,
    n_wave: int = PLOT_MAX_POINTS,
    n_spec: int = SPEC_WINDOWS,
    spec_nperseg: int = SPEC_NPERSEG,
) -> FileOverview:
    """Build overview plots with hop seeks — not a full-file decode.

    Spectrogram columns are true-rate STFT windows spaced across the file, so
    the X axis is wall-clock duration and Y is 0…Nyquist of the original file.
    """
    path = Path(path)
    with sf.SoundFile(str(path)) as reader:
        sample_rate = int(reader.samplerate)
        total = len(reader)
        channels = reader.channels
        duration_s = total / sample_rate if sample_rate else 0.0
        if total <= 0 or channels <= 0:
            empty = np.zeros((0, max(channels, 1)), dtype=np.float32)
            return FileOverview(empty, np.zeros(0, dtype=np.float32), None, None, sample_rate, 0.0)

        n_wave = max(2, min(n_wave, total))
        waveform = np.empty((n_wave, channels), dtype=np.float32)
        rms = np.empty(n_wave, dtype=np.float32)
        hop = total / n_wave
        window = min(64, total)
        for i in range(n_wave):
            reader.seek(min(total - 1, int(i * hop)))
            block = reader.read(window, dtype="float32", always_2d=True)
            if len(block) == 0:
                waveform = waveform[:i]
                rms = rms[:i]
                break
            waveform[i] = block.mean(axis=0)
            mono = block.mean(axis=1)
            rms[i] = 20.0 * np.log10(float(np.sqrt(np.mean(np.square(mono)) + 1e-12)))

        spectrogram_db = None
        freqs = None
        if n_spec > 0 and total >= spec_nperseg:
            n_spec = max(2, min(n_spec, total // spec_nperseg))
            cols = []
            spec_hop = max(1, (total - spec_nperseg) / (n_spec - 1))
            window_fn = sps.windows.hann(spec_nperseg, sym=False)
            for i in range(n_spec):
                reader.seek(min(total - spec_nperseg, int(i * spec_hop)))
                block = reader.read(spec_nperseg, dtype="float32", always_2d=True)
                if len(block) < spec_nperseg:
                    break
                mono = block.mean(axis=1) * window_fn
                spectrum = np.abs(np.fft.rfft(mono))
                cols.append(20.0 * np.log10(spectrum + 1e-12))
            if cols:
                spectrogram_db = np.stack(cols, axis=1)
                freqs = np.fft.rfftfreq(spec_nperseg, 1.0 / sample_rate)

    return FileOverview(
        waveform=waveform,
        rms_dbfs=rms,
        spectrogram_db=spectrogram_db,
        spectrogram_freqs=freqs,
        sample_rate_hz=sample_rate,
        duration_s=duration_s,
    )


def load_file_overviews_parallel(
    specs: dict[str, tuple[Path | str, dict[str, Any]]],
) -> dict[str, FileOverview]:
    """Load several plot overviews concurrently (independent seek-heavy readers)."""
    if not specs:
        return {}
    if len(specs) == 1:
        key, (path, kwargs) = next(iter(specs.items()))
        return {key: load_file_overview(path, **kwargs)}

    def _load_one(item: tuple[str, Path | str, dict[str, Any]]) -> tuple[str, FileOverview]:
        key, path, kwargs = item
        return key, load_file_overview(path, **kwargs)

    items = [(key, path, kwargs) for key, (path, kwargs) in specs.items()]
    workers = min(len(items), 4)
    with ThreadPoolExecutor(max_workers=workers) as pool:
        return dict(pool.map(_load_one, items))


def load_plot_preview(
    path: str | Path,
    *,
    max_points: int = PLOT_MAX_POINTS,
) -> tuple[np.ndarray, int]:
    """Hop-sample a preview. Does not scan every input frame."""
    overview = load_file_overview(path, n_wave=max_points, n_spec=0)
    return overview.waveform, overview.sample_rate_hz
