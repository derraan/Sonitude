# Sound_Bubble Quick Start (Windows + Conda)

This guide is a practical start-to-finish checklist for running the repo on Windows.

## 1) Open Anaconda Prompt and activate environment

1. Open **Anaconda Prompt** from Start menu.
2. Run:

```bat
conda activate sound_bubble
cd C:\Users\darre\Sound_Bubble
```

If the env does not exist yet:

```bat
conda create --name sound_bubble python=3.8 -y
conda activate sound_bubble
pip install -r requirements2.txt
```

## 2) Quick sanity checks

### 2.1 Check PyTorch + CUDA

```bat
python -c "import torch; print('torch=', torch.__version__); print('cuda=', torch.version.cuda); print('cuda_available=', torch.cuda.is_available())"
```

Expected (for GPU mode): `cuda_available=True`.

### 2.2 List audio devices

```bat
python edge/dummy_realtime.py dummy --list-devices
```

Use the printed device IDs for `--input-device` and `--output-device`.

## 3) Run test dataset evaluation (offline)

### 3.1 Metrics-only run

```bat
python src/test_samples.py ./test_samples/syn_1m/ ./TFG_S_big_newdis_v3_pt_fix_MutiLoss/ --distance_threshold 1 --use_cuda
```

### 3.2 Save audio for one sample

```bat
python src/test_samples.py ./test_samples/syn_1m/ ./TFG_S_big_newdis_v3_pt_fix_MutiLoss/ --distance_threshold 1 --use_cuda --save_id 1
```

Generated files:

- `debug/mix00001.wav` (input mixture)
- `debug/est00001.wav` (model output)
- `debug/gt00001.wav` (ground truth)

Play output:

```bat
start .\debug\est00001.wav
```

## 4) Real-time demo (dummy pipeline)

Script:

- `edge/dummy_realtime.py`

Stop runtime with `Ctrl + C`.

### 4.1 Stable pair (recommended first)

Use laptop mic array + headphone output (replace IDs if your list differs):

```bat
python edge/dummy_realtime.py ./TFG_S_big_newdis_v3_pt_fix_MutiLoss --distance-threshold 1 --input-device 14 --output-device 13 --input-channels 2 --output-channels 2 --model-device cuda --amp --latency high --io-sr 48000 --prefill-blocks 12 --block-multiplier 4 --print-levels --output-gain 6
```

### 4.2 Headset mic path (Bluetooth hands-free profile)

Hands-free devices are usually mono and often require 16 kHz:

```bat
python edge/dummy_realtime.py ./TFG_S_big_newdis_v3_pt_fix_MutiLoss --distance-threshold 1 --input-device 7 --output-device 10 --input-channels 1 --output-channels 2 --model-device cuda --amp --latency high --io-sr 16000 --prefill-blocks 12 --block-multiplier 4 --print-levels --output-gain 8
```

If sample-rate errors appear, try `--io-sr 8000`.

## 5) Useful debug flags for realtime

- `--passthrough` : bypass model, input -> output directly
- `--print-levels` : print infer timing + RMS + underruns
- `--output-gain N` : louder output (`N` = 2, 6, 10, etc.)
- `--mix-dry 0.2` : blend dry signal with model output
- `--prefill-blocks` / `--block-multiplier` : reduce underruns (higher latency)

Example passthrough check:

```bat
python edge/dummy_realtime.py ./TFG_S_big_newdis_v3_pt_fix_MutiLoss --input-device 14 --output-device 13 --input-channels 2 --output-channels 2 --model-device cpu --passthrough --print-levels --output-gain 10
```

## 6) Smaller-model test mode (if you have a small checkpoint)

Small config files exist in:

- `real_experiments/raspberrypi_model_pretrain.json`
- `real_experiments/orangpi_model_pretrain.json`

Run with config + checkpoint:

```bat
python edge/dummy_realtime.py --config-path ./real_experiments/raspberrypi_model_pretrain.json --checkpoint-path "C:\path\to\small\checkpoint.pt" --distance-threshold 1 --input-device 14 --output-device 13 --input-channels 2 --output-channels 2 --model-device cuda --amp --latency high --io-sr 48000 --prefill-blocks 12 --block-multiplier 4 --print-levels --output-gain 6
```

Note: if `--checkpoint-path` is omitted, the script runs with random weights (for speed/path testing only).
