import json
import os
import tempfile
import unittest
from unittest import mock

import numpy as np
import soundfile as sf
import torch

from src.datasets.general_multisrc_dataset_with_perturbations import Dataset
from src.hl_modules.distance_based_hl_module import PLModule


ASSET_ROOT = os.environ.get("SOUNDBUBBLE_ASSET_ROOT", "C:/Users/darre/Sound_Bubble")
OLD_CKPT_1P5 = os.path.join(ASSET_ROOT, "runs", "optim_pretrain_1_5m", "checkpoints", "best.pt")


def _head_model_params():
    return {
        "stft_chunk_size": 192,
        "stft_pad_size": 96,
        "num_ch": 6,
        "D": 16,
        "L": 4,
        "I": 1,
        "J": 1,
        "B": 3,
        "H": 64,
        "E": 2,
        "conv_lstm": True,
        "lstm_down": 5,
        "local_atten_len": 50,
        "use_attn": False,
        "lookahead": True,
        "chunk_causal": True,
        "use_first_ln": True,
        "merge_method": "early_cat",
        "count_distance_head": {
            "enabled": True,
            "hidden_dim": 16,
            "num_slots": 2,
            "fast_tau_s": 0.35,
            "slow_tau_s": 1.2,
        },
    }


def _write_sample(sample_dir: str, distances, real: bool = False, sr: int = 24000):
    os.makedirs(sample_dir, exist_ok=True)
    num_samples = 1024
    mixture = np.zeros((num_samples, 6), dtype=np.float32)
    sf.write(os.path.join(sample_dir, "mixture.wav"), mixture, sr)

    metadata = {"real": real, "n_BG": 0}
    for i in range(6):
        metadata[f"mic{i:02d}"] = {"position": [0.0, 0.0, 0.0]}

    t = np.linspace(0.0, 1.0, num_samples, dtype=np.float32)
    for idx, d in enumerate(distances):
        voice_key = f"voice{idx:02d}"
        metadata[voice_key] = {"dis": int(d * 100) if real else float(d), "angle": 0.0}
        wav = np.sin(2 * np.pi * (200 + 30 * idx) * t).astype(np.float32)
        sf.write(os.path.join(sample_dir, f"mic00_{voice_key}.wav"), wav, sr)

    with open(os.path.join(sample_dir, "metadata.json"), "w", encoding="utf-8") as f:
        json.dump(metadata, f)


def _sf_torch_load(path: str, downsample=1):
    wav, _ = sf.read(path, dtype="float32", always_2d=True)
    wav = torch.from_numpy(wav.T.copy())
    if downsample > 1:
        wav = wav[:, ::downsample]
    return wav


