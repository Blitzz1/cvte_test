"""Check measured pipeline times against source / receive / decode / present facts."""
import json
import argparse
from pathlib import Path
from collections import Counter
parser = argparse.ArgumentParser()
parser.add_argument('directory', type=Path)
args = parser.parse_args()
root = Path(__file__).resolve().parent.parent
result = json.loads((args.directory / 'result.json').read_text(encoding='utf-8-sig'))
errors, counts = [], Counter()
for chain in result['final']:
    stages, timings = {}, []
    for path in (root / 'data/observations').glob(chain['session'] + '*.jsonl'):
        if 'truth' in path.name: continue
        for line in path.read_text(encoding='utf-8-sig').splitlines():
            x = json.loads(line)
            if x.get('stage') == 'pipeline_timing': timings.append(x)
            elif x.get('id'):
                stages[x.get('stage'), x['id']] = x
    for x in timings:
        fid = x['id']
        times = [x[k] for k in ('source_us','received_us','decode_started_us','submitted_us','decoded_us','presented_us')]
        if times != sorted(times): errors.append([chain['id'], fid, 'non-monotonic times'])
        for stage, key in [('source_release','source_us'),('received_matched','received_us'),
                           ('decode_submission','submitted_us'),('decoded_output','decoded_us'),
                           ('software_present','presented_us')]:
            event = stages.get((stage, fid))
            if not event or event['mono_us'] != x[key]: errors.append([chain['id'], fid, stage])
        submission = stages.get(('decode_submission', fid))
        if not submission or submission.get('call_started_us') != x['decode_started_us']:
            errors.append([chain['id'], fid, 'decode_call_start'])
        counts[chain['id']] += 1
audit = {'same_frame_pipeline_records':dict(counts),'errors':errors,
         'scope':'All six times independently matched to raw observation facts, including the decoder call start; source-to-receive is measured directly, not inferred by subtracting unrelated window means.'}
(args.directory / 'timing-audit.json').write_text(json.dumps(audit,indent=2)+'\n',encoding='utf-8')
print(json.dumps(audit))
assert not errors
