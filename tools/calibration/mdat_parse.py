"""Best-effort REW ``.mdat`` reader.

REW stores measurements as Java-serialized objects (``REW Measurement Data File V2``).
Full IR/FR sample arrays need REW's class files or the REW HTTP API. This module extracts
the measurement notes, delays, levels, and FR summary cards that REW embeds as plain
text — enough for calibration ingest and GUI preview without launching REW.
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field
from pathlib import Path
from typing import Sequence

from .angles import parse_azimuth_from_name

_MDAT_MAGIC = b"\xac\xed\x00\x05"
_FILE_LABEL = b"REW Measurement Data File"

_DELAY_RE = re.compile(
    rb"Delay\s+([-+]?\d+(?:\.\d+)?)\s*ms(?:\s*\(([^)]*)\))?",
    re.IGNORECASE,
)
_CLOCK_RE = re.compile(rb"Clock adjustment:\s*([-+]?[\d,]+(?:\.\d+)?)\s*ppm", re.IGNORECASE)
_PEAK_LEVEL_RE = re.compile(rb"Timing signal peak level\s+([-+]?\d+(?:\.\d+)?)\s*dBFS", re.IGNORECASE)
_MEAS_PEAK_RE = re.compile(rb"measurement signal peak level\s+([-+]?\d+(?:\.\d+)?)\s*dBFS", re.IGNORECASE)
_CHANNEL_RE = re.compile(rb"\bChannel\s+(\d+)\b", re.IGNORECASE)
_WAV_RE = re.compile(
    rb"([A-Za-z]:\\[^\x00-\x1f]+\.wav|[A-Za-z0-9_+\-.]+\.wav)",
    re.IGNORECASE,
)
_HTML_CARD_RE = re.compile(rb"<BODY>(.*?)</BODY>", re.IGNORECASE | re.DOTALL)
_HTML_SPL_RE = re.compile(rb"([-+]?\d+(?:\.\d+)?)\s+to\s+([-+]?\d+(?:\.\d+)?)\s+dB\s*SPL", re.IGNORECASE)
_HTML_FREQ_RE = re.compile(rb"(\d+(?:,\d+)?(?:\.\d+)?)\s+to\s+(\d+(?:,\d+)?(?:\.\d+)?)\s*Hz", re.IGNORECASE)
_HTML_DATE_RE = re.compile(rb"(\d{1,2}\s+\w+,\s+\d{4})", re.IGNORECASE)
_HTML_TIME_RE = re.compile(rb"(\d{1,2}:\d{2}:\d{2}\s*[AP]M)", re.IGNORECASE)


@dataclass
class MdatMeasurement:
    title: str
    source_wav: str = ""
    channel: int | None = None
    delay_ms: float | None = None
    delay_distance_note: str = ""
    clock_ppm: float | None = None
    timing_peak_dbfs: float | None = None
    measurement_peak_dbfs: float | None = None
    azimuth_deg: float | None = None
    fr_f_min_hz: float | None = None
    fr_f_max_hz: float | None = None
    spl_min_db: float | None = None
    spl_max_db: float | None = None
    dated: str = ""
    note: str = ""

    def delay_samples(self, sample_rate_hz: float) -> float | None:
        if self.delay_ms is None:
            return None
        return float(self.delay_ms) * 1.0e-3 * float(sample_rate_hz)

    @property
    def is_array_channel(self) -> bool:
        blob = f"{self.source_wav} {self.title} {self.note}".lower()
        return "mic-array" in blob or "array-" in blob or (self.channel is not None and "mic-m" not in blob)


@dataclass
class MdatParseResult:
    path: Path
    version_label: str
    measurements: list[MdatMeasurement] = field(default_factory=list)
    sample_rate_hz_guess: float | None = None
    warnings: list[str] = field(default_factory=list)
    source_paths: list[Path] = field(default_factory=list)

    def by_azimuth(self) -> dict[float, list[MdatMeasurement]]:
        out: dict[float, list[MdatMeasurement]] = {}
        for item in self.measurements:
            if item.azimuth_deg is None:
                continue
            out.setdefault(float(item.azimuth_deg), []).append(item)
        return out

    def array_channel_delays_ms(self, azimuth_deg: float = 0.0) -> dict[int, float]:
        """Map 1-based REW channel index -> delay_ms for the array take at ``azimuth_deg``."""
        rows = [m for m in self.measurements if m.is_array_channel and m.channel is not None]
        preferred = [m for m in rows if m.azimuth_deg is not None and abs(float(m.azimuth_deg) - float(azimuth_deg)) < 1e-6]
        use = preferred or rows
        out: dict[int, float] = {}
        for row in use:
            if row.delay_ms is None:
                continue
            out[int(row.channel)] = float(row.delay_ms)
        return out

    def element_delays_ms(self) -> dict[str, float]:
        """Map MIC-Mx stem -> delay_ms for same-position element takes."""
        out: dict[str, float] = {}
        for item in self.measurements:
            if item.delay_ms is None:
                continue
            match = re.search(r"(MIC-M[0-5])", f"{item.source_wav} {item.title}", flags=re.IGNORECASE)
            if match:
                out[match.group(1).upper().replace("MIC-M", "M")] = float(item.delay_ms)
        return out


def _as_text(chunk: bytes) -> str:
    return "".join(chr(b) if 32 <= b < 127 else "\n" for b in chunk)


def _guess_sample_rate(text: str) -> float | None:
    lower = text.lower()
    if "44k" in lower or "44100" in lower:
        return 44100.0
    if "48k" in lower or "48000" in lower:
        return 48000.0
    if "96k" in lower or "96000" in lower:
        return 96000.0
    return None


def _parse_html_cards(data: bytes) -> list[dict[str, float | str]]:
    cards: list[dict[str, float | str]] = []
    for match in _HTML_CARD_RE.finditer(data):
        body = match.group(1)
        body = re.sub(rb"<BR\s*/?>", b"\n", body, flags=re.IGNORECASE)
        body = re.sub(rb"<[^>]+>", b"", body)
        text = _as_text(body)
        info: dict[str, float | str] = {"offset": match.start()}
        date = _HTML_DATE_RE.search(body)
        time = _HTML_TIME_RE.search(body)
        if date or time:
            parts = []
            if date:
                parts.append(date.group(1).decode("ascii", errors="ignore"))
            if time:
                parts.append(time.group(1).decode("ascii", errors="ignore"))
            info["dated"] = " ".join(parts)
        freq = _HTML_FREQ_RE.search(body)
        if freq:
            info["fr_f_min_hz"] = float(freq.group(1).replace(b",", b""))
            info["fr_f_max_hz"] = float(freq.group(2).replace(b",", b""))
        spl = _HTML_SPL_RE.search(body)
        if spl:
            info["spl_min_db"] = float(spl.group(1))
            info["spl_max_db"] = float(spl.group(2))
        cards.append(info)
    return cards


def _nearest_html(cards: list[dict[str, float | str]], offset: int) -> dict[str, float | str] | None:
    if not cards:
        return None
    return min(cards, key=lambda c: abs(int(c["offset"]) - offset))


def _extract_peak_levels(data: bytes, *, window_bytes: int = 500) -> dict[str, dict[str, float]]:
    """Map source WAV basename (+ optional channel) -> peak levels."""
    out: dict[str, dict[str, float]] = {}
    for match in _PEAK_LEVEL_RE.finditer(data):
        start = max(0, match.start() - window_bytes)
        end = min(len(data), match.end() + window_bytes)
        window = data[start:end]
        wav_match = None
        for candidate in _WAV_RE.finditer(window):
            abs_pos = start + candidate.start()
            if abs_pos <= match.start():
                wav_match = candidate
        if wav_match is None:
            continue
        source = Path(wav_match.group(1).decode("ascii", errors="ignore")).name.lower()
        channel_match = None
        for candidate in _CHANNEL_RE.finditer(window):
            abs_pos = start + candidate.start()
            if abs_pos <= match.start() + 40:
                channel_match = candidate
        key = source if channel_match is None else f"{source}|ch{int(channel_match.group(1))}"
        entry = out.setdefault(key, {})
        entry["timing_peak_dbfs"] = float(match.group(1))
        meas = _MEAS_PEAK_RE.search(window)
        if meas:
            entry["measurement_peak_dbfs"] = float(meas.group(1))
        # Also store basename-only fallback.
        out.setdefault(source, dict(entry))
    return out


def parse_rew_mdat(path: str | Path, *, window_bytes: int = 2500) -> MdatParseResult:
    path = Path(path)
    data = path.read_bytes()
    if not data.startswith(_MDAT_MAGIC) or _FILE_LABEL not in data[:80]:
        raise RuntimeError(f"Not a REW measurement data file: {path}")

    label = "REW Measurement Data File"
    head = _as_text(data[:64])
    for token in head.split("\n"):
        if token.startswith("REW Measurement Data File"):
            label = token.strip()
            break

    html_cards = _parse_html_cards(data)
    peak_levels = _extract_peak_levels(data)
    measurements: list[MdatMeasurement] = []
    seen: set[str] = set()

    for match in _DELAY_RE.finditer(data):
        start = max(0, match.start() - window_bytes)
        end = min(len(data), match.end() + window_bytes)
        window = data[start:end]
        text = _as_text(window)

        delay_ms = float(match.group(1))
        distance = match.group(2).decode("ascii", errors="ignore").strip() if match.group(2) else ""

        wav_match = _WAV_RE.search(window)
        source_wav = ""
        if wav_match:
            best = None
            for candidate in _WAV_RE.finditer(window):
                abs_pos = start + candidate.start()
                if abs_pos <= match.start():
                    best = candidate
            source_wav = (best or wav_match).group(1).decode("ascii", errors="ignore")

        channel_match = None
        for candidate in _CHANNEL_RE.finditer(window):
            abs_pos = start + candidate.start()
            if abs_pos <= match.start() + 40:
                channel_match = candidate
        clock_match = _CLOCK_RE.search(window)

        title_bits: list[str] = []
        if source_wav:
            title_bits.append(Path(source_wav).name)
        if channel_match:
            title_bits.append(f"Channel {int(channel_match.group(1))}")
        title = " / ".join(title_bits) if title_bits else f"Delay {delay_ms:.4f} ms"

        azimuth = parse_azimuth_from_name(source_wav) or parse_azimuth_from_name(text)
        html = _nearest_html(html_cards, match.start()) or {}

        peak_info: dict[str, float] = {}
        if source_wav:
            base = Path(source_wav).name.lower()
            if channel_match is not None:
                peak_info = peak_levels.get(f"{base}|ch{int(channel_match.group(1))}", {})
            if not peak_info:
                peak_info = peak_levels.get(base, {})

        item = MdatMeasurement(
            title=title,
            source_wav=source_wav,
            channel=int(channel_match.group(1)) if channel_match else None,
            delay_ms=delay_ms,
            delay_distance_note=distance,
            clock_ppm=float(clock_match.group(1).replace(b",", b"")) if clock_match else None,
            timing_peak_dbfs=peak_info.get("timing_peak_dbfs"),
            measurement_peak_dbfs=peak_info.get("measurement_peak_dbfs"),
            azimuth_deg=azimuth,
            fr_f_min_hz=float(html["fr_f_min_hz"]) if "fr_f_min_hz" in html else None,
            fr_f_max_hz=float(html["fr_f_max_hz"]) if "fr_f_max_hz" in html else None,
            spl_min_db=float(html["spl_min_db"]) if "spl_min_db" in html else None,
            spl_max_db=float(html["spl_max_db"]) if "spl_max_db" in html else None,
            dated=str(html.get("dated", "")),
            note=re.sub(r"\n+", " | ", text)[:400],
        )
        key = f"{item.source_wav}|{item.channel}|{item.delay_ms:.4f}"
        if key in seen:
            continue
        seen.add(key)
        measurements.append(item)

    sample_rate = None
    for item in measurements:
        sample_rate = _guess_sample_rate(item.source_wav) or _guess_sample_rate(item.note)
        if sample_rate is not None:
            break

    warnings: list[str] = []
    if not measurements:
        warnings.append("No Delay notes found in MDAT")
    warnings.append(
        "MDAT binary IR/FR sample arrays are not deserialized here; "
        "use exported WAV/IR text or the REW API for full traces."
    )

    return MdatParseResult(
        path=path,
        version_label=label,
        measurements=measurements,
        sample_rate_hz_guess=sample_rate,
        warnings=warnings,
        source_paths=[path],
    )


def merge_mdat_results(results: Sequence[MdatParseResult]) -> MdatParseResult:
    """Concatenate measurements from split REW MDATs (e.g. 32-measurement cap)."""
    parsed = [item for item in results if item is not None]
    if not parsed:
        raise RuntimeError("No MDAT results to merge")
    if len(parsed) == 1:
        return parsed[0]

    measurements: list[MdatMeasurement] = []
    by_key: dict[str, MdatMeasurement] = {}
    order: list[str] = []
    warnings: list[str] = []
    labels: list[str] = []
    source_paths: list[Path] = []
    sample_rate: float | None = None
    for result in parsed:
        labels.append(result.version_label)
        for src in result.source_paths or [result.path]:
            if src not in source_paths:
                source_paths.append(src)
        if sample_rate is None:
            sample_rate = result.sample_rate_hz_guess
        for item in result.measurements:
            if item.azimuth_deg is not None and item.channel is not None:
                key = f"az{float(item.azimuth_deg):g}|ch{item.channel}"
            else:
                key = f"{item.source_wav.lower()}|{item.channel}"
            if key not in by_key:
                order.append(key)
            by_key[key] = item
        for warning in result.warnings:
            if warning not in warnings:
                warnings.append(warning)
    measurements = [by_key[key] for key in order]

    names = ", ".join(p.name for p in source_paths)
    warnings.insert(0, f"Merged {len(source_paths)} MDAT files: {names}")
    unique_labels = list(dict.fromkeys(labels))
    return MdatParseResult(
        path=source_paths[0],
        version_label=" + ".join(unique_labels),
        measurements=measurements,
        sample_rate_hz_guess=sample_rate,
        warnings=warnings,
        source_paths=source_paths,
    )


def summarize_mdat_markdown(result: MdatParseResult) -> str:
    sources = result.source_paths or [result.path]
    title = sources[0].name if len(sources) == 1 else f"{len(sources)} files"
    lines = [
        f"# REW MDAT: {title}",
        "",
        f"- format: {result.version_label}",
        f"- files: {', '.join(p.name for p in sources)}",
        f"- measurements parsed: {len(result.measurements)}",
        f"- sample_rate_guess_hz: {result.sample_rate_hz_guess}",
        "",
        "| Title | Ch | Azimuth | Delay ms | Peak dBFS | SPL range | FR Hz |",
        "| --- | ---: | ---: | ---: | ---: | --- | --- |",
    ]
    for item in result.measurements:
        az = "" if item.azimuth_deg is None else f"{item.azimuth_deg:g}"
        delay = "" if item.delay_ms is None else f"{item.delay_ms:.4f}"
        peak = "" if item.timing_peak_dbfs is None else f"{item.timing_peak_dbfs:.1f}"
        spl = ""
        if item.spl_min_db is not None and item.spl_max_db is not None:
            spl = f"{item.spl_min_db:g}…{item.spl_max_db:g}"
        fr = ""
        if item.fr_f_min_hz is not None and item.fr_f_max_hz is not None:
            fr = f"{item.fr_f_min_hz:g}…{item.fr_f_max_hz:g}"
        ch = "" if item.channel is None else str(item.channel)
        lines.append(f"| {item.title} | {ch} | {az} | {delay} | {peak} | {spl} | {fr} |")
    if result.warnings:
        lines.append("")
        lines.append("## Notes")
        lines.append("")
        for warning in result.warnings:
            lines.append(f"- {warning}")
    return "\n".join(lines) + "\n"
