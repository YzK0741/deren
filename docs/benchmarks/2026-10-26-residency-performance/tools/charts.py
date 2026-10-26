"""Create standalone performance figures from audited measurements using Pillow."""
import bisect
import csv
import json
from pathlib import Path
import statistics as st
from PIL import Image, ImageDraw, ImageFont

ROOT=Path(__file__).resolve().parents[4]
BASE=ROOT/'tmp/residency-review-pinned-20261026'
STRESS=ROOT/'tmp/residency-review-pinned-stress-20261026'
OUT=ROOT/'docs/benchmarks/2026-10-26-residency-performance'
OUT.mkdir(exist_ok=True,parents=True)
audit=json.loads((BASE/'audit.json').read_text())
summary={}
for camera in ('far','near','sweep'):
    summary[camera]={}
    for mode in ('eager','selective'):
        rows=[r for r in audit['baseline'] if f'-{camera}-{mode}' in r['tag']]
        keys=('fps','frame_mean_ms','frame_p95_ms','frame_p99_ms','frame_max_ms','one_percent_low_fps',
              'first_frame_ms','cold_max_ms','process_wall_seconds','dedicated_bytes_median','dedicated_bytes_peak',
              'dedicated_bytes_process_peak','shared_bytes_median','working_set_bytes_median','frames_over_50_ms','frames_over_100_ms')
        summary[camera][mode]={key:dict(median=st.median(r[key] for r in rows),minimum=min(r[key] for r in rows),
                                          maximum=max(r[key] for r in rows),runs=[r[key] for r in rows]) for key in keys}
    off,on=summary[camera]['eager'],summary[camera]['selective']
    summary[camera]['fps_change_percent']=100*(on['fps']['median']/off['fps']['median']-1)
    summary[camera]['dedicated_reduction_percent']=100*(1-on['dedicated_bytes_median']['median']/off['dedicated_bytes_median']['median'])
summary['pixel_pairs']=audit['pixel_pairs']
summary['exact_rgb_gate_passed']=audit['exact_rgb_gate_passed']
summary['stress']=audit.get('stress',[])
(OUT/'summary.json').write_text(json.dumps(summary,indent=2))
font_path='C:/Windows/Fonts/arial.ttf'
font=lambda size:ImageFont.truetype(font_path,size)
ink='#203044';muted='#607080';colors=('#3879b7','#df7b29')


im=Image.new('RGB',(1440,650),'#ffffff');d=ImageDraw.Draw(im)
d.text((40,24),'Texture residency: memory saved, frame time paid',fill=ink,font=font(30))
d.text((40,70),'RTX 4060 Laptop 8 GB | Bistro 1280 x 720 | frozen scene animation | same Release build',fill=muted,font=font(18))
for k,(label,c) in enumerate((('Eager (feature off)',colors[0]),('Selective (feature on)',colors[1]))):
    x=40+k*330;d.rectangle((x,112,x+20,132),fill=c);d.text((x+30,109),label,fill=ink,font=font(18))
for panel,(key,title,scale) in enumerate((('fps','Throughput (FPS)',1),('dedicated_bytes_median','Process dedicated GPU memory (GiB)',2**30),('frame_p99_ms','P99 frame interval (ms)',1))):
    left=58+panel*473;right=left+400;top=206;bottom=535
    d.text((left-10,166),title,fill=ink,font=font(19))
    groups=[]
    for camera in ('far','near','sweep'):
        group=[]
        for mode in ('eager','selective'):
            group.append([r[key]/scale for r in audit['baseline'] if f'-{camera}-{mode}' in r['tag']])
        groups.append(group)
    maximum=max(v for g in groups for a in g for v in a)*1.22
    for n in range(5):
        y=bottom-n*(bottom-top)/4
        d.line((left,y,right,y),fill='#e4e9ee',width=1)
        value=n*maximum/4;d.text((left-42,y-9),f'{value:.1f}' if scale>1 else f'{value:.0f}',fill=muted,font=font(14))
    for index,(name,group) in enumerate(zip(('Far','Near','Moving'),groups)):
        center=left+67+index*133
        d.text((center,bottom+14),name,fill=ink,font=font(18),anchor='mt')
        for arm,values in enumerate(group):
            x=center-45+arm*48;median=st.median(values);y=bottom-median/maximum*(bottom-top)
            d.rectangle((x,y,x+38,bottom),fill=colors[arm])
            for value in values:
                yy=bottom-value/maximum*(bottom-top)
                d.ellipse((x+16,yy-3,x+22,yy+3),fill=ink)
            d.text((x+19,min(y,bottom-max(values)/maximum*(bottom-top))-13),f'{median:.2f}' if scale>1 else f'{median:.1f}',fill=ink,font=font(15),anchor='mb')
d.text((40,602),'Bars: median of 3 runs. Dots: individual runs. Each run: 120 warm-up + 480 measured frames.',fill=muted,font=font(18))
im.save(OUT/'comparison.png')

if 'stress' in audit:
    im=Image.new('RGB',(1280,470),'white');d=ImageDraw.Draw(im)
    d.text((35,20),'Five camera revolutions: observed process GPU memory',fill=ink,font=font(27))
    d.text((35,60),'3600 frames at 0.5 degrees/frame | first 120 frames excluded | one run per mode',fill=muted,font=font(18))
    left,right,top,bottom=95,1220,110,370
    for value in range(6):
        y=bottom-value/5*(bottom-top);d.line((left,y,right,y),fill='#e4e9ee');d.text((left-15,y),str(value),fill=muted,font=font(16),anchor='rm')
    for n in range(6):
        x=left+n/5*(right-left);d.line((x,top,x,bottom),fill='#eff2f5');d.text((x,bottom+12),str(n),fill=ink,font=font(17),anchor='mt')
    for arm,mode in enumerate(('eager','selective')):
        run=STRESS/'long-loop'/f'r1-sweep-{mode}'
        trace=list(csv.DictReader((run/'frames.csv').open()))[:3600];bounds=[float(r['unix_ms']) for r in trace]
        memory=[json.loads(s) for s in (run/'memory.jsonl').read_text().splitlines()]
        points=[]
        for r in memory:
            i=bisect.bisect_right(bounds,r['unix_ms'])
            if bounds[119]<=r['unix_ms']<=bounds[-1]:
                points.append((left+(i/720)/5*(right-left),bottom-r['dedicated_bytes']/2**30/5*(bottom-top)))
        d.line(points,fill=colors[arm],width=3)
        d.text((left+arm*360,426),mode.title(),fill=colors[arm],font=font(19))
    d.text((left-70,top-18),'GiB',fill=muted,font=font(17))
    d.text(((left+right)/2,bottom+42),'Camera revolutions (frame-index aligned)',fill=ink,font=font(18),anchor='mt')
    im.save(OUT/'long-loop-memory.png')
print('Figures written to',OUT)
