# Training Instructions — Small TFGridNet Optim Model

This runbook is for training `src.models.tfgridnet_realtime_clean_optim.net.Net`
on Windows first, with a clean handoff path for later RPi porting.

## 1) Prerequisites

- Repository root: `C:\Users\darre\Sound_Bubble`
- Dataset roots already extracted:
  - `datasets/syn_1m/syn_1m/{train,val}`
  - `datasets/syn_1_5m/syn_1_5m/{train,val}`
  - `datasets/syn_2m/syn_2m/{train,val}`
- Python environment: `sound_bubble` (conda)

## 2) Cursor Terminal Setup (Conda Auto-Activate)

If not already configured, add a terminal profile in Cursor user settings
(`%APPDATA%\Cursor\User\settings.json`) so each new terminal opens with
`sound_bubble` active.

Use terminal profile:

- `Conda (sound_bubble)` for training/inference
- `PowerShell` for vanilla shell tabs (optional parallel tab)

## 3) Environment Validation

Run inside `C:\Users\darre\Sound_Bubble`:

```powershell
python -c "import torch; print(torch.cuda.is_available())"
python -c "import wandb"
python -c "import src.datasets.general_multisrc_dataset_with_perturbations"
```

Expected:

- CUDA availability prints `True` on RTX machine
- no import errors

### 3.1 Optional preflight checklist (imports + path checks only)

If you want, I can run a preflight command checklist now (imports + path checks
only) before you launch the full training.

```powershell
# from C:\Users\darre\Sound_Bubble
python -c "import os; print('cwd_ok=', os.path.basename(os.getcwd())=='Sound_Bubble')"
python -c "import torch, wandb; print('torch=', torch.__version__, 'cuda=', torch.version.cuda, 'avail=', torch.cuda.is_available())"
python -c "import src.datasets.general_multisrc_dataset_with_perturbations as d; print('dataset_module_ok=', hasattr(d, 'Dataset'))"
python -c "from pathlib import Path; print('syn_1_5m_train=', Path('datasets/syn_1_5m/syn_1_5m/train').exists())"
python -c "from pathlib import Path; print('cfg_1_5m=', Path('real_experiments/raspberrypi_local_pretrain.json').exists())"
python -c "from pathlib import Path; print('cfg_1m=', Path('real_experiments/raspberrypi_local_pretrain_1m.json').exists())"
python -c "from pathlib import Path; print('cfg_2m=', Path('real_experiments/raspberrypi_local_pretrain_2m.json').exists())"
```

## 4) Configs and What They Mean

- `real_experiments/raspberrypi_local_pretrain.json`
  - distance variant: `1.5 m`
  - dataset: `syn_1_5m`
  - threshold: `dis_threshold: 1.5`
- `real_experiments/raspberrypi_local_pretrain_1m.json`
  - distance variant: `1.0 m`
  - dataset: `syn_1m`
  - threshold: `dis_threshold: 1.0`
- `real_experiments/raspberrypi_local_pretrain_2m.json`
  - distance variant: `2.0 m`
  - dataset: `syn_2m`
  - threshold: `dis_threshold: 2.0`

Important contract:

- Train one model per distance threshold.
- Do not mix `syn_1m`, `syn_1_5m`, and `syn_2m` under one global threshold.

## 5) Start Training (Windows)

### 5.1 Default run (recommended first): 1.5 m

```powershell
python -m src.train_pt `
  --config real_experiments/raspberrypi_local_pretrain.json `
  --run_dir runs/optim_pretrain_1_5m

$env:WANDB_MODE="disabled"
python -m src.train_pt `
  --config real_experiments/raspberrypi_local_pretrain.json `
  --run_dir runs/optim_pretrain_1_5m
```

### 5.2 Optional distance-specific runs

```powershell
python src/train_pt.py `
  --config real_experiments/raspberrypi_local_pretrain_1m.json `
  --run_dir runs/optim_pretrain_1m

python src/train_pt.py `
  --config real_experiments/raspberrypi_local_pretrain_2m.json `
  --run_dir runs/optim_pretrain_2m
```

## 6) Checkpoints and Resume Behavior

For each run directory:

- `checkpoints/best.pt` = best validation checkpoint
- `checkpoints/last.pt` = latest checkpoint

`src/train_pt.py` resumes automatically from `checkpoints/last.pt`
when the same `--run_dir` is reused.

## 7) Realtime Smoke Test After Training

### 7.1 Direct run-dir loading

```powershell
python edge/dummy_realtime_lite.py runs/optim_pretrain_1_5m
```

### 7.2 Preset-based local loading

`--preset optim-local` now prefers:

- `runs/optim_pretrain_1_5m/config.json` (if present)
- fallback to `real_experiments/raspberrypi_local_pretrain.json`

```powershell
python edge/dummy_realtime_lite.py --preset optim-local
```

## 8) Monitoring

- W&B project defaults to `AcousticBubble`
- primary monitor: `val/loss` (lower is better)
- keep run names aligned with distance variant for traceability:
  - `optim_pretrain_1m`
  - `optim_pretrain_1_5m`
  - `optim_pretrain_2m`

## 9) Immediate Next Steps

1. Run the `1.5 m` training first to establish baseline.
2. Validate realtime smoke test with `runs/optim_pretrain_1_5m`.
3. Only then train `1m` and `2m` variants if you need selectable bubble sizes.
4. In the later RPi porting phase, reuse these checkpoints and profile CPU path.

