# Count–Distance Head (speaker # + per-slot distance)

Optional aux head on the Raspberry Pi optim Sound Bubble model that reports:

- inside-bubble **speaker count** classes `0 / 1 / 2+`
- up to **2 nearest-first distance slots** (metres) with per-slot activity

Audio separation behavior is unchanged when the head is enabled (frozen-backbone training).

## Scope

- Model family only: `src.models.tfgridnet_realtime_clean_optim`
- Canonical aux-head path: **`count_distance_head`** (`CountDistanceHead`)
- Legacy names `distance_head` / `speaker_head` are rejected; distance-only and count-only paths are removed
- Backbone-only pretrain configs (`raspberrypi_*`, `orangpi_*`) remain unchanged

## Radius checkpoint mapping

Each count–distance head run starts from the matching optim backbone checkpoint:

| Radius | Backbone `init_ckpt` | Config | Run dir |
|--------|----------------------|--------|---------|
| 1.0 m | `.../runs/optim_pretrain_1m/checkpoints/best.pt` | `real_experiments/count_distance_head_optim_1m.json` | `runs/count_distance_head_optim_1m` |
| 1.5 m | `.../runs/optim_pretrain_1_5m/checkpoints/best.pt` (epoch 93) | `real_experiments/count_distance_head_optim_1_5m.json` | `runs/count_distance_head_optim_1_5m` |
| 2.0 m | `.../runs/optim_pretrain_2m/checkpoints/best.pt` | `real_experiments/count_distance_head_optim_2m.json` | `runs/count_distance_head_optim_2m` |

Optional joint fine-tune: `real_experiments/count_distance_head_optim_1_5m_joint.json`.

Configs keep backbone `model_params` aligned to the pretrain runs (`D=16`, `B=3`, `H=64`, `directional=false`, `spectral_masking=false`, …).

## What the head does

- Tap point: after the last GridNet block (`[B,D,T,F]`), before deconv concat
- Dual causal EMA (`fast_tau_s` for count/activity, `slow_tau_s` for distance) with state `count_distance_head_buf`
- Update interval: one hop (`192` samples @ 24 kHz ≈ 8 ms); no extra look-ahead
- Outputs: `speaker_count_logits`, `spk_distance_m`, `spk_active_logit` / `spk_active`
- Head disabled by default preserves baseline buffers/outputs and headless ONNX layout

## Training losses / metrics

In `src/hl_modules/distance_based_hl_module.py` with `count_distance_head.enabled`:

- `w_count *` CE on count logits
- `w_activity *` BCE on per-slot activity
- `w_distance *` masked SmoothL1 on slot distances (active frames only)
- Logged/printed: `count_loss`, `count_acc`, `spk_distance_loss`, `spk_distance_mae`, `spk_activity_loss`
- With `freeze_backbone: true`, SNRi/SI-SDRi are omitted from the epoch summary

## Train

```powershell
cd C:\Users\darre\Sound_Bubble_optim_distance
python -m src.train_pt --config real_experiments\count_distance_head_optim_1_5m.json --run_dir runs\count_distance_head_optim_1_5m --no_wandb
python -m src.train_pt --config real_experiments\count_distance_head_optim_1m.json   --run_dir runs\count_distance_head_optim_1m   --no_wandb
python -m src.train_pt --config real_experiments\count_distance_head_optim_2m.json   --run_dir runs\count_distance_head_optim_2m   --no_wandb
```

## Eval

```powershell
python -m src.eval_count_distance `
  C:/Users/darre/Sound_Bubble/datasets/syn_test/syn_1_5m/test `
  C:/Users/darre/Sound_Bubble/runs/optim_pretrain_1_5m `
  runs/count_distance_head_optim_1_5m `
  runs/count_distance_eval_1_5m `
  --distance_threshold 1.5 --use_cuda
```

Reports frame/clip count accuracy, slot distance MAE/RMSE/bias on active slots, false-active rates, and SI-SDRi/audio parity vs the frozen backbone.

## ONNX export / benchmark

```powershell
python edge/export_to_onnx.py --run-dir runs/count_distance_head_optim_1_5m --output edge/zoo/optim_count_distance_1_5m.onnx --opset 9 --frames-for-check 3 --batch-size 1
python edge/benchmark.py --model edge/zoo/optim_count_distance_1_5m.onnx --contract-path edge/zoo/optim_count_distance_1_5m.runtime.json --runs 200 --warmup 30 --num-threads 4
```

Headless (no aux head):

```powershell
python edge/export_to_onnx.py --run-dir C:/Users/darre/Sound_Bubble/runs/optim_pretrain_1_5m --output edge/zoo/optim_headless.onnx --opset 9 --frames-for-check 3 --batch-size 1
```

Untrained count–distance fixture exports require `--allow-untrained-count-distance-head` (tests only).

Streamer returns `speaker_count` / `speaker_count_status` and per-slot `speaker_slot_distance_m` / `speaker_slot_status` (`valid|inactive|unavailable`).

## Tests

- `tests/test_count_distance_head.py` — causality, reject stale `distance_head`, audio-path identity
- `tests/test_count_distance_data_loss.py` — labels/masks/losses and freeze-backbone trainable set
- `tests/test_count_distance_onnx.py` — export parity and streamer speaker status

## Limitations

- At most 2 inside-bubble slots; `2+` is a count class, not identity tracking
- Slots are nearest-first by GT distance during training, not stable speaker IDs over time
- Count/distance do not change separator suppression
- Existing backbone checkpoints do not include trained `count_distance_head.*` weights until you train them
