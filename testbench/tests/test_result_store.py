from __future__ import annotations

from pathlib import Path

from app.storage.result_store import ResultStore, new_test_id


def test_new_test_id_is_unique() -> None:
    assert new_test_id() != new_test_id()


def test_new_test_creates_directory(tmp_path: Path) -> None:
    store = ResultStore(tmp_path)
    test = store.new_test()
    assert test.root.exists()
    assert test.root.parent == store.results_dir


def test_copy_input_does_not_delete_source(tmp_path: Path) -> None:
    store = ResultStore(tmp_path)
    source = tmp_path / "original.wav"
    source.write_bytes(b"RIFF....WAVEfmt ")
    test = store.new_test()
    store.copy_input(source, test.input_wav)
    assert source.exists()
    assert test.input_wav.exists()
    assert test.input_wav.read_bytes() == source.read_bytes()


def test_save_and_load_metadata_metrics(tmp_path: Path) -> None:
    store = ResultStore(tmp_path)
    test = store.new_test("TEST_fixed")
    store.save_metadata(test, {"test_id": "TEST_fixed", "channels": 6})
    store.save_metrics(test, {"noise_suppression": {"method": "estimated"}})

    assert store.load_metadata("TEST_fixed")["channels"] == 6
    assert store.load_metrics("TEST_fixed")["noise_suppression"]["method"] == "estimated"
    assert "TEST_fixed" in store.list_tests()
