"""Verify UDP per-frame attribution from observations, without reading injection truth."""
import argparse,json
from collections import Counter
from pathlib import Path
parser=argparse.ArgumentParser()
parser.add_argument('directory',type=Path)
args=parser.parse_args()
root=Path(__file__).resolve().parent.parent
result=json.loads((args.directory/'result.json').read_text(encoding='utf-8-sig'))
session=result['final']['session']
events=[]
for p in sorted((root/'data/observations').glob(session+'*.jsonl')):
    if 'truth' not in p.name:
        events.extend(json.loads(line) for line in p.read_text(encoding='utf-8-sig').splitlines())
source={};rtp={};submitted={};decoded={};hash_anchor=None
errors=[];outcomes=Counter();recovered=0;examples={}
for e in events:
    fid=e.get('id');stage=e.get('stage')
    if stage=='source_release':source[fid]=e
    elif stage=='rtp_boundary_observation':rtp[fid]=e
    elif stage=='received_matched' and e.get('video'):
        if e.get('evidence')=='payload_sha256' and rtp.get(fid,{}).get('marker_seen'): hash_anchor=e
        elif e.get('evidence')=='rtp_boundary_and_validated_pts':
            recovered+=1
            boundary=rtp.get(fid)
            if not boundary or not boundary.get('marker_seen') or not boundary['packets_after']:
                errors.append([fid,'no verified forwarded RTP boundary'])
            if not hash_anchor or hash_anchor['generation']!=e['generation']:
                errors.append([fid,'no same-generation payload anchor'])
            elif abs(e['source_media_us']-e['rx_media_us']-hash_anchor['source_media_us']+hash_anchor['rx_media_us'])>2000:
                errors.append([fid,'PTS mapping disagrees with payload anchor'])
            elif boundary:
                anchor_rtp=rtp[hash_anchor['id']]['rtp_timestamp']
                offset=(anchor_rtp-round(hash_anchor['rx_media_us']*.09))&0xffffffff
                expected=(round(e['rx_media_us']*.09)+offset)&0xffffffff
                distance=(boundary['rtp_timestamp']-expected)&0xffffffff
                if min(distance,(-distance)&0xffffffff)>180:
                    errors.append([fid,'RTP clock does not independently validate received PTS'])
    elif stage=='decode_submission' and e['result']==0:submitted[fid]=e
    elif stage=='decoded_output' and fid:decoded[fid]=e
    elif stage=='frame_resolution':
        outcome=e['outcome'];outcomes[outcome]+=1
        examples.setdefault(outcome,[])
        if len(examples[outcome])<3:examples[outcome].append(e)
        if outcome in ('transport_lost','damaged_decode_no_output'):
            boundary=rtp.get(fid)
            if not boundary or not boundary['packets_after']<boundary['packets_before']:
                errors.append([fid,'no actual RTP damage observation'])
            if fid in decoded and decoded[fid]['mono_us']<=e['mono_us']:
                errors.append([fid,'already decoded frame called missing'])
        if outcome=='transport_lost':
            send=submitted.get(fid)
            if send and send['generation']==e['generation']:
                errors.append([fid,'successful submission misclassified as pre-decode transport loss'])
        if outcome=='damaged_decode_no_output':
            send=submitted.get(fid)
            if not send or send['generation']!=e['generation'] or e['mono_us']-send['mono_us']<=5_000_000:
                errors.append([fid,'missing successful same-generation submission and timeout'])
audit={'session':session,'resolution_counts':dict(outcomes),'rtp_pts_associations':recovered,
       'unmapped_submissions':sum(e.get('stage')=='decode_submission' and not e.get('id') for e in events),
       'unmapped_outputs':sum(e.get('stage')=='decoded_output' and not e.get('id') for e in events),
       'errors':errors,'examples':examples,
       'scope':'RTP boundary packet counts, source identity, validated PTS mapping, successful decoder submission and missing output at timeout. Injection truth is not read. Partial loss may still decode; no exact reference-picture dependency claim.'}
(args.directory/'udp-audit.json').write_text(json.dumps(audit,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
print(json.dumps({k:v for k,v in audit.items() if k!='examples'},ensure_ascii=False))
assert not errors
assert outcomes['transport_lost']>0,'no observed pre-decode UDP loss classification'
assert outcomes['damaged_decode_no_output']>0,'no observed damaged accepted input timeout classification'
