import torch
import torchaudio
import torch.nn.functional as F
import numpy as np

try:
    import soxr
except ImportError:
    soxr = None

class SpeedPerturbation:
    def __init__(self, min_speed, max_speed, sample_rate = 24000):
        self.min_speed = min_speed
        self.max_speed = max_speed
        
        self.sample_rate = sample_rate

    def __call__(self, audio_data, gt_audio):
        T = audio_data.shape[-1]
        speed_factor = torch.rand((1,)).item() * (self.max_speed - self.min_speed) + self.min_speed

        # Preferred path: SoX effects (Linux/macOS builds).
        # Windows torchaudio does not provide sox_effects, so use python-soxr fallback.
        try:
            if not hasattr(torchaudio, "sox_effects"):
                raise AttributeError("torchaudio.sox_effects is unavailable in this build")
            sox_effects = [
                ["speed", str(speed_factor)],
                ["rate", str(self.sample_rate)],
            ]
            transformed_audio, _ = torchaudio.sox_effects.apply_effects_tensor(
                audio_data, self.sample_rate, sox_effects
            )
            gt_audio, _ = torchaudio.sox_effects.apply_effects_tensor(
                gt_audio, self.sample_rate, sox_effects
            )
        except Exception:
            transformed_audio = self._speed_fallback(audio_data, speed_factor)
            gt_audio = self._speed_fallback(gt_audio, speed_factor)
        
        # Adjust size so it is the same as the original size
        if transformed_audio.shape[-1] > T:
            transformed_audio = transformed_audio[..., :T]
            gt_audio = gt_audio[..., :T]
        else:
            transformed_audio = F.pad(transformed_audio, (0, T - transformed_audio.shape[-1]))
            gt_audio = F.pad(gt_audio, (0, T - gt_audio.shape[-1]))

        assert transformed_audio.shape[-1] == T
        assert gt_audio.shape[-1] == T

        return transformed_audio, gt_audio

    @staticmethod
    def _speed_fallback(waveform: torch.Tensor, speed_factor: float) -> torch.Tensor:
        """
        Cross-platform speed perturbation fallback.
        Uses python-soxr when available, otherwise linear interpolation.
        """
        if soxr is not None:
            # soxr runs on CPU numpy arrays; preserve dtype/device on return.
            x_np = waveform.detach().cpu().numpy().astype(np.float32, copy=False)
            out_channels = []
            in_rate = float(waveform.shape[-1])
            out_rate = max(1.0, in_rate / float(speed_factor))
            for ch in x_np:
                y = soxr.resample(ch, in_rate, out_rate)
                out_channels.append(torch.from_numpy(np.asarray(y, dtype=np.float32)))
            stretched = torch.stack(out_channels, dim=0).to(waveform.device)
            return stretched

        t_in = waveform.shape[-1]
        t_out = max(1, int(round(t_in / speed_factor)))
        return F.interpolate(
            waveform.unsqueeze(0),
            size=t_out,
            mode="linear",
            align_corners=False,
        ).squeeze(0)
