// Real Three.js skeletal playback and repeated transform-save regression.
// node check_preview.mjs <ReplayWeaponEditor directory>
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {pathToFileURL} from 'node:url';
import {resolve} from 'node:path';
import {registerHooks} from 'node:module';
import vm from 'node:vm';

const root=resolve(process.argv[2]);
registerHooks({resolve(specifier,context,next){
  if(specifier==='three')return {url:pathToFileURL(resolve(root,'vendor/three/build/three.module.js')).href,shortCircuit:true};
  if(specifier.startsWith('three/addons/'))return {url:pathToFileURL(resolve(root,'vendor/three/examples/jsm',specifier.slice(13))).href,shortCircuit:true};
  return next(specifier,context);
}});

const {importModel,WeaponViewer}=await import('./web/viewer.js');
const path=process.argv[3];
const file=new File([readFileSync(path)],'scorned-revolver.glb');
const before=performance.now();const result=await importModel(file,[file]);
const objBytes=Buffer.byteLength(result.obj);let vertices=0,faces=0;
for(const line of result.obj.split('\n')){if(line.startsWith('v '))vertices++;else if(line.startsWith('f '))faces++;}
assert.equal(vertices,1008274);assert.equal(faces,1954539);
console.log(JSON.stringify({vertices,triangles:faces,objBytes,sourceBytes:file.size,seconds:(performance.now()-before)/1000,parts:result.parts}));

const viewport=Object.create(WeaponViewer.prototype);viewport.readAsset=async()=>file.arrayBuffer();
const preview=await viewport.loadGeometry('test','converted.obj','original.glb');
let previewVertices=0,previewTriangles=0;preview.object.traverse(mesh=>{if(mesh.isMesh){previewVertices+=mesh.geometry.attributes.position.count;previewTriangles+=(mesh.geometry.index?.count||mesh.geometry.attributes.position.count)/3;}});
assert.equal(previewVertices,vertices);assert.equal(previewTriangles,faces);assert.equal(preview.sourceVertices,true);
console.log(JSON.stringify({indexed_preview:true,previewVertices,previewTriangles}));
