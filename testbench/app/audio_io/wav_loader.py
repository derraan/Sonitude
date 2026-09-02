"""Compatibility wrapper. Prefer app.audio_io.audio_loader."""

from app.audio_io.audio_loader import *  # noqa: F403
from app.audio_io.audio_loader import (  # noqa: F401
    CodecCapabilityError,
    CodecError,
    REQUIRED_CHANNELS,
    extract_channels,
    find_wav_files,
    load_wav,
    probe_audio,
    read_wav_metadata,
    validate_dsp_input,
    validate_six_channel_wav,
)
