"""Audit per-frame timing without inferring stages from unrelated averages."""
import argparse
import json
import statistics
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('directory', type=Path)
args = parser.parse_args()
root = Path(__file__).resolve().parent.parent
result = json.loads((args.directory / 'result.json').read_text(encoding='utf-8-sig'))
errors, summary = [], {}
def stats(values):
    if not values: return {'count': 0}
    values = sorted(values)
    return {'count': len(values), 'mean_ms': statistics.mean(values),
            'median_ms': statistics.median(values), 'p95_ms': values[int((len(values)-1)*.95)]}
for chain in result['final']:
    events = {}
    for path in (root / 'data/observations').glob(chain['session'] + '*.jsonl'):
        if 'truth' in path.name: continue
        for line in path.read_text(encoding='utf-8-sig').splitlines():
            x = json.loads(line)
            if x.get('id'): events[x.get('stage'), x['id']] = x
    fields = {k: [] for k in ('transport','hash','lock_wait','match','association','source_to_write',
                             'write_call','source_to_rtp_marker','rtp_marker_to_read', 'next_source_to_read')}
    source_order = sorted((v for (stage, _), v in events.items() if stage == 'source_release' and v.get('video')),
                          key=lambda x: x['mono_us'])
    next_source = {x['id']: y['mono_us'] for x, y in zip(source_order, source_order[1:])}
    count = 0
    for (stage, fid), assoc in events.items():
        if stage != 'receiver_association': continue
        src = events.get(('source_release',fid)); rx = events.get(('received_matched',fid))
        if not src or not rx: errors.append([chain['id'],fid,'missing source/receive']); continue
        # Exclude connection warm-up from aggregate comparisons, still audit its timestamps.
        times = [assoc[k] for k in ('received_us','hash_started_us','hash_done_us','lock_requested_us',
                                   'lock_acquired_us','match_started_us','matched_us')]
        if times != sorted(times) or times[0] != rx['mono_us']:
            errors.append([chain['id'],fid,'association clocks'])
        count += 1
        if src['mono_us'] < source_order[0]['mono_us'] + 15_000_000: continue
        def add(name,a,b):
            if b < a: errors.append([chain['id'],fid,name,'negative'])
            else: fields[name].append((b-a)/1000)
        add('transport',src['mono_us'],times[0])
        add('hash',times[1],times[2]); add('lock_wait',times[3],times[4])
        add('match',times[5],times[6]); add('association',times[0],times[6])
        write=events.get(('publish_write',fid))
        if write:
            add('source_to_write',src['mono_us'],write['write_started_us'])
            add('write_call',write['write_started_us'],write['write_done_us'])
        rtp=events.get(('rtp_boundary_observation',fid))
        if rtp and rtp.get('marker_seen') and rtp['packets_before'] == rtp['packets_after']:
            add('source_to_rtp_marker',src['mono_us'],rtp['last_packet_us'])
            add('rtp_marker_to_read',rtp['last_packet_us'],times[0])
        if fid in next_source and next_source[fid] <= times[0]:
            add('next_source_to_read',next_source[fid],times[0])
    summary[chain['id']]={'session':chain['session'],'audited_associations':count,
                          'stages':{k:stats(v) for k,v in fields.items()}}
audit={'chains':summary,'errors':errors,'scope':'Monotonic local timestamps; receive is complete av_read_frame output, RTP marker is observed at the local UDP relay before forwarding, not NIC arrival. Warm-up excluded from aggregates.'}
(args.directory/'receive-audit.json').write_text(json.dumps(audit,indent=2)+'\n',encoding='utf-8')
print(json.dumps(audit))
assert not errors
