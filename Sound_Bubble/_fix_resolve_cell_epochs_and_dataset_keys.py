import json
import pathlib


def fix_notebook(path: pathlib.Path) -> bool:
    nb = json.loads(path.read_text(encoding="utf-8"))
    changed = False

    for cell in nb.get("cells", []):
        if cell.get("cell_type") != "code":
            continue
        src = cell.get("source", [])
        text = "".join(src)
        if not text.lstrip().startswith("# Resolve run_dir"):
            continue

        new_text = text
        new_text = new_text.replace(
            "#   (b) n_epochs -> EPOCHS_TARGET.\n",
            "#   (b) epochs -> EPOCHS_TARGET (train_pt.py reads params['epochs']).\n",
        )
        new_text = new_text.replace(
            'cfg_epochs_before = cfg.get("n_epochs")\n'
            'if EPOCHS_TARGET is not None and EPOCHS_TARGET != cfg_epochs_before:\n'
            '    cfg["n_epochs"] = int(EPOCHS_TARGET)\n',
            'cfg_epochs_before = cfg.get("epochs")\n'
            'if EPOCHS_TARGET is not None and EPOCHS_TARGET != cfg_epochs_before:\n'
            '    cfg["epochs"] = int(EPOCHS_TARGET)\n',
        )
        new_text = new_text.replace(
            'print(f"[cfg] n_epochs: {cfg_epochs_before} -> {cfg.get(\'n_epochs\')}")\n',
            'print(f"[cfg] epochs: {cfg_epochs_before} -> {cfg.get(\'epochs\')}")\n',
        )
        new_text = new_text.replace(
            'for k in ("train_dataset_args", "val_dataset_args", "test_dataset_args"):\n',
            'for k in ("train_data_args", "val_data_args", "test_data_args"):\n',
        )

        if new_text != text:
            # Preserve per-line list format (each line ends with \n)
            cell["source"] = [line if line.endswith("\n") else line + "\n" for line in new_text.splitlines(True)]
            changed = True

    if changed:
        backup = path.with_suffix(path.suffix + ".bak-epochs-fix")
        backup.write_text(json.dumps(nb, ensure_ascii=False, indent=1) + "\n", encoding="utf-8")
        path.write_text(json.dumps(nb, ensure_ascii=False, indent=1) + "\n", encoding="utf-8")

    return changed


def main() -> None:
    targets = [
        pathlib.Path(r"c:\Users\darre\Downloads\training_run.ipynb"),
        pathlib.Path(r"c:\Users\darre\Sound_Bubble\colab\training_run.ipynb"),
    ]
    for t in targets:
        if not t.exists():
            print("SKIP missing:", t)
            continue
        ok = fix_notebook(t)
        print(("UPDATED" if ok else "NOCHANGE"), t)


if __name__ == "__main__":
    main()
