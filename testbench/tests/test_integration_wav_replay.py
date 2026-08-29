"""End-to-end integration test against the REAL sonitude_wav_replay binary.

When SONITUDE_REQUIRE_CPP=1 (CI), a missing binary is a failure, not a skip.
"""

from __future__ import annotations

from pathlib import Path

import numpy as np
import pytest
import soundfile as sf
import yaml

from app.audio_io import audio_loader
from app.config_reader import DEFAULT_CONFIG_PATH, read_runtime_config_summary
from app.processing.batch_adapter import run_wav_replay
from app.processing.capabilities import query_tool_capabilities
from app.processing.suppression import resolve_suppression
from app.storage.models import BinauralRequest, SteeringEvent


@pytest.fixture
def six_channel_fixture(tmp_path: Path) -> Path:
    config_summary = read_runtime_config_summary(DEFAULT_CONFIG_PATH)
    sample_rate = config_summary.capture_sample_rate_hz
    duration_s = 1.0
    n = int(sample_rate * duration_s)
    t = np.linspace(0, duration_s, n, endpoint=False)
    tone = 0.3 * np.sin(2 * np.pi * 400 * t)
    rng = np.random.default_rng(7)
    data = np.stack([tone + rng.normal(0, 0.02, n) for _ in range(6)], axis=1).astype(np.float32)
    path = tmp_path / "fixture_6ch.wav"
    sf.write(str(path), data, sample_rate, subtype="FLOAT")
    return path


def test_wav_replay_produces_all_expected_outputs(
    six_channel_fixture: Path, tmp_path: Path, wav_replay_binary: Path
) -> None:
    events = [SteeringEvent(time_s=0.0, azimuth_deg=0.0, elevation_deg=0.0)]
    result = run_wav_replay(
        six_channel_fixture,
        DEFAULT_CONFIG_PATH,
        events,
        tmp_path / "out",
        binary_path=wav_replay_binary,
    )
    assert Path(result.command[0]).resolve() == wav_replay_binary.resolve()
    assert wav_replay_binary.exists()
    assert "sonitude_wav_replay" in wav_replay_binary.name
    assert "sonitude_resolved" in result.stderr
    assert result.output_wav.exists()
    assert result.beamformed_wav.exists()
    assert result.suppressed_wav.exists()

    beamformed, sr = audio_loader.load_wav(result.beamformed_wav)
    assert sr == read_runtime_config_summary(DEFAULT_CONFIG_PATH).capture_sample_rate_hz
    assert len(beamformed) > 0
    assert np.any(beamformed != 0.0)


def test_suppression_actually_changes_the_output(
    six_channel_fixture: Path, tmp_path: Path, wav_replay_binary: Path
) -> None:
    events = [SteeringEvent(time_s=0.0, azimuth_deg=0.0, elevation_deg=0.0)]

    without_suppression = run_wav_replay(
        six_channel_fixture,
        DEFAULT_CONFIG_PATH,
        events,
        tmp_path / "no_suppression",
        suppression="off",
        binary_path=wav_replay_binary,
    )
    with_suppression = run_wav_replay(
        six_channel_fixture,
        DEFAULT_CONFIG_PATH,
        events,
        tmp_path / "with_suppression",
        suppression="on",
        binary_path=wav_replay_binary,
    )

    assert without_suppression.suppression_requested == "off"
    assert with_suppression.suppression_requested == "on"
    assert without_suppression.resolved.get("suppression_resolved") is False
    assert with_suppression.resolved.get("suppression_resolved") is True

    beamformed_a, _ = audio_loader.load_wav(without_suppression.beamformed_wav)
    beamformed_b, _ = audio_loader.load_wav(with_suppression.beamformed_wav)
    np.testing.assert_allclose(beamformed_a, beamformed_b, atol=1e-6)

    suppressed_without, _ = audio_loader.load_wav(without_suppression.suppressed_wav)
    suppressed_with, _ = audio_loader.load_wav(with_suppression.suppressed_wav)
    assert not np.allclose(suppressed_without, suppressed_with, atol=1e-6), (
        "enabling suppression should change the post-suppression tap relative to suppression disabled"
    )


def test_cli_off_overrides_yaml_enabled(
    six_channel_fixture: Path, tmp_path: Path, wav_replay_binary: Path
) -> None:
    config_dir = DEFAULT_CONFIG_PATH.parent
    raw = yaml.safe_load(DEFAULT_CONFIG_PATH.read_text(encoding="utf-8"))
    raw["suppression"]["enabled"] = True
    raw["geometry_path"] = str((config_dir / raw["geometry_path"]).resolve())
    raw["calibration_path"] = str((config_dir / raw["calibration_path"]).resolve())
    yaml_on = tmp_path / "suppression_on.yaml"
    yaml_on.write_text(yaml.safe_dump(raw), encoding="utf-8")
    events = [SteeringEvent(time_s=0.0, azimuth_deg=0.0, elevation_deg=0.0)]

    auto = run_wav_replay(
        six_channel_fixture, yaml_on, events, tmp_path / "auto", suppression="auto", binary_path=wav_replay_binary
    )
    forced_off = run_wav_replay(
        six_channel_fixture, yaml_on, events, tmp_path / "off", suppression="off", binary_path=wav_replay_binary
    )
    python_resolved = resolve_suppression("off", yaml_enabled=True)
    assert python_resolved.resolved_enabled is False
    assert auto.resolved.get("suppression_resolved") is True
    assert forced_off.resolved.get("suppression_resolved") is False

    auto_tap, _ = audio_loader.load_wav(auto.suppressed_wav)
    off_tap, _ = audio_loader.load_wav(forced_off.suppressed_wav)
    assert not np.allclose(auto_tap, off_tap, atol=1e-6)


