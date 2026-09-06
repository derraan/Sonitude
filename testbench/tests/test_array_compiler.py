import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

from calibration.compile_array import compile_from_irs, make_synthetic_impulse_cube, pack_profile


def test_synthetic_compiler_distortionless_and_hash():
    irs = make_synthetic_impulse_cube(44100, [[0.0, 0.5, 1.0, 1.5, 2.0, 2.5]])
    profile = compile_from_irs(irs, [0.0], 44100)
    w = profile["weights"]
    d = profile["steering"]
    valid = profile["valid"]
    for k in range(profile["bin_count"]):
        if not valid[0, k]:
            continue
        unity = np.vdot(d[0, k], w[0, k])
        assert abs(unity - 1.0) < 1e-3
    blob = pack_profile(profile)
    assert blob[:4] == b"SMV3"
    assert len(blob) > 128
