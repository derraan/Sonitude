# Colab bridge

One-way staging folder between this workspace and Google Colab, using the
existing Drive-synced tree at `C:\Users\darre\Documents\Obsidian Notes\CDE3301`.

## Layout

```
<drive-synced-root>/CDE3301/colab_bridge/
  code/            # workspace snapshot pushed from PC (rsync target)
  datasets/        # tarballs pushed once, read by Colab
  runs/            # Colab writes training runs here (back-synced to PC)
  zoo/             # Colab writes exported ONNX + runtime.json here (→ edge/zoo/)
```

Bi-directional via the Google Drive Desktop client. No git required for the
bridge itself. RPi can consume the zoo via `gdown` by sharing the bridge.

## Workflow

```
[PC workspace] ── sync_workspace_to_bridge.ps1 ──▶ [Drive: colab_bridge/code]
                                                         │
                                                         ▼
                                                [Colab training_run.ipynb]
                                                         │
                                           (reads code + datasets,
                                            writes runs + zoo back)
                                                         │
                                                         ▼
[PC workspace] ◀── sync_bridge_to_workspace.ps1 ── [Drive: colab_bridge/{runs,zoo}]
```

## Typical loop

1. Edit locally in `Sound_Bubble/`.
2. `./colab/sync_workspace_to_bridge.ps1` to push source snapshot to the bridge.
3. Open `colab/training_run.ipynb` in Colab (via Drive: right-click → Open with → Colab).
4. Set `RADIUS`, `EPOCHS`, `CONFIG_PATH` constants in the first cell.
5. Run all. Training + ONNX export land in `colab_bridge/runs/` and `colab_bridge/zoo/`.
6. Back on PC: `./colab/sync_bridge_to_workspace.ps1` to pull runs + zoo into the repo.
7. `edge/zoo/` is now populated — live pipeline resolves with `--zoo-dir edge/zoo --bubble-radius X`.

## Datasets

Drop `syn_1m.tar`, `syn_1_5m.tar`, `syn_2m.tar`, `syn_test.tar` into
`colab_bridge/datasets/` once. The notebook copies tarballs into Colab local
disk (`/content/data/`) and untars there for fast I/O during training.

## RPi pickup (optional)

Share any bridge subfolder (or individual files) with "Anyone with the link".
On the RPi:

```bash
pip install gdown
gdown --folder <drive-share-link> -O edge/zoo/
```

This gives the Pi direct access to the latest zoo without SSH/push-from-PC.
