"""Verify timeout attribution against raw stage observations, independently of injection truth."""
import argparse
import json
from collections import Counter
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('directory', type=Path)
args = parser.parse_args()
result = json.loads((args.directory / 'result.json').read_text(encoding='utf-8-sig'))
session = result['final']['session']
root = Path(__file__).resolve().parent.parent
submitted, decoded, losses = {}, {}, {}
resolutions, states, errors = [], Counter(), []
for path in sorted((root / 'data/observations').glob(session + '*.jsonl')):
    if 'truth' in path.name:
        continue
    for line in path.read_text(encoding='utf-8-sig').splitlines():
        x = json.loads(line)
        stage, fid = x.get('stage'), x.get('id')
        if stage == 'decode_submission' and x['result'] == 0:
            submitted[fid] = x
        elif stage == 'decoded_output':
            decoded[fid] = x
        elif stage == 'compressed_unit_discarded' or (stage == 'rtp_boundary_observation' and x['packets_after'] < x['packets_before']):
            losses[fid] = x
        elif stage == 'decode_receive_status':
            states[x['state']] += 1
        elif stage == 'frame_resolution' and x['outcome'] in (
                'decode_no_output', 'decode_after_input_loss', 'damaged_decode_no_output'):
            resolutions.append(x)
            send = submitted.get(fid)
            if not send or send['generation'] != x['generation']:
                errors.append([fid, 'missing successful same-generation submission'])
            if x['mono_us'] - (x.get('submitted_us') or x['mono_us']) <= 5_000_000:
                errors.append([fid, 'premature timeout'])
            if fid in decoded and decoded[fid]['mono_us'] <= x['mono_us']:
                errors.append([fid, 'decoded frame incorrectly resolved missing'])
            if x['outcome'] == 'decode_after_input_loss':
                gap = losses.get(x['preceding_input_loss_id'])
                if not gap or gap['mono_us'] > send['mono_us']:
                    errors.append([fid, 'no preceding observed input loss'])
            if x['outcome'] == 'damaged_decode_no_output' and not x['transport_damaged']:
                errors.append([fid, 'no transport damage evidence'])
audit = {'session': session, 'timeout_outcomes': dict(Counter(x['outcome'] for x in resolutions)),
         'decoder_receive_states': dict(states), 'errors': errors,
         'scope': 'Successful submission, configured 5-second wait, no earlier matched decode output and actual preceding loss observations. Does not prove exact H.264 reference dependencies; injection truth is not read.'}
(args.directory / 'identity-audit.json').write_text(json.dumps(audit, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')
print(json.dumps(audit, ensure_ascii=False))
assert not errors
