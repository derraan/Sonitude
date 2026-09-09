"""
The main training script for training on synthetic data
"""

import torch
import torch.utils.data
import torch.nn as nn

import argparse
import json
import os
import multiprocessing
import time

import numpy as np
import src.utils as utils
from src.training.tain_val import train_epoch, test_epoch
import shutil

import wandb

VAL_SEED = 0
CURRENT_EPOCH = 0
TRAIN_SEED = 0


class _NoOpWandbRun:
    def log(self, *_args, **_kwargs):
        return None

    def finish(self, *_args, **_kwargs):
        return None

def seed_from_epoch(seed):
    global CURRENT_EPOCH

    utils.seed_all(seed + CURRENT_EPOCH)

def worker_init_train(_worker_id: int):
    seed_from_epoch(TRAIN_SEED)

def worker_init_val(_worker_id: int):
    utils.seed_all(VAL_SEED)

def print_metrics(metrics: list):
    input_sisdr = np.array([x['input_si_sdr'] for x in metrics])
    sisdr = np.array([x['si_sdr'] for x in metrics])

    print("Average Input SI-SDR: {:03f}, Average Output SI-SDR: {:03f}, Average SI-SDRi: {:03f}".format(np.mean(input_sisdr), np.mean(sisdr), np.mean(sisdr - input_sisdr)))


