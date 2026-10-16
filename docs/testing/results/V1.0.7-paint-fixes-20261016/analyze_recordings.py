"""Verify and sample recorded screen RGB; never render or replace application UI."""
import bisect
import hashlib
import json
from pathlib import Path
import subprocess
import sys

import numpy as np
from PIL import Image

source, target = map(Path, sys.argv[1:3])
target.mkdir(parents=True, exist_ok=True)
summary = []
for report_path in sorted(source.glob('*-paint.json')):
    report = json.loads(report_path.read_text(encoding='utf-8-sig'))
    recording = report['recording']
    video = source / recording['video']
    assert hashlib.file_digest(video.open('rb'), 'sha256').hexdigest() == recording['video_sha256']
    timeline = [json.loads(line) for line in (source / recording['actual_timeline']).read_text().splitlines()]
    encoded = sorted((frame for frame in timeline if frame['encoded_index'] is not None), key=lambda f: f['encoded_index'])
    assert [f['encoded_index'] for f in encoded] == list(range(recording['encoded_frames']))
    times = [f['seconds'] for f in encoded]
    selections = {}
    for click in report['sidebar_clicks']:
        phase = click['phase']
        if phase in [f'rapid-{n:03d}' for n in (0,24,49,74,99)]:
            offsets = [.016]
        elif phase in [f'normal-{direction}-{n:02d}' for direction in ('collapse','expand') for n in (0,9,19)]:
            offsets = [.040,.100,.180,.240,.260,.280,.300,.320]
        else:
            continue
        for offset in offsets:
            desired = click['observed_seconds'] + offset
            index = min(range(len(times)), key=lambda n: abs(times[n] - desired))
            selections.setdefault(index, []).append({'phase':phase,'requested_after_observed_release_ms':offset*1000,
                'desired_seconds':desired,'actual_seconds':times[index],'difference_ms':round((times[index]-desired)*1000,3)})
    originals = {}
    for entry in recording['saved_screen_samples']:
        path = source / entry['path']
        assert hashlib.sha256(path.read_bytes()).hexdigest() == entry['sha256']
        originals[entry['encoded_index']] = Image.open(path).convert('RGB').tobytes()
    width,height = recording['width'],recording['height']
    frame_bytes = width*height*3
    command = ['ffmpeg','-v','error','-i',str(video),'-f','rawvideo','-pix_fmt','rgb24','pipe:1']
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    count = matches = 0
    candidates, extracts = [],[]
    while True:
        raw = process.stdout.read(frame_bytes)
        if not raw:
            break
        assert len(raw) == frame_bytes
        assert count < len(encoded)
        if count in originals:
            assert raw == originals[count], f'Lossless decoded RGB mismatch at {video.name}:{count}'
            matches += 1
        if report['theme'] == 'dark':
            # Independent vectorized implementation of the exact sampled-grid
            # oracle: 8 horizontal neutral bright cells on 3 adjacent rows.
            step = max(4,round(4*report['dpi']/96))
            grid = np.frombuffer(raw,dtype=np.uint8).reshape(height,width,3)[::step,::step]
            light = (grid.min(axis=2)>=190) & ((grid.max(axis=2)-grid.min(axis=2))<=16)
            sums = np.pad(light.astype(np.int32).cumsum(axis=1),((0,0),(1,0)))
            horizontal = (sums[:,8:]-sums[:,:-8])==8
            if np.any(horizontal[2:] & horizontal[1:-1] & horizontal[:-2]):
                candidates.append(count)
        if count in selections:
            name = video.stem+f'-decoded-{count:05d}.png'
            path = target/name
            Image.frombytes('RGB',(width,height),raw).save(path)
            extracts.append({'path':name,'sha256':hashlib.sha256(path.read_bytes()).hexdigest(),
                'encoded_index':count,'original_sample_index':encoded[count]['sample_index'],
                'capture_bounds':recording['capture_bounds'],'selection':selections[count]})
        count += 1
    stderr = process.stderr.read().decode(errors='replace')
    assert process.wait()==0,stderr
    assert count == recording['encoded_frames'] and matches == len(originals)
    summary.append({'report':report_path.name,'status':report['status'],'video':video.name,
        'video_sha256':recording['video_sha256'],'decoded_frames':count,
        'original_pngs_pixel_identical_to_video':matches,
        'independent_dark_white_patch_frames':candidates,'extracted_original_pixel_frames':extracts,
        'method':'FFmpeg RGB24 lossless decode; PNG encoding only; no resizing, recoloring or reconstruction',
        'limits':'Temporal sampling has recorded gaps; vectorized test only detects the specified large light rectangles; inspect originals visually.'})
(target/'decoded-review-manifest.json').write_text(json.dumps(summary,indent=2),encoding='utf-8')
print(json.dumps([{k:v for k,v in row.items() if k!='extracted_original_pixel_frames'} for row in summary],indent=2))
