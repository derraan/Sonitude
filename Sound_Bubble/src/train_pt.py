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

try:
    import wandb
except ImportError:  # optional; --no_wandb works without it installed
    wandb = None

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

    # Optional CLI overrides (do not require editing the JSON config)
    if getattr(args, "epochs", None) is not None:
        params["epochs"] = int(args.epochs)

    # Initialize datasets
    data_train = utils.import_attr(params['train_dataset'])(**params['train_data_args'], split='train')
    data_val = utils.import_attr(params['val_dataset'])(**params['val_data_args'], split='val')

    # Set up the device and workers
    use_cuda = True
    device = torch.device('cuda' if use_cuda else 'cpu')
    print("Using device {}".format('cuda' if use_cuda else 'cpu'))

    # Set multiprocessing params
    # Windows spawn can't pickle nested lambdas; use module-level worker_init_fns.
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
    
    if "project_name" in params.keys():
        project_name = params["project_name"]
    else:
        project_name = "AcousticBubble"
    # Initialize wandb (optional)
    wandb_run = _NoOpWandbRun()
    if not getattr(args, "no_wandb", False):
        if wandb is None:
            print("[wandb] disabled (package not installed)")
        else:
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
        if start_epoch >= params['epochs']:
            print(
                f"Nothing left to train: start_epoch={start_epoch} >= epochs={params['epochs']}. "
                f"Delete {state_path} or raise epochs / use a fresh --run_dir to continue."
            )
            return

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
            hl_module.dump_state(state_path)

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
    parser.add_argument('--epochs',
                        type=int,
                        default=None,
                        help='Override config epochs (useful to resume a finished run to a higher max).')
    train(parser.parse_args())
