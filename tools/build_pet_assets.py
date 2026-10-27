"""Offline packer for reviewed companion artwork; never required by the runtime."""
from pathlib import Path
import argparse, hashlib, json
import numpy as np
from PIL import Image

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--poses', type=Path, required=True)
parser.add_argument('--states', type=Path, required=True)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
out = root / 'src/app/assets'
size, columns, baseline, body = 256, 8, 240, 208
state_sheet = Image.open(args.states).convert('RGBA')
pixels = np.array(state_sheet)
h, w = pixels.shape[:2]
ycuts = [0, min(range(h//2-h//20, h//2+h//20),
                key=lambda y: ((pixels[y,:,3]>100).sum(),abs(y-h//2))), h]

def trim(im):
    mask = im.getchannel('A').point(lambda v:255 if v>100 else 0)
    box = mask.getbbox()
    assert box, 'Missing character silhouette'
    return im.crop((max(0,box[0]-3),max(0,box[1]-3),
                    min(im.width,box[2]+3),min(im.height,box[3]+3)))

states = []
for top,bottom in zip(ycuts,ycuts[1:]):
    count = (pixels[top:bottom,:,3]>100).sum(0)
    step = w//4
    cuts = [0]+[min(range(b-step//6,b+step//6),key=lambda x:(count[x],abs(x-b)))
                for b in (step,step*2,step*3)]+[w]
    for left,right in zip(cuts,cuts[1:]):
        states.append(trim(Image.fromarray(pixels[top:bottom,left:right])))

inputs, images = [], []
for i in range(24):
    name = 'c2' if i==16 else f'o{i}'
    path = args.poses/(name+'.png')
    images.append(Image.open(path).convert('RGBA'))
    inputs.append({'slot':i,'source':name,'sha256':hashlib.sha256(path.read_bytes()).hexdigest()})
for i in range(4):
    path = args.poses/f'c{i}.png'
    images.append(Image.open(path).convert('RGBA'))
    inputs.append({'slot':24+i,'source':f'c{i}','sha256':hashlib.sha256(path.read_bytes()).hexdigest()})
images += states
base_factor = body / images[17].height
state_factor = body / states[0].height
atlas = Image.new('RGBA',(size*columns,size*5))
for i,im in enumerate(images):
    factor = state_factor if i>=28 else base_factor
    sprite = im.resize((round(im.width*factor),round(im.height*factor)),Image.Resampling.LANCZOS)
    assert sprite.width<=size-8 and sprite.height<=baseline-4,(i,sprite.size)
    x=(i%columns)*size+(size-sprite.width)//2
    y=(i//columns)*size+baseline-sprite.height
    atlas.alpha_composite(sprite,(x,y))
path = out/'ai-pet.png'
atlas.quantize(colors=255,method=Image.Quantize.FASTOCTREE).save(path,optimize=True)
manifest={'frames':36,'columns':columns,'rows':5,'cell':size,'baseline':baseline,'body':body,
          'sha256':hashlib.sha256(path.read_bytes()).hexdigest(),
          'reference':'https://github.com/fornarwhal/deepseek-whale-girl-icon',
          'license':'CC-BY-NC-SA-4.0','generated_states_sha256':hashlib.sha256(args.states.read_bytes()).hexdigest(),
          'rejected_pose':'original frame16 is excluded; slot16 is the reviewed corrected cross',
          'state_slots':{'idle':28,'wave':[29,30,31],'thinking':32,'working':33,'done':34,'concern':35},
          'inputs':inputs}
(out/'ai-pet.json').write_text(json.dumps(manifest,indent=2)+'\n')
print(json.dumps({'path':str(path),'bytes':path.stat().st_size,'frames':len(images),'sha256':manifest['sha256']}))
