from __future__ import annotations

from pathlib import Path

from app.config_reader import DEFAULT_CONFIG_PATH, read_runtime_config_summary
from app.processing.batch_adapter import binaural_cli_args
from app.processing.capabilities import parse_capabilities_json, preferred_binaural_backend
from app.storage.models import BinauralRequest


def test_preferred_backend_skips_mono_reference_when_hrtf_exists() -> None:
    available = ["mono_reference", "itd_ild", "compact_hrtf", "full_hrtf_reference"]
    assert preferred_binaural_backend(available, "mono_reference") == "compact_hrtf"
    assert preferred_binaural_backend(available, "itd_ild") == "itd_ild"


def test_parse_capabilities_json_lists_hrtf_backends() -> None:
    caps = parse_capabilities_json(
        {
            "protocol_version": 2,
            "suppression": {"modes": ["auto", "on", "off"]},
            "taps": ["beamformed", "suppressed", "processed", "binaural"],
            "binaural": {
                "available": True,
                "backends": ["mono_reference", "itd_ild", "compact_hrtf", "full_hrtf_reference"],
                "unavailable_backends": [],
                "note": "ITD/ILD and SADIE II D2 HRTF tables are implemented.",
            },
        }
    )
    assert caps.queried is True
    assert caps.binaural.backend_supported("compact_hrtf")
    assert caps.binaural.unavailable_backends == []


def test_config_reader_exposes_binaural_yaml() -> None:
    summary = read_runtime_config_summary(DEFAULT_CONFIG_PATH)
    assert summary.binaural.backend
    assert summary.binaural.follow_steering is True
    assert "generic_sadie2_d2" in summary.binaural.table_path


def test_binaural_cli_args_pass_backend_and_follow() -> None:
    wav = Path("binaural_stereo.wav")
    assert binaural_cli_args(BinauralRequest(enabled=False), wav) == []
    follow = binaural_cli_args(
        BinauralRequest(enabled=True, backend="compact_hrtf", follow_beamformer_steering=True),
        wav,
    )
    assert follow[:4] == ["--output-binaural", str(wav), "--binaural-backend", "compact_hrtf"]
    assert "--binaural-follow-steering" in follow
    fixed = binaural_cli_args(
        BinauralRequest(
            enabled=True,
            backend="itd_ild",
            follow_beamformer_steering=False,
            azimuth_deg=45.0,
            elevation_deg=-10.0,
        ),
        wav,
    )
    assert "--binaural-fixed-direction" in fixed
    assert "45.000000" in fixed
    assert "-10.000000" in fixed
