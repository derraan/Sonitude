import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

import numpy as np
import onnx

from edge.pipeline.onnx_streamer import ONNXStreamer
from src.hl_modules.distance_based_hl_module import PLModule


REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
ASSET_ROOT = os.environ.get("SOUNDBUBBLE_ASSET_ROOT", "C:/Users/darre/Sound_Bubble")
HEADLESS_RUN = os.path.join(ASSET_ROOT, "runs", "optim_pretrain_1_5m")
OLD_CKPT_1P5 = os.path.join(ASSET_ROOT, "runs", "optim_pretrain_1_5m", "checkpoints", "best.pt")


def _export(run_dir: str, out_onnx: str, extra_args=None):
    if extra_args is None:
        extra_args = []
    cmd = [
        sys.executable,
        os.path.join(REPO_ROOT, "edge", "export_to_onnx.py"),
        "--run-dir",
        run_dir,
        "--output",
        out_onnx,
        "--frames-for-check",
        "1",
        "--batch-size",
        "1",
        "--opset",
        "9",
    ] + list(extra_args)
    subprocess.run(cmd, cwd=REPO_ROOT, check=True)


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
            "hidden_dim": 32,
            "num_slots": 2,
            "fast_tau_s": 0.35,
            "slow_tau_s": 1.2,
            "sample_rate": 24000,
            "active_threshold": 0.5,
        },
    }


def _build_temp_head_run() -> str:
    td = tempfile.mkdtemp(prefix="sb_count_distance_head_run_")
    run_dir = os.path.join(td, "run")
    os.makedirs(os.path.join(run_dir, "checkpoints"), exist_ok=True)

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
    ckpt_path = os.path.join(run_dir, "checkpoints", "best.pt")
    pl.dump_state(ckpt_path)

    config = {
        "pl_module": "src.hl_modules.distance_based_hl_module.PLModule",
        "pl_module_args": {
            "metrics": ["snr_i", "si_snr_i", "si_sdr_i"],
            "model": "src.models.tfgridnet_realtime_clean_optim.net.Net",
            "model_params": _head_model_params(),
            "count_distance_head": {
                "enabled": True,
                "freeze_backbone": True,
                "w_count": 1.0,
                "w_distance": 1.0,
                "w_activity": 0.2,
                "vad_threshold_db": -35.0,
            },
            "samples_per_speaker_number": 5,
            "optimizer": "torch.optim.Adam",
            "optimizer_params": {"lr": 1e-3},
            "loss": "src.losses.SNRLP.SNRLPLoss",
            "loss_params": {"snr_loss_name": "snr", "neg_weight": 50},
            "scheduler": None,
            "scheduler_params": None,
            "sr": 24000,
            "grad_clip": 1,
        },
        "train_data_args": {"dis_threshold": 1.5},
    }
    with open(os.path.join(run_dir, "config.json"), "w", encoding="utf-8") as f:
        json.dump(config, f, indent=2)

    return run_dir


class SpeakerOnnxTests(unittest.TestCase):
    def setUp(self):
        self._tmp_dirs = []

    def tearDown(self):
        for d in self._tmp_dirs:
            shutil.rmtree(d, ignore_errors=True)

    def _track_tmp(self, path: str) -> str:
        self._tmp_dirs.append(path)
        return path

    def test_headless_export_keeps_original_interface(self):
        if not os.path.isdir(HEADLESS_RUN):
            self.skipTest(f"missing run dir: {HEADLESS_RUN}")
        with tempfile.TemporaryDirectory() as td:
            out = os.path.join(td, "headless.onnx")
            _export(HEADLESS_RUN, out)
            m = onnx.load(out)
            input_names = [i.name for i in m.graph.input]
            output_names = [o.name for o in m.graph.output]
            state_inputs = [name for name in input_names if name.startswith("state_")]
            state_outputs = [name for name in output_names if name.startswith("next_state_")]
            self.assertIn("mixture", input_names)
            self.assertIn("output", output_names)
            self.assertNotIn("dis_embed", input_names)
            self.assertNotIn("speaker_count_logits", output_names)
            self.assertNotIn("spk_distance_m", output_names)
            self.assertNotIn("spk_active", output_names)
            self.assertEqual(9, len(state_inputs))
            self.assertEqual(9, len(state_outputs))

    def test_count_distance_head_export_emits_outputs_and_streamer_status(self):
        if not os.path.exists(OLD_CKPT_1P5):
            self.skipTest(f"missing checkpoint: {OLD_CKPT_1P5}")
        run_dir = self._track_tmp(_build_temp_head_run())
        with tempfile.TemporaryDirectory() as td:
            out = os.path.join(td, "count_distance_head.onnx")
            _export(run_dir, out)
            m = onnx.load(out)
            input_names = [i.name for i in m.graph.input]
            output_names = [o.name for o in m.graph.output]
            self.assertIn("speaker_count_logits", output_names)
            self.assertIn("spk_distance_m", output_names)
            self.assertIn("spk_active", output_names)
            self.assertNotIn("dis_embed", input_names)
            self.assertTrue(any(name.startswith("state_") for name in input_names))

            streamer = ONNXStreamer(out, intra_threads=1, inter_threads=1)
            mixture = np.random.randn(1, 6, 288).astype(np.float32)

            streamer.set_count_distance_runtime_config(trained=False)
            unavailable = streamer.run(mixture)
            self.assertEqual("unavailable", unavailable.speaker_count_status)
            self.assertIsNone(unavailable.speaker_count)

            streamer.set_count_distance_runtime_config(trained=True, active_threshold=1.1)
            inactive = streamer.run(mixture)
            self.assertEqual("inactive", inactive.speaker_count_status)
            self.assertIsNotNone(inactive.speaker_count)
            self.assertIsNotNone(inactive.speaker_slot_status)
            self.assertTrue(all(status == "inactive" for status in inactive.speaker_slot_status))

            streamer.reset_state()
            for v in streamer.state_arrays.values():
                self.assertTrue(np.allclose(v, 0.0))


if __name__ == "__main__":
    unittest.main()