def _mirror_run_dir(
    src_run_dir: str,
    dst_run_dir: str,
    *,
    mirror_checkpoints_only: bool = True,
) -> None:
    """
    Best-effort mirror of training artifacts to a durable location (e.g. Drive).

    We intentionally keep this function resilient: failures should never crash training.
    """
    try:
        if not dst_run_dir:
            return
        os.makedirs(dst_run_dir, exist_ok=True)

        # Always mirror config.json if present (tiny + helpful for resuming).
        src_cfg = os.path.join(src_run_dir, "config.json")
        if os.path.isfile(src_cfg):
            shutil.copy2(src_cfg, os.path.join(dst_run_dir, "config.json"))

        if mirror_checkpoints_only:
            src_ck = os.path.join(src_run_dir, "checkpoints")
            dst_ck = os.path.join(dst_run_dir, "checkpoints")
            if os.path.isdir(src_ck):
                os.makedirs(dst_ck, exist_ok=True)
                # Mirror only the standard checkpoint names + any *.pt (small-ish).
                for fn in os.listdir(src_ck):
                    if not (fn.endswith(".pt") or fn.endswith(".pth")):
                        continue
                    shutil.copy2(os.path.join(src_ck, fn), os.path.join(dst_ck, fn))
            return

        # Full mirror (slower). Prefer rsync if available.
        try:
            import subprocess

            subprocess.run(
                ["rsync", "-a", "--delete", src_run_dir.rstrip("/") + "/", dst_run_dir.rstrip("/") + "/"],
                check=True,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
        except Exception:
            # Fallback: Python copytree (no delete; best-effort).
            for root, _dirs, files in os.walk(src_run_dir):
                rel = os.path.relpath(root, src_run_dir)
                out_dir = os.path.join(dst_run_dir, rel) if rel != "." else dst_run_dir
                os.makedirs(out_dir, exist_ok=True)
                for f in files:
                    fp = os.path.join(root, f)
                    try:
                        shutil.copy2(fp, os.path.join(out_dir, f))
                    except Exception:
                        pass
    except Exception:
        # Never crash training due to mirror.
        return


def train(args: argparse.Namespace):
    """
    Resolve the network to be trained
    """
    # Fix random seeds
    utils.seed_all(args.seed)
    os.environ["CUBLAS_WORKSPACE_CONFIG"]=":4096:8"

    # Turn on deterministic algorithms if specified (Note: slower training).
    if torch.cuda.is_available():
        if args.use_nondeterministic_cudnn:
            torch.backends.cudnn.deterministic = False
        else:
            torch.backends.cudnn.deterministic = True

    # Load experiment description
    with open(args.config, 'rb') as f:
        params = json.load(f)

    # Initialize datasets
    data_train = utils.import_attr(params['train_dataset'])(**params['train_data_args'], split='train')
    data_val = utils.import_attr(params['val_dataset'])(**params['val_data_args'], split='val')

    # Set up the device and workers
    use_cuda = torch.cuda.is_available()
    device = torch.device('cuda' if use_cuda else 'cpu')
    print("Using device {}".format('cuda' if use_cuda else 'cpu'))

    # Set multiprocessing params
    global TRAIN_SEED
    TRAIN_SEED = args.seed
    num_workers = min(multiprocessing.cpu_count(), params['num_workers'])
    kwargs = {
        'num_workers': num_workers,
        'worker_init_fn': worker_init_train,
        'pin_memory': True
    } if use_cuda else {}

    # Set up data loaders
    train_loader = torch.utils.data.DataLoader(data_train,
                                               batch_size=params['batch_size'],
                                               shuffle=True,
                                               **kwargs)
   
    val_kwargs = dict(kwargs)
    if len(val_kwargs) > 0:
        val_kwargs['worker_init_fn'] = worker_init_val
    test_loader = torch.utils.data.DataLoader(data_val,
                                              batch_size=params['eval_batch_size'],
                                              **val_kwargs)

    # Initialize HL module
    hl_module = utils.import_attr(params['pl_module'])(**params['pl_module_args'])
    hl_module.model.to(device) 
    
    # Get run name from run dir
    run_name = os.path.basename(args.run_dir.rstrip('/'))
    checkpoints_dir = os.path.join(args.run_dir, 'checkpoints')
    # Optional durability mirror (e.g. Colab -> Drive).
    mirror_dir = (
        args.mirror_dir
        or os.environ.get("SOUND_BUBBLE_MIRROR_DIR", "")
        or os.environ.get("SB_MIRROR_DIR", "")
    )
    mirror_every_epochs = int(
        args.mirror_every_epochs
        if args.mirror_every_epochs is not None
        else os.environ.get("SOUND_BUBBLE_MIRROR_EVERY_EPOCHS", os.environ.get("SB_MIRROR_EVERY_EPOCHS", "0"))
    )
    mirror_every_secs = float(
        args.mirror_every_secs
        if args.mirror_every_secs is not None
        else os.environ.get("SOUND_BUBBLE_MIRROR_EVERY_SECS", os.environ.get("SB_MIRROR_EVERY_SECS", "0"))
    )
    mirror_checkpoints_only = bool(args.mirror_checkpoints_only)
    last_mirror_time = 0.0

    # Set up checkpoints
    if not os.path.exists(checkpoints_dir):
        os.makedirs(checkpoints_dir)

    # Copy json
    if not os.path.exists(os.path.join(args.run_dir, 'config.json')):
        shutil.copyfile(args.config, os.path.join(args.run_dir, 'config.json'))

    # Check if a model state path exists for this model, if it does, load it
    best_path = os.path.join(checkpoints_dir, 'best.pt')
    state_path = os.path.join(checkpoints_dir, 'last.pt')
    if os.path.exists(state_path):
        hl_module.load_state(state_path)

    start_epoch = hl_module.epoch
    # How often to overwrite last.pt (latest checkpoint). Best checkpoint is handled
    # inside hl_module.on_epoch_end() and is still evaluated every epoch.
    last_every_epochs = int(args.last_every_epochs) if args.last_every_epochs is not None else 2
    
    if "project_name" in params.keys():
        project_name = params["project_name"]
    else:
        project_name = "AcousticBubble"
    # Initialize wandb (optional). If no API key is configured (common on Colab),
    # fall back to a no-op logger so training can proceed.
    wandb_run = _NoOpWandbRun()
    if not args.no_wandb:
        try:
            wandb_run = wandb.init(
                project=project_name,
                name=run_name,
                notes="Example of a note",
                tags=["speech", "audio", "embedded-systems"],
            )
        except Exception as e:
            print(f"[wandb] disabled ({type(e).__name__}: {e})")
            wandb_run = _NoOpWandbRun()

    # Training loop
    try:        
        # Go over remaining epochs
        for epoch in range(start_epoch, params['epochs']):
            global CURRENT_EPOCH, VAL_SEED
            CURRENT_EPOCH = epoch
            seed_from_epoch(args.seed)

            hl_module.on_epoch_start()

            current_lr = hl_module.get_current_lr()
            print("CURRENT learning rate: {:0.08f}".format(current_lr))

            print("[TRAINING]")
            
            # Run testing step
            
            t1 = time.time()
            train_loss = train_epoch(hl_module, train_loader, device)
            t2 = time.time()
            print(f"Train epoch time: {t2 - t1:02f}s")

            print("\nTrain set: Average Loss: {:.4f}\n".format(train_loss))

            print()

            # Fix seed for all validation passes 
            # (needed since localization models will choose some random points, so we fix those)
            utils.seed_all(VAL_SEED)

            # Run testing step

            print("[TESTING]")
            
            test_loss = test_epoch(hl_module, test_loader, device)
            
            print("\nTest set: Average Loss: {:.4f}\n".format(test_loss))
                    
            # # Add and save losses as pkl file
            # train_losses.append(train_loss)
            # val_losses.append(test_loss)
                    
            # # Save model params, optimizer, scheduler, losses
            # torch.save(model.module.state_dict(), os.path.join(checkpoints_dir, experiment_name + "_{}.pt".format(epoch)))
            # state = {
            #     'epoch': epoch,
            #     'optimizer': optimizer.state_dict(),
            #     'lr_sched': scheduler,
            #     'train_losses': train_losses,
            #     'val_losses': val_losses,
            # }
            # torch.save(state, state_path)
            hl_module.on_epoch_end(best_path, wandb_run)
            # Save latest state periodically to reduce I/O; always save on final epoch.
            is_final_epoch = (epoch + 1) >= params['epochs']
            if last_every_epochs <= 1 or (epoch % last_every_epochs) == 0 or is_final_epoch:
                hl_module.dump_state(state_path)

            # ---- Durability mirror (best-effort) ----
            do_mirror = bool(mirror_dir)
            if do_mirror and mirror_every_epochs > 0:
                do_mirror = (epoch % mirror_every_epochs) == 0
            if do_mirror and mirror_every_secs > 0:
                now = time.time()
                if last_mirror_time <= 0.0 or (now - last_mirror_time) >= mirror_every_secs:
                    pass
                else:
                    do_mirror = False
            if do_mirror and mirror_dir:
                _mirror_run_dir(args.run_dir, mirror_dir, mirror_checkpoints_only=mirror_checkpoints_only)
                last_mirror_time = time.time()

            print()
            print("=" * 25, "FINISHED EPOCH", epoch, "=" * 25)
            print()

    except KeyboardInterrupt:
        print("Interrupted")
    except Exception as _:
        import traceback
        traceback.print_exc()

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    # Experiment Params
    parser.add_argument('--config', type=str,
                        help='Path to experiment config')

    parser.add_argument('--run_dir', type=str,
                        help='Path to experiment directory')

    # Randomization Params
    parser.add_argument('--seed', type=int, default=0,
                        help='Random seed for reproducibility')
    parser.add_argument('--use_nondeterministic_cudnn',
                        action='store_true',
                        help="If using cuda, chooses whether or not to use \
                                non-deterministic cudDNN algorithms. Training will be\
                                faster, but the final results may differ slighty.")
    
    # wandb params
    parser.add_argument('--project_name',
                        type=str,
                        default='AcousticBubble',
                        help='Project name that shows up on wandb')
    parser.add_argument('--no_wandb',
                        action='store_true',
                        help='Disable Weights & Biases logging (offline/no key).')
    parser.add_argument(
        '--last_every_epochs',
        type=int,
        default=2,
        help='Overwrite checkpoints/last.pt every N epochs (default: 2). '
             'Set to 1 to save last.pt every epoch.',
    )
    # Optional: mirror checkpoints/config during training for durability (e.g. Colab -> Drive).
    parser.add_argument(
        '--mirror_dir',
        type=str,
        default=None,
        help='Optional directory to mirror checkpoints/config into during training (best-effort). '
             'Can also be set via SOUND_BUBBLE_MIRROR_DIR / SB_MIRROR_DIR env var.',
    )
    parser.add_argument(
        '--mirror_every_epochs',
        type=int,
        default=None,
        help='Mirror every N epochs (0/None disables). Env: SOUND_BUBBLE_MIRROR_EVERY_EPOCHS / SB_MIRROR_EVERY_EPOCHS.',
    )
    parser.add_argument(
        '--mirror_every_secs',
        type=float,
        default=None,
        help='Mirror every N seconds (0/None disables). Useful if epochs are long. '
             'Env: SOUND_BUBBLE_MIRROR_EVERY_SECS / SB_MIRROR_EVERY_SECS.',
    )
    parser.add_argument(
        '--mirror_checkpoints_only',
        action='store_true',
        help='Mirror only config.json + checkpoints/*.pt (faster, recommended).',
    )
    train(parser.parse_args())
