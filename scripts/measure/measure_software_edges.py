"""Three-round interleaved triangle-edge threshold sweep against software-off HZB."""
import argparse,json,statistics
from pathlib import Path
from measure_render_optimizations import run_one,CASES,sha256,shader_bundle_sha256

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--work',type=Path,required=True)
    parser.add_argument('--asset',type=Path,required=True)
    parser.add_argument('--exe',type=Path,default=Path('build-release-dyn-clang64/deren.exe'))
    args=parser.parse_args()
    work,asset,exe=args.work.resolve(),args.asset.resolve(),args.exe.resolve()
    work.mkdir(parents=True,exist_ok=False)
    modes={'hardware':None,'sphere_1_0':1.0,'edge_0_5':0.5,'edge_1_0':1.0,'edge_2_0':2.0}
    for mode,threshold in modes.items():
        (work/mode).mkdir()
        if threshold is not None: CASES[mode]={**CASES['all_enabled'],'threshold_px':threshold}
    result={'complete':False,'executable_sha256':sha256(exe),'asset_sha256':sha256(asset),
            'shader_bundle_sha256':shader_bundle_sha256(exe.parent/'shaders'),'runs':[]}
    output=work/'results.json'
    order=list(modes)
    for round_id in range(1,4):
        rotated=order[round_id-1:]+order[:round_id-1]
        for camera in ('far','near'):
            for mode in rotated:
                software=mode!='hardware'
                edge=mode.startswith('edge_')
                row=run_one(exe,asset,work/mode,mode if software else 'hzb',camera,round_id,130,triangles=software,edge_routing=edge)
                if row.get('gpu_frames')!=129 or not row.get('hzb_cull_active') or bool(row.get('software_raster_active'))!=software:
                    raise ValueError(f'{mode}: missing timing/features')
                row['mode']=mode;result['runs'].append(row)
                output.write_text(json.dumps(result,indent=2)+'\n')
    result['summary']={}
    for mode in modes:
        result['summary'][mode]={}
        for camera in ('far','near'):
            rows=[r for r in result['runs'] if r['mode']==mode and r['camera']==camera]
            result['summary'][mode][camera]={key:statistics.median(r[key] for r in rows)
                for key in ('gpu_ms','cpu_active_ms','cut_wall_us','software_raster_ms','visibility_raster_ms')}
    if sha256(exe)!=result['executable_sha256'] or shader_bundle_sha256(exe.parent/'shaders')!=result['shader_bundle_sha256']:
        raise ValueError('executable/shaders changed during measurement')
    result['complete']=True;output.write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result['summary'],indent=2),flush=True)

if __name__=='__main__':main()
