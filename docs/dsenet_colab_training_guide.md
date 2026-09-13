# DSENet Colab Training Guide (Sonitude 6-mic)

This guide explains how to train the DSENet Sonitude adaptation in Google Colab using:

- Dataset A: synthetic LibriSpeech with paper-style priors
- Dataset B: synthetic LibriSpeech with Sound Bubble metadata-derived priors

The notebook this guide refers to is:

- `python/dsenet/notebooks/dsenet_colab_training.ipynb`

## 1) Before You Start

- Google account with access to [Google Colab](https://colab.research.google.com/)
- Optional but recommended: Google Drive storage for artifacts
- Sonitude repository accessible in Colab (`git clone` or uploaded folder)
- Runtime: `GPU` recommended

Notes:

- This workflow is a Sonitude 6-mic adaptation (`44.1 kHz`, 2 ms hop via `L=88`).
- Sound Bubble packaged mixtures are not directly supervised targets for DSENet; metadata is used as an acoustic prior to synthesize supervised data.

## 2) Open the Notebook in Colab

1. Open Colab.
2. Upload `python/dsenet/notebooks/dsenet_colab_training.ipynb`, or open from GitHub if hosted.
3. Set runtime: `Runtime -> Change runtime type -> GPU`.

## 3) Configure Repository Access

In the setup cell:

- Set `RUN_CLONE = True` and update `REPO_URL` if the repo is not already in `/content/Sonitude`.
- If your repo is already present, keep `RUN_CLONE = False`.

The setup cell installs:

- `python/dsenet/requirements.txt`
- `torchaudio`

## 4) Configure Training Settings

In the config cell (`ColabConfig`), set:

- Model/runtime:
  - `sample_rate_hz = 44100`
  - `num_mics = 6`
  - `hop_size = 88`
  - `lookback = 88`
  - `lookahead = 88`
  - `hidden_size = 128`
- Training:
  - `batch_size`, `epochs`, `learning_rate`
- Dataset sizes:
  - `synth_a_train/val/test`
  - `synth_b_train/val/test`
- Toggles:
  - `use_synth_paper = True`
  - `use_soundbubble_style = True`
- Paths:
  - `librispeech_root`, `dataset_a_root`, `dataset_b_root`, `artifact_root`
  - optional `drive_root` for export

If you want a quick smoke run, reduce all sample counts and epochs.

## 5) Dataset Loading Paths and Behavior

## 5.1 LibriSpeech Input

The notebook downloads a subset of LibriSpeech automatically using `torchaudio`.

- Source data ends up under `CFG.librispeech_root`
- Generated samples are `.npy` arrays plus per-split `manifest.json`

## 5.2 Sound Bubble Metadata Prior Input

In the Drive cell, set:

- `SOUNDBUBBLE_ROOT = Path('/content/drive/MyDrive/Sound_Bubble/datasets/syn_1m/syn_1m')`

Change this path if your dataset is elsewhere.

What is read from Sound Bubble:

- `metadata.json` files (room info, RT60, voice/mic positions)

What is **not** used directly:

- `mixture.wav` as supervised label source (no isolated source targets in packaged split)

## 6) Run Order (Top to Bottom)

Execute all cells in order:

1. Intro/scope
2. Setup + install + GPU check
3. Config
4. Geometry load
5. LibriSpeech download
6. Generalized generator definitions
7. Dataset A generation
8. Drive mount + Sound Bubble prior extraction
9. Dataset B generation and resampling (24 kHz -> 44.1 kHz)
10. Combined loader creation
11. Training model build (SI-SDR objective)
12. Train + best checkpoint load
13. Eta estimation
14. Weight transfer + TFLite export + metadata write
15. Streaming parity gate
16. Optional test metrics
17. Optional copy artifacts to Drive
18. Caveats/blockers notes

## 7) Expected Outputs

After a successful run, the main artifacts are:

- `dsenet_filter_estimator.keras`
- `dsenet_streaming_step.tflite`
- `dsenet_streaming_step.dsenet.json`
- `combined_manifest.json`

Default local artifact location:

- `CFG.artifact_root` and `CFG.combined_root`

Optional export location:

- `CFG.drive_root`

## 8) How to Use Your Own Dataset Sizes

To scale up training:

- Increase `synth_a_*` and `synth_b_*` counts
- Increase `epochs`
- Keep an eye on Colab RAM and execution time

To run only one dataset source:

- Only synthetic paper prior:
  - `use_synth_paper = True`
  - `use_soundbubble_style = False`
- Only Sound Bubble prior synthetic:
  - `use_synth_paper = False`
  - `use_soundbubble_style = True`

At least one toggle must remain `True`.

## 9) Validation Checklist

Before exporting to Sonitude runtime, verify:

- Training cell completed without NaN loss
- Eta value computed (`eta`)
- Streaming parity cell passes (`assert_allclose` across hops)
- Metadata includes:
  - `num_mics = 6`
  - geometry IDs/order
  - `L`, `Lp`, `Lf`, `H`
  - tensor names and shapes
  - `trained = true`

## 10) Troubleshooting

- `Repo not found at /content/Sonitude`:
  - Set `RUN_CLONE = True` and valid `REPO_URL`, or upload repo to `/content`.
- `pyroomacoustics import failed`:
  - Re-run setup install cell; restart runtime if needed.
- `Missing path` for Sound Bubble:
  - Correct `SOUNDBUBBLE_ROOT` to your Drive location.
- OOM or very slow training:
  - Lower sample counts, `batch_size`, or `epochs`.
- Parity failure:
  - Ensure weight transfer happened after loading best checkpoint.
  - Re-run export and parity cells in sequence.
- C++ runtime metadata rejection later:
  - Ensure sample rate, geometry order, and tensor metadata exactly match Sonitude runtime config.

## 11) Next Step After Colab

Use the exported:

- `dsenet_streaming_step.tflite`
- `dsenet_streaming_step.dsenet.json`

with Sonitude DSENet mode, then run native runtime verification and benchmark on target hardware (Pi 5) using the project tooling.
