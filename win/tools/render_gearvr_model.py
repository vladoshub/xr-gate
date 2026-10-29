#!/usr/bin/env python3
"""Render the MIT WebXR Input Profiles Samsung Gear VR GLB to a static guide image.

Development-only dependencies: numpy, Pillow. No network or 3D viewer is needed
by the launcher or its offline documentation. Supply the model as a local file.
"""
import argparse
import io
import json
import struct
from pathlib import Path

import numpy as np
from PIL import Image


def unit(v):
    return v/np.linalg.norm(v)


def render(source, destination, side=False):
    raw=source.read_bytes()
    if raw[:4]!=b'glTF':raise ValueError('Expected binary glTF')
    size,kind=struct.unpack_from('<II',raw,12)
    model=json.loads(raw[20:20+size]);binary=raw[28+size:]
    def accessor(index):
        a=model['accessors'][index];v=model['bufferViews'][a['bufferView']]
        count={'SCALAR':1,'VEC2':2,'VEC3':3,'VEC4':4}[a['type']]
        dtype=np.dtype({5123:'<u2',5125:'<u4',5126:'<f4'}[a['componentType']])
        return np.ndarray((a['count'],count),dtype=dtype,buffer=binary,
            offset=v.get('byteOffset',0)+a.get('byteOffset',0),
            strides=(v.get('byteStride',count*dtype.itemsize),dtype.itemsize)).copy()
    view=model['bufferViews'][model['images'][0]['bufferView']]
    texture=np.asarray(Image.open(io.BytesIO(binary[view.get('byteOffset',0):view.get('byteOffset',0)+view['byteLength']])).convert('RGB'))/255.
    parts=[]
    def visit(index,parent):
        node=model['nodes'][index];local=np.eye(4)
        if 'matrix' in node:local=np.array(node['matrix']).reshape(4,4).T
        else:
            x,y,z,w=node.get('rotation',[0,0,0,1])
            local[:3,:3]=np.array([[1-2*y*y-2*z*z,2*x*y-2*z*w,2*x*z+2*y*w],
                [2*x*y+2*z*w,1-2*x*x-2*z*z,2*y*z-2*x*w],
                [2*x*z-2*y*w,2*y*z+2*x*w,1-2*x*x-2*y*y]]) @ np.diag(node.get('scale',[1,1,1]))
            local[:3,3]=node.get('translation',[0,0,0])
        world=parent@local
        if 'mesh' in node:
            for p in model['meshes'][node['mesh']]['primitives']:
                attr=p['attributes'];vertices=accessor(attr['POSITION']);normals=accessor(attr['NORMAL'])
                vertices=vertices@world[:3,:3].T+world[:3,3]
                normals=normals@np.linalg.inv(world[:3,:3])
                parts.append((vertices,normals,accessor(attr['TEXCOORD_0']),accessor(p['indices']).reshape(-1,3)))
        for child in node.get('children',[]):visit(child,world)
    visit(next(i for i,n in enumerate(model['nodes']) if n.get('name')=='samsung_gearvr_none'),np.eye(4))
    toward=unit(np.array([1.8,.15,-.1] if side else [.65,1.5,.3]));up=unit(np.array([0.,0.,-1.])-toward*np.dot(toward,[0,0,-1]))
    right=np.cross(up,toward);basis=np.stack([right,up,toward])
    points=np.concatenate([p[0]@basis.T for p in parts]);low,high=points.min(0),points.max(0)
    width,height,scale=600,760,2
    w,h=width*scale,height*scale
    zoom=min((w-80*scale)/(high[0]-low[0]),(h-70*scale)/(high[1]-low[1]))
    center=(low+high)/2
    pixels=np.zeros((h,w,4),dtype=np.uint8);depth=np.full((h,w),-np.inf)
    lights=[unit(np.array(v)) for v in ([.1,1,-.7],[-1,1,.4],[1,.3,.4])]
    for vertices,normals,uv,triangles in parts:
        v=vertices@basis.T
        screen=np.stack([(v[:,0]-center[0])*zoom+w/2,-(v[:,1]-center[1])*zoom+h/2,v[:,2]],axis=1)
        for tri in triangles:
            a,b,c=screen[tri]
            x0,y0=np.maximum(np.floor(np.min(screen[tri,:2],axis=0)).astype(int),0)
            x1,y1=np.minimum(np.ceil(np.max(screen[tri,:2],axis=0)).astype(int),[w-1,h-1])
            if x0>x1 or y0>y1:continue
            xx,yy=np.meshgrid(np.arange(x0,x1+1)+.5,np.arange(y0,y1+1)+.5)
            den=(b[1]-c[1])*(a[0]-c[0])+(c[0]-b[0])*(a[1]-c[1])
            if abs(den)<1e-8:continue
            wa=((b[1]-c[1])*(xx-c[0])+(c[0]-b[0])*(yy-c[1]))/den
            wb=((c[1]-a[1])*(xx-c[0])+(a[0]-c[0])*(yy-c[1]))/den;wc=1-wa-wb
            zz=wa*a[2]+wb*b[2]+wc*c[2];region=depth[y0:y1+1,x0:x1+1]
            mask=(wa>=-1e-6)&(wb>=-1e-6)&(wc>=-1e-6)&(zz>region)
            if not mask.any():continue
            weights=np.stack([wa[mask],wb[mask],wc[mask]],axis=1)
            coords=weights@uv[tri];coords=np.clip(coords,0,1)
            tex=texture[np.minimum((coords[:,1]*texture.shape[0]).astype(int),texture.shape[0]-1),
                        np.minimum((coords[:,0]*texture.shape[1]).astype(int),texture.shape[1]-1)]
            normal=weights@normals[tri];normal/=np.maximum(np.linalg.norm(normal,axis=1,keepdims=True),1e-10)
            diffuse=.75+.7*np.maximum(normal@lights[0],0)+.2*np.maximum(normal@lights[1],0)
            halfway=unit(lights[0]+toward);shine=.18*np.maximum(normal@halfway,0)**35
            color=np.clip(tex*diffuse[:,None]+shine[:,None],0,1)
            target=pixels[y0:y1+1,x0:x1+1];target[mask,:3]=(color*255).astype(np.uint8);target[mask,3]=255;region[mask]=zz[mask]
    destination.parent.mkdir(parents=True,exist_ok=True)
    Image.fromarray(pixels).resize((width,height),Image.Resampling.LANCZOS).save(destination)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('model',type=Path);parser.add_argument('output',type=Path)
    parser.add_argument('--side',action='store_true')
    args=parser.parse_args();render(args.model,args.output,args.side)
