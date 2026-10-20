"""Approximate spec/gloss as core metallic/roughness for the Bistro test asset.
Math follows Khronos' convert-between-workflows/three.pbrUtilities.js reference:
https://github.com/KhronosGroup/glTF/tree/main/extensions/2.0/Archived/KHR_materials_pbrSpecularGlossiness/examples/convert-between-workflows
This approximation cannot preserve every colored dielectric/specular effect.
Requires NumPy and Pillow. Geometry and node hierarchy are left intact.
"""
import argparse
import copy
import json
from pathlib import Path
import numpy as np
from PIL import Image


def convert(diffuse, specular, gloss):
    strength = 1 - np.max(specular, axis=-1)
    brightness = lambda x: np.sqrt(np.sum(x*x*np.array([.299,.587,.114]), axis=-1))
    spec = brightness(specular)
    b = brightness(diffuse)*strength/.96 + spec - .08
    metallic = np.where(spec < .04, 0, np.clip((-b+np.sqrt(np.maximum(b*b-.16*(.04-spec), 0)))/.08, 0, 1))
    from_diffuse = diffuse * (strength/.96/np.maximum(1-metallic, 1e-6))[..., None]
    from_specular = (specular-.04*(1-metallic)[..., None])/np.maximum(metallic, 1e-6)[..., None]
    base = np.clip(from_diffuse*(1-metallic[..., None]**2)+from_specular*metallic[..., None]**2, 0, 1)
    return base, metallic, 1-gloss


def decode(x):
    return np.where(x <= .04045, x/12.92, ((x+.055)/1.055)**2.4)


def encode(x):
    return np.where(x <= .0031308, 12.92*x, 1.055*np.maximum(x, 0)**(1/2.4)-.055)


def prepare(source, output):
    assert output.parent.resolve() == source.parent.resolve(), 'Keep PNG references beside input'
    scene = json.loads(source.read_bytes())
    count = 0
    def load(info):
        if not info:
            return Image.new('RGBA', (1,1), 'white')
        assert info.get('texCoord', 0) == 0 and not info.get('extensions'), 'UV transforms need explicit conversion'
        texture = scene['textures'][info['index']]
        return Image.open(source.parent/scene['images'][texture['source']]['uri']).convert('RGBA')
    def texture(image, name, sampler):
        image.save(output.parent/name)
        image_id = len(scene['images'])
        scene['images'].append({'uri':name})
        result = len(scene['textures'])
        item = {'source':image_id}
        if sampler is not None:
            item['sampler'] = sampler
        scene['textures'].append(item)
        return result
    for number, material in enumerate(scene['materials']):
        spec = material.get('extensions', {}).pop('KHR_materials_pbrSpecularGlossiness', None)
        if spec is None:
            continue
        diff_info = spec.get('diffuseTexture')
        spec_info = spec.get('specularGlossinessTexture')
        if diff_info and spec_info:
            assert scene['textures'][diff_info['index']].get('sampler') == scene['textures'][spec_info['index']].get('sampler')
        diffuse_image = load(diff_info)
        spec_image = load(spec_info)
        size = (max(diffuse_image.width,spec_image.width),max(diffuse_image.height,spec_image.height))
        d = np.asarray(diffuse_image.resize(size, Image.Resampling.BILINEAR),dtype=np.float32)/255
        s = np.asarray(spec_image.resize(size, Image.Resampling.BILINEAR),dtype=np.float32)/255
        factor = spec.get('diffuseFactor',[1,1,1,1])
        base,metal,rough = convert(decode(d[:,:,:3])*factor[:3], decode(s[:,:,:3])*spec.get('specularFactor',[1,1,1]), s[:,:,3]*spec.get('glossinessFactor',1))
        rgba = np.concatenate([encode(base), (d[:,:,3]*factor[3])[:,:,None]],axis=-1)
        mr = np.stack([np.ones_like(metal),rough,metal],axis=-1)
        sampler_info = diff_info or spec_info
        sampler = scene['textures'][sampler_info['index']].get('sampler') if sampler_info else None
        albedo = texture(Image.fromarray(np.rint(np.clip(rgba,0,1)*255).astype('uint8')),f'material-{number:04d}-base.png',sampler)
        packed = texture(Image.fromarray(np.rint(np.clip(mr,0,1)*255).astype('uint8')),f'material-{number:04d}-mr.png',sampler)
        material['pbrMetallicRoughness'] = {'baseColorFactor':[1,1,1,1], 'baseColorTexture':{'index':albedo}, 'metallicFactor':1, 'roughnessFactor':1, 'metallicRoughnessTexture':{'index':packed}}
        if not material.get('extensions'):
            material.pop('extensions',None)
        count += 1
        if count%25 == 0: print('Converted materials',count,flush=True)
    for key in ['extensionsUsed','extensionsRequired']:
        if key in scene:
            scene[key] = [x for x in scene[key] if x != 'KHR_materials_pbrSpecularGlossiness']
    output.write_text(json.dumps(scene))
    print('Converted',count,'materials; unsupported transmission remains explicitly outside acceptance',flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source',type=Path)
    parser.add_argument('output',type=Path)
    args = parser.parse_args()
    prepare(args.source,args.output)
