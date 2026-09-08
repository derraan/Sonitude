from __future__ import annotations

import numpy as np

from edge.realtime.ring_buffer import OutputRingBuffer


class DryReferenceRing:
    """
    Maintain a dry-reference audio timeline at output_sr.

    The worker feeds dry audio (from input monitor channel) continuously, and
    the wet path consumes exactly matching frames when mixing. This prevents
    silent truncation when the wet path produces variable-length output (e.g.
    catch-up producing multiple model chunks).
    """

    def __init__(self, *, capacity_frames: int, output_sr: int, input_sr: int) -> None:
        from edge.pipeline.processing import StreamingResampler

        self.output_sr = int(output_sr)
        self.input_sr = int(input_sr)
        self._ring = OutputRingBuffer(num_channels=1, capacity_frames=int(capacity_frames))
        self._src = StreamingResampler(src_sr=self.input_sr, dst_sr=self.output_sr, num_channels=1)

    def clear(self) -> None:
        self._ring.clear()
        self._src.reset()

    def reset(self) -> None:
        self.clear()

    def ahead_frames(self) -> int:
        return self._ring.ahead_frames()

    def drop_oldest_frames(self, n: int) -> int:
        return self._ring.drop_oldest_frames(n)

    def read_into_output(self, out: np.ndarray) -> int:
        """
        Read up to out.shape[0] frames of dry audio at output_sr into out [T, C].

        Mono dry is replicated across all output channels. Returns the number of
        frames actually read (may be less than T if the ring underruns).
        """
        out = np.asarray(out, dtype=np.float32)
        if out.ndim != 2:
            raise ValueError(f"expected out [T,C], got {out.shape}")
        n = int(out.shape[0])
        c = int(out.shape[1])
        if n <= 0 or c <= 0:
            return 0
        mono = np.zeros((n, 1), dtype=np.float32)
        got = int(self._ring.read_into(mono))
        if c == 1:
            out[:, 0] = mono[:, 0]
        else:
            out[:, :] = mono[:, 0:1]
        return got

    def dropped_oldest_frames(self) -> int:
        return int(self._ring.stats.dropped_oldest_frames)

    def push_input_block(self, x_ref_in: np.ndarray) -> int:
        """
        Push a dry monitor-channel block at input_sr.

        x_ref_in: shape [T] float32-ish
        """
        x_ref_in = np.asarray(x_ref_in, dtype=np.float32)
        if x_ref_in.ndim != 1:
            raise ValueError(f"expected x_ref_in [T], got {x_ref_in.shape}")
        x_ref_out = self._src.process(x_ref_in[None, :]).T  # [T,1]
        return int(self._ring.write_drop_oldest(x_ref_out))

    def mix(self, *, y_wet: np.ndarray, mix_dry: float, perf: dict | None = None) -> np.ndarray:
        """
        Mix wet audio with dry timeline-aligned reference.

        y_wet: shape [T]
        Returns mixed [T].
        """
        y_wet = np.asarray(y_wet, dtype=np.float32)
        if y_wet.ndim != 1:
            raise ValueError(f"expected y_wet [T], got {y_wet.shape}")
        a = float(mix_dry)
        if a <= 0.0:
            return y_wet

        dry_block = np.zeros((y_wet.shape[0], 1), dtype=np.float32)
        dry_valid = self._ring.read_into(dry_block)
        if dry_valid < y_wet.shape[0] and perf is not None:
            perf.setdefault("dry_ring_underrun_frames", 0)
            perf["dry_ring_underrun_frames"] += int(y_wet.shape[0] - dry_valid)
        return (1.0 - a) * y_wet + a * dry_block[:, 0]