class SpeakerDatasetLossTests(unittest.TestCase):
    def test_dataset_speaker_labels_and_units(self):
        with tempfile.TemporaryDirectory() as td:
            d1 = os.path.join(td, "00000")
            d2 = os.path.join(td, "00001")
            d3 = os.path.join(td, "00002")
            _write_sample(d1, [1.2], real=False)
            _write_sample(d2, [1.4, 1.1], real=False)
            _write_sample(d3, [1.5], real=True)  # stored as 150 cm

            with mock.patch(
                "src.datasets.general_multisrc_dataset_with_perturbations.utils.read_audio_file_torch",
                side_effect=_sf_torch_load,
            ):
                ds = Dataset(
                    dataset_dirs=[{"path": td, "max_samples": 10}],
                    dis_threshold=1.6,
                    mic_config=["mic00", "mic01", "mic02", "mic03", "mic04", "mic05"],
                    downsample=1,
                    split="val",
                )
                self.assertEqual(len(ds), 3)

                _, tgt1 = ds[0]
                _, tgt2 = ds[1]
                _, tgt3 = ds[2]

            self.assertEqual(int(tgt1["num_target_speakers_clamped"]), 1)
            self.assertTrue(torch.allclose(tgt1["spk_valid"], torch.tensor([1.0, 0.0])))
            self.assertTrue(torch.allclose(tgt1["spk_distance_m"], torch.tensor([1.2, 0.0]), atol=1e-4))

            self.assertEqual(int(tgt2["num_target_speakers_clamped"]), 2)
            self.assertTrue(torch.allclose(tgt2["spk_valid"], torch.tensor([1.0, 1.0])))
            self.assertTrue(torch.allclose(tgt2["spk_distance_m"], torch.tensor([1.1, 1.4]), atol=1e-4))
            self.assertEqual(tuple(tgt2["spk_ref"].shape), (2, 1024))

            self.assertEqual(int(tgt3["num_target_speakers_clamped"]), 1)
            self.assertTrue(torch.allclose(tgt3["spk_distance_m"], torch.tensor([1.5, 0.0]), atol=1e-4))
            self.assertTrue(torch.allclose(tgt3["spk_valid"], torch.tensor([1.0, 0.0])))

    def test_masked_speaker_losses_handle_empty_supervision(self):
        if not os.path.exists(OLD_CKPT_1P5):
            self.skipTest(f"missing checkpoint: {OLD_CKPT_1P5}")
        pl = PLModule(
            model="src.models.tfgridnet_realtime_clean_optim.net.Net",
            model_params=_head_model_params(),
            sr=24000,
            optimizer="torch.optim.Adam",
            optimizer_params={"lr": 1e-3},
            loss="src.losses.SNRLP.SNRLPLoss",
            loss_params={"snr_loss_name": "snr", "neg_weight": 50},
            metrics=[],
            init_ckpt=OLD_CKPT_1P5,
            use_dp=False,
            count_distance_head={
                "enabled": True,
                "freeze_backbone": True,
                "w_count": 1.0,
                "w_distance": 1.0,
                "w_activity": 0.2,
                "vad_threshold_db": -35.0,
            },
        )
        mixture = torch.randn(2, 6, 288)
        targets = {
            "target": torch.zeros(2, 1, 288),
            "targets_outside": torch.zeros(2, 1, 288),
            "num_target_speakers": torch.tensor([0, 2]),
            "num_target_speakers_clamped": torch.tensor([0, 2]),
            "num_interfering_speakers": torch.tensor([2, 0]),
            "num_noises": torch.tensor([0, 0]),
            "distance_m": torch.tensor([0.0, 0.0]),
            "distance_valid": torch.tensor([0.0, 0.0]),
            "spk_distance_m": torch.tensor([[0.0, 0.0], [1.0, 1.4]]),
            "spk_valid": torch.tensor([[0.0, 0.0], [1.0, 1.0]]),
            "spk_ref": torch.zeros(2, 2, 288),
        }
        inputs = {"mixture": mixture, "reference_channels": [0]}
        loss, _ = pl._step((inputs, targets), 0, step="train")
        self.assertTrue(torch.isfinite(loss))

    def test_freeze_backbone_only_count_distance_head_trainable(self):
        if not os.path.exists(OLD_CKPT_1P5):
            self.skipTest(f"missing checkpoint: {OLD_CKPT_1P5}")
        pl = PLModule(
            model="src.models.tfgridnet_realtime_clean_optim.net.Net",
            model_params=_head_model_params(),
            sr=24000,
            optimizer="torch.optim.Adam",
            optimizer_params={"lr": 1e-3},
            loss="src.losses.SNRLP.SNRLPLoss",
            loss_params={"snr_loss_name": "snr", "neg_weight": 50},
            metrics=[],
            init_ckpt=OLD_CKPT_1P5,
            use_dp=False,
            count_distance_head={"enabled": True, "freeze_backbone": True},
        )
        trainable = [name for name, p in pl.model.named_parameters() if p.requires_grad]
        self.assertGreater(len(trainable), 0)
        self.assertTrue(all("count_distance_head" in name for name in trainable))


if __name__ == "__main__":
    unittest.main()
