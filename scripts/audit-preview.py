"""Verify HTTP preview identities against actual software-present observations."""
import json
import sys
from pathlib import Path

directory=Path(sys.argv[1]).resolve()
root=Path(__file__).resolve().parent.parent
def read(name):
    return json.loads((directory/name).read_text(encoding='utf-8-sig'))
def flat(value):
    return value.get('value',[]) if isinstance(value,dict) else value
sessions=set()
for name in ('warming.json','drop100.json','final.json'):
    sessions.update(c['session'] for c in flat(read(name)) if c.get('session'))
presented=set()
for session in sessions:
    for path in (root/'data'/'observations').glob(session+'*.jsonl'):
        if 'truth' in path.name: continue
        for line in path.read_text(encoding='utf-8-sig').splitlines():
            row=json.loads(line)
            if row['stage']=='software_present':
                chain=session.rsplit('-',1)[0]
                presented.add((chain,int(row['generation']),int(row['id']),int(row['mono_us'])))
rows=[json.loads(line) for line in (directory/'preview-frames.jsonl').read_text(encoding='utf-8-sig').splitlines()]
images=[r for r in rows if r['status']==200]
unmatched=[r for r in images if (r['id'],int(r['generation']),int(r['frame_id']),int(r['presented_us'])) not in presented]
report={'checked_jpeg_responses':len(images),'unmatched_actual_presentations':len(unmatched),
        'sessions':sorted(sessions),'passed':bool(images) and not unmatched,
        'scope':'Every delivered JPEG identity/time equals a real WIC software_present record. No optical calibration or one-hour stability claim.'}
(directory/'identity-audit.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
print(json.dumps(report,indent=2))
if not report['passed']: raise SystemExit(1)
