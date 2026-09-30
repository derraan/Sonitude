import os
import unittest

import torch

from src.hl_modules.distance_based_hl_module import PLModule
from src.models.tfgridnet_realtime_clean_optim.net import Net
from src.models.tfgridnet_realtime_clean_optim.tfgridnet_causal import CountDistanceHead


ASSET_ROOT = os.environ.get("SOUNDBUBBLE_ASSET_ROOT", "C:/Users/darre/Sound_Bubble")
OLD_CKPT_1P5 = os.path.join(ASSET_ROOT, "runs", "optim_pretrain_1_5m", "checkpoints", "best.pt")


def _base_model_params():
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
    }


class CountDistanceHeadModelTests(unittest.TestCase):
    def test_disabled_head_buffers_unchanged(self):
        model = Net(**_base_model_params())
        state = model.init_buffers(batch_size=1, device=torch.device("cpu"))
        self.assertNotIn("count_distance_head_buf", state)
        self.assertIn("conv_buf", state)
        self.assertIn("deconv_buf", state)
        self.assertIn("istft_buf", state)
        self.assertIn("gridnet_bufs", state)

    def test_count_distance_head_causality_matches_split_stream(self):
        torch.manual_seed(0)
        head = CountDistanceHead(
            in_dim=16,
            hidden_dim=12,
            num_slots=2,
            fast_tau_s=0.35,
            slow_tau_s=1.2,
            hop_samples=192,
            sample_rate=24000,
        ).eval()
        features = torch.randn(2, 16, 9, 11)
        state0 = torch.zeros(2, 24)

        full_out, full_state = head(features, state0)
        part1_out, state1 = head(features[:, :, :4, :], state0)
        part2_out, state2 = head(features[:, :, 4:, :], state1)

        for key in ("speaker_count_logits", "spk_distance_m", "spk_active_logit", "spk_active"):
            stitched = torch.cat([part1_out[key], part2_out[key]], dim=1)
            self.assertTrue(torch.allclose(full_out[key], stitched, atol=1e-6, rtol=1e-6))
        self.assertTrue(torch.allclose(full_state, state2, atol=1e-6, rtol=1e-6))

    def test_legacy_head_configs_are_rejected(self):
        params = _base_model_params()
        params["distance_head"] = {"enabled": True}
        params["count_distance_head"] = {"enabled": True}
        with self.assertRaises(ValueError):
            Net(**params)

        params = _base_model_params()
        params["speaker_head"] = {"enabled": True}
        with self.assertRaises(ValueError):
            Net(**params)

    def test_audio_path_identical_with_enabled_count_distance_head(self):
        if not os.path.exists(OLD_CKPT_1P5):
            self.skipTest(f"missing checkpoint: {OLD_CKPT_1P5}")

        state = torch.load(OLD_CKPT_1P5, map_location="cpu")["model"]

        base = Net(**_base_model_params()).eval()
        base.load_state_dict(state, strict=True)

        params = _base_model_params()
        params["count_distance_head"] = {
            "enabled": True,
            "hidden_dim": 16,
            "num_slots": 2,
            "fast_tau_s": 0.35,
            "slow_tau_s": 1.2,
        }
        with_head = Net(**params).eval()
        missing, unexpected = with_head.load_state_dict(state, strict=False)
        self.assertEqual([], [k for k in unexpected if "count_distance_head" not in k])
        self.assertGreater(len([k for k in missing if "count_distance_head" in k]), 0)

        torch.manual_seed(0)
        x = torch.randn(1, 6, 288)
        out_base = base({"mixture": x}, pad=False)["output"]
        out_head = with_head({"mixture": x}, pad=False)["output"]
        self.assertTrue(torch.equal(out_base, out_head))

    def test_old_checkpoint_loads_backbone_with_enabled_count_distance_head(self):
        if not os.path.exists(OLD_CKPT_1P5):
            self.skipTest(f"missing checkpoint: {OLD_CKPT_1P5}")
        model_params = _base_model_params()
        model_params["count_distance_head"] = {
            "enabled": True,
            "hidden_dim": 16,
            "num_slots": 2,
            "fast_tau_s": 0.35,
            "slow_tau_s": 1.2,
        }
        pl = PLModule(
            model="src.models.tfgridnet_realtime_clean_optim.net.Net",
            model_params=model_params,
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
        self.assertTrue(pl.model.tfgridnet.count_distance_head_enabled)


if __name__ == "__main__":
    unittest.main()
