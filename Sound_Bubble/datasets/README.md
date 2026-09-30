# Datasets (Google Drive — not in GitHub)

Raw audio is too large for GitHub (~100GB+). Datasets are synced with **Google Drive for desktop**.

## Current machine setup

Local path (canonical):

`C:\Users\darre\Sound_Bubble\datasets`

This folder is mirrored under Drive **Computers** (folder name: `datasets`).

Expected contents:

```
datasets/
  syn_1m/   + syn_1m.tar
  syn_1_5m/ + syn_1_5m.tar
  syn_2m/   + syn_2m.tar
  syn_test/ + syn_test.tar
  LibriSpeech/
    train-clean-100/
    dev-clean/
  train-clean-100.tar.gz   # optional archive
  dev-clean.tar.gz         # optional archive
  syn_libri_edge_1_35m_ls/ # optional stopband set (generated)
  vctk_split.json
  WHAM_split.json
```

## Notes

- Prefer keeping either the extracted folders **or** the `.tar` / `.tar.gz` archives if Drive space gets tight (having both doubles usage).
- Training configs in this repo point at `C:/Users/darre/Sound_Bubble/datasets/...` on this machine.
- On another machine: install Drive for desktop, wait for `datasets` to finish syncing, then either keep that path or update config paths.

## Generate stopband edge set (after LibriSpeech is present)

```powershell
python scripts\generate_libri_edge_dataset.py `
  --speech-dir C:\Users\darre\Sound_Bubble\datasets\LibriSpeech\train-clean-100 `
  --val-speech-dir C:\Users\darre\Sound_Bubble\datasets\LibriSpeech\dev-clean `
  --output-path C:\Users\darre\Sound_Bubble\datasets\syn_libri_edge_1_35m_ls `
  --n-train 3000 --n-val 400 --duration 5.0 `
  --dis-threshold 1.35 --outside-gap 0.08 `
  --p-n-in0 0.5 --p-n-in1 0.25 --seed 12
```
