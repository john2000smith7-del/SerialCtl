"""Validate and summarize the real-click native stopwatch, without dropping samples."""
import argparse
import json
import math
from pathlib import Path


def percentile(values, fraction):
    ordered=sorted(values)
    return ordered[max(0,math.ceil(len(ordered)*fraction)-1)]


def summarize(path):
    dialogs={name:[] for name in ('ssh','serial','telnet','remote','ai','settings')}
    for line in path.read_text(encoding='utf-8-sig').splitlines():
        parts=line.strip().split(',')
        if parts[0] not in dialogs:continue
        if len(parts)<4:raise RuntimeError('Invalid stopwatch row: '+line)
        values={key:int(value) for key,value in (part.split('=',1) for part in parts[2:])}
        if values['responsive']!=1 or values['first_paint_ms']<0:raise RuntimeError('Incomplete/unresponsive sample: '+line)
        dialogs[parts[0]].append({'sample':int(parts[1]),**values})
    result={}
    for name,rows in dialogs.items():
        if [row['sample'] for row in rows]!=list(range(31)):
            raise RuntimeError(f'{path.name}/{name}: expected first plus 30 consecutive warm samples')
        metrics={}
        for field in ('first_paint_ms','responsive_ms'):
            warm=[row[field] for row in rows[1:]]
            metrics[field]={'first':rows[0][field],'warm_n':len(warm),'warm_p50':percentile(warm,.5),
                            'warm_p95':percentile(warm,.95),'warm_max':max(warm)}
        result[name]={'metrics':metrics,'raw_samples':rows}
    return result


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--log-dir',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--require-warm-p95-ms',type=int,default=100)
    args=parser.parse_args()
    report={'clock':'GetTickCount64, actual mouse input to full child GDI paint / WM_NULL response; not DWM presentation',
            'percentile':'nearest rank; first is a separate single sample, never cold P95','cases':{},'failures':[]}
    for theme in ('light','dark'):
        for kind in ('baseline','candidate'):
            rows=summarize(args.log_dir/f'{kind}-{theme}.log')
            report['cases'][f'{kind}/{theme}']=rows
            for name,row in rows.items():
                metrics=row['metrics']
                print(f'DIALOG_SUMMARY {kind}/{theme}/{name} '+json.dumps(metrics,separators=(',',':')))
                if kind=='candidate':
                    for field,value in metrics.items():
                        if value['warm_p95']>args.require_warm_p95_ms:
                            report['failures'].append(f'{theme}/{name}/{field}: warm P95 {value["warm_p95"]}ms exceeds {args.require_warm_p95_ms}ms')
                    if metrics['first_paint_ms']['first']>200:
                        report['failures'].append(f'{theme}/{name}: first full paint exceeds the original 200ms cold target')
    args.output.write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
    if report['failures']:raise RuntimeError('; '.join(report['failures']))


if __name__=='__main__':main()
