"""Read persisted evidence after a run; do not infer packet loss from frame gaps."""
import argparse
import json
import sqlite3
from datetime import datetime
from pathlib import Path


def flat(value):
    return value.get("value", []) if isinstance(value, dict) else value


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    directory = args.directory.resolve()
    root = Path(__file__).resolve().parent.parent
    result = json.loads((directory / "result.json").read_text(encoding="utf-8-sig"))
    rows = [json.loads(line) for line in (directory / "samples.jsonl").read_text(encoding="utf-8-sig").splitlines() if line.strip()]
    latest = flat(rows[-1]["chains"])
    windows = {}
    for row in rows:
        if row["elapsed_s"] < 30:
            continue
        for chain in flat(row["chains"]):
            if chain.get("loss_unknown", 0):
                windows.setdefault(chain["id"], []).append(chain["mono_us"])
    raw, drop_reviews = [], []
    for current in latest:
        chain_id = current["id"]
        snapshots = windows.get(chain_id, [])
        session = next(chain["session"] for chain in latest if chain["id"] == chain_id)
        units, loops, unmapped_outputs = {}, [], []
        stable_start = next(chain["mono_us"] for chain in flat(rows[0]["chains"]) if chain["id"] == chain_id) + 30000000
        stable_end = current["mono_us"]
        drops, decoder_errors = [], []
        json_records, json_errors = 0, 0
        files = sorted((root / "data" / "observations").glob(session + "*.jsonl"), key=lambda path: path.stat().st_mtime_ns)
        for path in files:
            if "truth" in path.name:
                continue
            with path.open(encoding="utf-8-sig") as source:
                for line in source:
                    try:
                        event = json.loads(line)
                    except ValueError:
                        json_errors += 1
                        continue
                    json_records += 1
                    stage, stamp, frame_id = event.get("stage"), event.get("mono_us", 0), event.get("id")
                    if stable_start <= stamp <= stable_end:
                        if stage == "presentation_dropped":
                            drops.append({"id": frame_id, "mono_us": stamp, "reason": event.get("reason")})
                        elif stage == "decode_failed":
                            decoder_errors.append(event)
                    if stage == "source_loop":
                        loops.append(stamp)
                    if stage == "software_present" and not frame_id and any(end - 15000000 <= stamp <= end for end in snapshots):
                        unmapped_outputs.append({"mono_us": stamp, "media_us": event.get("media_us")})
                    if stage == "source_release" and event.get("video") and any(end - 15000000 <= stamp < end - 5000000 for end in snapshots):
                        units[frame_id] = {"id": frame_id, "source_us": stamp, "media_us": event.get("media_us"), "received_us": None, "presented_us": None, "presentation_count": 0, "explicit_drop": None}
                    elif frame_id in units:
                        unit = units[frame_id]
                        if stage == "received_matched":
                            unit["received_us"] = stamp
                        elif stage == "software_present":
                            unit["presented_us"] = stamp
                            unit["presentation_count"] += 1
                        elif stage in ("presentation_dropped", "compressed_unit_discarded", "decode_failed"):
                            unit["explicit_drop"] = {"stage": stage, "reason": event.get("reason")}
        unresolved = []
        for unit in units.values():
            if unit["presented_us"] is None and unit["explicit_drop"] is None:
                unit["nearest_source_loop_delta_ms"] = min(((unit["source_us"] - loop) / 1000 for loop in loops), key=abs, default=None)
                unit["evidence"] = "matched_at_receiver_without_observed_output" if unit["received_us"] else "no_receiver_match_record"
                unresolved.append(unit)
        raw.append({"id": chain_id, "unknown_snapshot_count": len(snapshots), "candidate_source_units": len(units), "unique_unresolved_units": len(unresolved), "source_loop_count": len(loops), "units": unresolved, "unmapped_output_count": len(unmapped_outputs), "unmapped_outputs": unmapped_outputs, "duplicate_presented_ids": [unit for unit in units.values() if unit["presentation_count"] > 1]})
        reasons = {}
        for drop in drops:
            reasons[drop["reason"]] = reasons.get(drop["reason"], 0) + 1
        drop_reviews.append({"id": chain_id, "event_count": len(drops), "unique_ids": len({item["id"] for item in drops}), "reasons": reasons, "events": drops, "decoder_error_events": decoder_errors, "raw_json_records": json_records, "raw_json_parse_errors": json_errors})

    start = int(datetime.fromisoformat(result["started"]).timestamp() * 1000)
    finish = int(datetime.fromisoformat(result["finished"]).timestamp() * 1000)
    db = sqlite3.connect((root / "data" / "quality.db").as_uri() + "?mode=ro", uri=True)
    summaries = {}
    for chain_id, payload in db.execute("SELECT chain,payload FROM samples WHERE time_ms>=? AND time_ms<?", (start + 30000, finish)):
        if chain_id not in {chain["id"] for chain in latest}:
            continue
        value = json.loads(payload)
        summary = summaries.setdefault(chain_id, {"samples": 0, "zero_fps": 0, "fps_below28": 0, "min_fps": None, "invalid_latency": 0, "invalid_sync": 0})
        summary["samples"] += 1
        fps = value.get("output_fps", 0)
        summary["zero_fps"] += fps <= 0
        summary["fps_below28"] += fps < 28
        summary["min_fps"] = fps if summary["min_fps"] is None else min(fps, summary["min_fps"])
        summary["invalid_latency"] += value.get("latency_ms") is None
        summary["invalid_sync"] += value.get("av_offset_ms") is None
    freezes = []
    for chain_id, stamp, payload in db.execute("SELECT chain,time_ms,payload FROM events WHERE type='freeze' AND time_ms>=? AND time_ms<? ORDER BY time_ms", (start + 30000, finish)):
        freezes.append({"id": chain_id, "time_ms": stamp, "payload": json.loads(payload)})
    db.close()
    report = {"raw_unknown_windows": raw, "presentation_drop_reviews": drop_reviews, "persisted_one_second_metrics": summaries, "freeze_events": freezes, "note": "No receiver match does not prove network packet loss; shared decoder and identity limitations remain. Raw decoding-completion events are not recorded per frame."}
    (directory / "raw-audit.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps({"unknown_units": {item["id"]: item["unique_unresolved_units"] for item in raw}, "freeze_events": len(freezes), "persisted_metrics": summaries}, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
