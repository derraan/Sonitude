# Sonitude_SoundBubble

Private Sonitude archive of Sound Bubble work: Raspberry Pi optim model, **count–distance head**, training/eval configs, ONNX exports, and edge/RPi realtime runtime.

## What’s included

- Latest model code (`src/models/tfgridnet_realtime_clean_optim`) with `count_distance_head`
- Training configs under `real_experiments/` (including stopband joint finetune)
- Checkpoints under `runs/` (optim pretrain radii + count–distance head)
- Edge runtime: ONNX zoo, streamer/pipeline, RPi5 scripts (`edge/rpi5`), realtime helpers
- Docs: `README_COUNT_DISTANCE.md`, `edge/README_EDGE.md`, etc.

## What’s not included

Raw audio datasets (~100GB+) live on **Google Drive**, not GitHub. See `datasets/README.md` for the Drive layout and local sync paths.

## Quick train (count–distance head, 1.5 m)

```powershell
python -m src.train_pt --config real_experiments\count_distance_head_optim_1_5m.json --run_dir runs\count_distance_head_optim_1_5m --no_wandb
```

## Eval

```powershell
python -m src.eval_count_distance `
  datasets\syn_test\syn_1_5m\test `
  runs\optim_pretrain_1_5m `
  runs\count_distance_head_optim_1_5m `
  runs\speaker_eval_1_5m `
  --distance_threshold 1.5 --use_cuda
```

