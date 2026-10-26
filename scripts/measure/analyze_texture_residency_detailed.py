"""Validate repeated memory/FPS captures, summarize run medians, and export figures (Pillow/matplotlib)."""
import argparse
import bisect
import csv
import hashlib
import json
import statistics
from pathlib import Path
from PIL import Image, ImageChops


def aggregate(rows, rounds):
    expected={(r,c,on) for r in range(1,rounds+1) for c in ('far','near','sweep') for on in (False,True)}
    if len(rows)!=len(expected) or {(r['round'],r['camera'],r['selective']) for r in rows}!=expected:
        raise ValueError('incomplete or duplicate run matrix')
    result={}
    keys=('fps','frame_mean_ms','frame_p95_ms','frame_p99_ms','frame_max_ms','one_percent_low_fps',
        'first_frame_ms','cold_max_ms','dedicated_bytes_median','dedicated_bytes_peak','dedicated_bytes_process_peak',
        'shared_bytes_median','working_set_bytes_median','working_set_bytes_process_peak')
    for camera in ('far','near','sweep'):
        result[camera]={}
        for enabled in (False,True):
            group=[r for r in rows if r['camera']==camera and r['selective']==enabled]
            arm={}
            for key in keys:
                values=[r[key] for r in group]
                arm[key]=dict(median=statistics.median(values),minimum=min(values),maximum=max(values),runs=values)
            result[camera]['selective' if enabled else 'eager']=arm
        off,on=result[camera]['eager'],result[camera]['selective']
        result[camera]['fps_change_percent']=100*(on['fps']['median']/off['fps']['median']-1)
        result[camera]['dedicated_reduction_percent']=100*(1-on['dedicated_bytes_median']['median']/off['dedicated_bytes_median']['median'])
        result[camera]['dedicated_saved_bytes']=off['dedicated_bytes_median']['median']-on['dedicated_bytes_median']['median']
    return result


def figures(root,rows,summary):
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    plt.rcParams.update({'font.size':11,'axes.spines.top':False,'axes.spines.right':False})
    cameras=('far','near','sweep')
    fig,axes=plt.subplots(1,2,figsize=(11,4),layout='constrained')
    for ax,key,scale,label in ((axes[0],'dedicated_bytes_median',2**30,'Process dedicated GPU memory (GiB)'),
                               (axes[1],'fps',1,'Steady present throughput (FPS)')):
        for enabled,color,offset,name in ((False,'#4c78a8',-.18,'Eager'),(True,'#e88735',.18,'Selective')):
            values=[summary[c]['selective' if enabled else 'eager'][key]['median']/scale for c in cameras]
            ax.bar([i+offset for i in range(3)],values,.34,color=color,label=name,alpha=.85)
            for i,camera in enumerate(cameras):
                samples=[r[key]/scale for r in rows if r['camera']==camera and r['selective']==enabled]
                ax.scatter([i+offset]*len(samples),samples,c='#242424',s=13,zorder=3)
                ax.annotate(f'{values[i]:.2f}',(i+offset,max(samples)),xytext=(0,6),textcoords='offset points',ha='center',fontsize=10)
        ax.set_xticks(range(3),('Far','Near','Moving camera'))
        ax.set_ylabel(label)
        ax.set_ylim(0,ax.get_ylim()[1]*1.12)
        ax.grid(axis='y',alpha=.2)
        ax.set_axisbelow(True)
    axes[0].legend(loc='upper right')
    fig.suptitle('Bistro, 1280x720 | 3 interleaved rounds | 120 warm-up + 480 measured frames\nBars: median of runs; dots: each run. WDDM process counters; vbuffer only.')
    fig.savefig(root/'comparison.png',dpi=170)
    plt.close(fig)
    fig,ax=plt.subplots(figsize=(10,4),layout='constrained')
    for enabled,color,name in ((False,'#4c78a8','Eager'),(True,'#e88735','Selective')):
        run=root/f'r1-sweep-{"selective" if enabled else "eager"}'
        with (run/'frames.csv').open() as stream: frames=list(csv.DictReader(stream))[:600]
        boundaries=[float(f['unix_ms']) for f in frames]
        memory=[json.loads(s) for s in (run/'memory.jsonl').read_text().splitlines()]
        samples=[r for r in memory if boundaries[0]<=r['unix_ms']<=boundaries[-1]]
        ax.plot([bisect.bisect_right(boundaries,r['unix_ms']) for r in samples],
                [r['dedicated_bytes']/2**30 for r in samples],label=name,color=color)
    ax.axvspan(0,120,color='#999999',alpha=.15,label='Warm-up (excluded from FPS)')
    ax.set(xlabel='Presented frame (0.5 degrees yaw per frame)',ylabel='Process dedicated GPU memory (GiB)',
           title='Moving-camera memory samples, round 1 (approximately 0.5–1 second cadence)')
    ax.legend(loc='best'); ax.grid(alpha=.2)
    fig.savefig(root/'moving-memory.png',dpi=170)
    plt.close(fig)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory',type=Path)
    args=parser.parse_args(); root=args.directory
    manifest=json.loads((root/'manifest.json').read_text())
    rows=json.loads((root/'results.json').read_text())
    if hashlib.sha256(Path(manifest['exe']).read_bytes()).hexdigest()!=manifest['exe_sha256']:
        raise ValueError('executable changed during experiment')
    summary=aggregate(rows,manifest['rounds'])
    for row in rows:
        run=root/row['tag']
        if len(list(run.glob('screenshot_*.png')))!=1: raise ValueError(f'{run}: missing/ambiguous capture')
        with (run/'frames.csv').open() as stream: frames=list(csv.DictReader(stream))
        if len(frames) not in (manifest['frames'],manifest['frames']+1): raise ValueError('unexpected frame count')
        if row['frame_count']!=manifest['frames']-manifest['warmup']: raise ValueError('wrong warm-up exclusion')
    matches=[]
    for round_id in range(1,manifest['rounds']+1):
        for camera in ('far','near','sweep'):
            images=[Image.open(next((root/f'r{round_id}-{camera}-{kind}').glob('screenshot_*.png'))).convert('RGB')
                    for kind in ('eager','selective')]
            equal=ImageChops.difference(*images).getbbox() is None
            matches.append(dict(round=round_id,camera=camera,rgb_identical=equal))
    if not all(r['rgb_identical'] for r in matches): raise ValueError(f'pixel mismatch: {matches}')
    output=dict(method='median of per-run statistics; FPS = frames / sum(present intervals)',summary=summary,pixel_checks=matches)
    (root/'analysis.json').write_text(json.dumps(output,indent=2))
    figures(root,rows,summary)
    print(json.dumps(output,indent=2))


if __name__=='__main__': main()
