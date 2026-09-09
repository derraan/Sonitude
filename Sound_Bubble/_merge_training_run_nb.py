import copy
import json
import pathlib


def load_nb(p: pathlib.Path) -> dict:
    return json.loads(p.read_text(encoding="utf-8"))


def first_code_cell(nb: dict, predicate):
    for i, c in enumerate(nb.get("cells", [])):
        if c.get("cell_type") != "code":
            continue
        src = "".join(c.get("source", []))
        if predicate(src):
            return i, c
    return None, None


def main() -> None:
    repo_nb_path = pathlib.Path(r"c:\Users\darre\Sound_Bubble\colab\training_run.ipynb")
    user_nb_path = pathlib.Path(r"c:\Users\darre\Downloads\training_run.ipynb")

    repo = load_nb(repo_nb_path)
    user = load_nb(user_nb_path)

    r_mount_i, r_mount = first_code_cell(
        repo,
        lambda s: ("# ---- Resolve EXP_NAME" in s and "drive.mount" in s),
    )
    r_resolve_i, r_resolve = first_code_cell(
        repo, lambda s: s.lstrip().startswith("# Resolve run_dir")
    )
    r_train_i, r_train = first_code_cell(
        repo,
        lambda s: s.lstrip().startswith("# Kick off training") and ("preflight" in s),
    )

    if not (r_mount and r_resolve and r_train):
        raise RuntimeError(
            f"Could not find template cells in repo notebook: {r_mount_i}, {r_resolve_i}, {r_train_i}"
        )

    u_mount_i, u_mount = first_code_cell(
        user,
        lambda s: ("# ---- Resolve EXP_NAME" in s and "drive.mount" in s),
    )
    u_resolve_i, u_resolve = first_code_cell(
        user, lambda s: s.lstrip().startswith("# Resolve run_dir")
    )
    u_train_i, u_train = first_code_cell(
        user, lambda s: s.lstrip().startswith("# Kick off training")
    )

    if not (u_mount and u_resolve and u_train):
        raise RuntimeError(
            f"Could not find target cells in user notebook: {u_mount_i}, {u_resolve_i}, {u_train_i}"
        )

    # Replace sources but keep the user's cell metadata, outputs, execution counts, ids.
    u_mount["source"] = copy.deepcopy(r_mount["source"])
    u_resolve["source"] = copy.deepcopy(r_resolve["source"])
    u_train["source"] = copy.deepcopy(r_train["source"])

    u_knob_i, u_knob = first_code_cell(
        user, lambda s: "# ==== User knobs" in s
    )
    if u_knob:
        for j, line in enumerate(u_knob["source"]):
            if "overrides config's n_epochs" in line:
                u_knob["source"][j] = line.replace(
                    "overrides config's n_epochs", "overrides config's `epochs`"
                )

    backup = user_nb_path.with_suffix(".ipynb.bak-merge")
    backup.write_text(json.dumps(user, ensure_ascii=False, indent=1) + "\n", encoding="utf-8")
    user_nb_path.write_text(json.dumps(user, ensure_ascii=False, indent=1) + "\n", encoding="utf-8")

    print(f"Wrote: {user_nb_path}")
    print(f"Backup: {backup}")
    print(
        "Replaced cells:",
        {"mount": u_mount_i, "resolve": u_resolve_i, "train": u_train_i},
    )


if __name__ == "__main__":
    main()