def test_directivity_blend_moves_output_toward_omnidirectional(
    six_channel_fixture: Path, tmp_path: Path, wav_replay_binary: Path
) -> None:
    narrow = [SteeringEvent(time_s=0.0, azimuth_deg=0.0, elevation_deg=0.0, width_deg=0.0)]
    wide = [SteeringEvent(time_s=0.0, azimuth_deg=0.0, elevation_deg=0.0, width_deg=180.0)]

    narrow_result = run_wav_replay(
        six_channel_fixture, DEFAULT_CONFIG_PATH, narrow, tmp_path / "narrow", binary_path=wav_replay_binary
    )
    wide_result = run_wav_replay(
        six_channel_fixture, DEFAULT_CONFIG_PATH, wide, tmp_path / "wide", binary_path=wav_replay_binary
    )

    narrow_beam, _ = audio_loader.load_wav(narrow_result.beamformed_wav)
    wide_beam, _ = audio_loader.load_wav(wide_result.beamformed_wav)
    assert not np.allclose(narrow_beam, wide_beam, atol=1e-6), "blend=180 should differ from blend=0"


def test_flac_input_is_decoded_then_processed(
    tmp_path: Path, wav_replay_binary: Path
) -> None:
    config_summary = read_runtime_config_summary(DEFAULT_CONFIG_PATH)
    sample_rate = config_summary.capture_sample_rate_hz
    n = int(sample_rate * 0.5)
    t = np.linspace(0, 0.5, n, endpoint=False)
    tone = 0.2 * np.sin(2 * np.pi * 300 * t)
    data = np.stack([tone for _ in range(6)], axis=1).astype(np.float32)
    flac_path = tmp_path / "six.flac"
    sf.write(str(flac_path), data, sample_rate, format="FLAC", subtype="PCM_24")
    result = run_wav_replay(
        flac_path,
        DEFAULT_CONFIG_PATH,
        [SteeringEvent(0.0, 0.0, 0.0)],
        tmp_path / "flac_out",
        binary_path=wav_replay_binary,
    )
    assert result.output_wav.exists()
    assert result.decoded_input_wav.suffix == ".wav"


def test_compact_hrtf_binaural_is_stereo_and_not_lr_duplicate(
    six_channel_fixture: Path, tmp_path: Path, wav_replay_binary: Path
) -> None:
    caps = query_tool_capabilities("sonitude_wav_replay", binary_path=wav_replay_binary)
    if not caps.binaural.backend_supported("compact_hrtf"):
        pytest.skip("this sonitude_wav_replay build does not advertise compact_hrtf")

    events = [SteeringEvent(time_s=0.0, azimuth_deg=90.0, elevation_deg=0.0)]
    result = run_wav_replay(
        six_channel_fixture,
        DEFAULT_CONFIG_PATH,
        events,
        tmp_path / "hrtf",
        binaural=BinauralRequest(
            enabled=True,
            backend="compact_hrtf",
            follow_beamformer_steering=True,
        ),
        binary_path=wav_replay_binary,
    )
    assert result.binaural_wav is not None and result.binaural_wav.exists()
    assert "--binaural-backend" in result.command
    assert "compact_hrtf" in result.command
    assert result.resolved.get("binaural_backend") == "compact_hrtf"
    stereo, _ = audio_loader.load_wav(result.binaural_wav)
    assert stereo.ndim == 2
    assert stereo.shape[1] == 2
    assert not np.allclose(stereo[:, 0], stereo[:, 1], atol=1e-6), (
        "compact_hrtf must not be an L=R duplicate of directional mono"
    )


def test_spectral_backend_is_experimental_and_off_is_bit_exact(
    six_channel_fixture: Path, tmp_path: Path, wav_replay_binary: Path
) -> None:
    events = [SteeringEvent(time_s=0.0, azimuth_deg=0.0, elevation_deg=0.0)]
    off = run_wav_replay(
        six_channel_fixture,
        DEFAULT_CONFIG_PATH,
        events,
        tmp_path / "off",
        suppression="off",
        binary_path=wav_replay_binary,
    )
    spectral = run_wav_replay(
        six_channel_fixture,
        DEFAULT_CONFIG_PATH,
        events,
        tmp_path / "spectral",
        suppression="on",
        suppression_backend="spectral",
        binary_path=wav_replay_binary,
        disable_limiter=True,
    )
    conservative = run_wav_replay(
        six_channel_fixture,
        DEFAULT_CONFIG_PATH,
        events,
        tmp_path / "cons",
        suppression="on",
        suppression_backend="conservative",
        binary_path=wav_replay_binary,
        disable_limiter=True,
    )
    assert off.resolved.get("suppression_backend_resolved") == "off"
    assert off.resolved.get("suppression_algorithmic_delay_samples") in (0, 0.0)
    assert spectral.resolved.get("suppression_backend_resolved") == "spectral"
    assert spectral.resolved.get("suppression_implementation_status") == "EXPERIMENTAL"
    assert spectral.resolved.get("suppression_fft_size") == 128
    assert spectral.resolved.get("suppression_hop_size") == 32
    assert spectral.resolved.get("suppression_algorithmic_delay_samples") == 127
    beam_off, _ = audio_loader.load_wav(off.beamformed_wav)
    supp_off, _ = audio_loader.load_wav(off.suppressed_wav)
    assert np.array_equal(beam_off, supp_off), "disabled backend must match the beamformed tap exactly"
    supp_spec, _ = audio_loader.load_wav(spectral.suppressed_wav)
    supp_cons, _ = audio_loader.load_wav(conservative.suppressed_wav)
    assert not np.allclose(supp_spec, supp_cons, atol=1e-6), (
        "spectral and conservative backends must not be identical when both are on"
    )
