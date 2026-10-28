"""Summarize frame identities in one raw chain journal without guessing loss causes."""
import collections
import json
import sys
from pathlib import Path

rows = [json.loads(line) for line in Path(sys.argv[1]).read_text(encoding="utf-8-sig").splitlines()]
sources = {r["id"]: r for r in rows if r["stage"] == "source_release" and r["video"]}
received = {r["id"]: r for r in rows if r["stage"] == "received_matched" and r["video"]}
outputs = [r for r in rows if r["stage"] == "software_present"]
submissions = {r["id"]: r for r in rows if r["stage"] == "decode_submission"}
decoded = {r["id"]: r for r in rows if r["stage"] == "decoded_output"}
counts = collections.Counter(r["id"] for r in outputs)
origin = min(r["mono_us"] for r in sources.values())
groups = collections.defaultdict(collections.Counter)
for identity, source in sources.items():
    groups[int((source["mono_us"] - origin) / 1_000_000)].update({
        "source": 1, "matched": int(identity in received),
        "submitted": int(identity in submissions), "decoded": int(identity in decoded),
        "presented": int(identity in counts), "duplicates": max(0, counts[identity] - 1),
    })
    if identity in received:
        bucket = groups[int((source["mono_us"] - origin) / 1_000_000)]
        bucket["max_receive_delay_ms"] = max(bucket["max_receive_delay_ms"],
                                            round((received[identity]["mono_us"] - source["mono_us"]) / 1000, 3))
report = {
    "seconds": dict(groups),
    "duplicate_ids": [(i, count) for i, count in counts.items() if i and count > 1],
    "loops": [r for r in rows if r["stage"] == "source_loop"],
    "unmapped_outputs": sum(r["id"] == 0 for r in outputs),
    "decoder_output_by_wall_second": dict(collections.Counter(int((r["mono_us"]-origin)/1_000_000)
                                                              for r in rows if r["stage"] == "decoded_output")),
    "submission_by_wall_second": dict(collections.Counter(int((r["mono_us"]-origin)/1_000_000)
                                                           for r in rows if r["stage"] == "decode_submission")),
    "large_submissions": [r for r in rows if r["stage"] == "decode_submission" and r.get("packet_size", 0) > 100000],
    "submission_flags": dict(collections.Counter(r.get("packet_flags") for r in rows if r["stage"] == "decode_submission")),
    "unresolved_examples": [{"id": i, "media_us": s["media_us"], "rx_media_us": received.get(i, {}).get("rx_media_us"),
                             "submission": submissions.get(i), "decoded": decoded.get(i)}
                            for i, s in sources.items() if i in received and i not in counts][:30],
}
print(json.dumps(report, indent=2))
