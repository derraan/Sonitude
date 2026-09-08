from __future__ import annotations


def main():
    raise RuntimeError(
        "dummy_realtime_lite.py is deprecated: it still uses the old raw output-ring and callback-heavy design. "
        "Use dummy_realtime.py or inference_pipeline.py instead."
    )


if __name__ == "__main__":
    main()
