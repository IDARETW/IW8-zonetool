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
const {WeaponViewer,THREE}=await import('./web/viewer.js');
const {OBJLoader}=await import('three/addons/loaders/OBJLoader.js');
const {GLTFLoader}=await import('three/addons/loaders/GLTFLoader.js');
const project=JSON.parse(readFileSync(resolve(root,'projects/26c283c28a4c/project.json')));
const clip=JSON.parse(readFileSync(resolve(root,'tunnel/native-reload-preview.json')));
const viewer=Object.create(WeaponViewer.prototype);
Object.assign(viewer,{project,view:'view_model',scene:new THREE.Scene(),model:new THREE.Group(),
  skeletonRoot:new THREE.Group(),attachments:new THREE.Group(),arms:new THREE.Group(),gizmo:{detach(){}},showArms:true});
viewer.scene.add(viewer.model,viewer.skeletonRoot,viewer.attachments,viewer.arms);
const obj=readFileSync(resolve(root,'projects',project.id,project.model),'utf8');
const object=new OBJLoader().parse(obj);viewer.model.add(object);
const matrix=project.rig.view_model.transform;
viewer.model.matrixAutoUpdate=false;viewer.model.matrix.set(...matrix[0],...matrix[1],...matrix[2],0,0,0,1);
viewer.skinModel(object,obj,project.rig.view_model);
const glb=readFileSync(resolve(root,'library/viewhands.glb'));
const hands=await new GLTFLoader().parseAsync(glb.buffer.slice(glb.byteOffset,glb.byteOffset+glb.length),'');
viewer.arms.add(hands.scene);viewer.armBones=[];viewer.armSkeletons=[];
hands.scene.traverse(o=>{if(o.isBone)viewer.armBones.push(o);if(o.isSkinnedMesh&&!viewer.armSkeletons.includes(o.skeleton))viewer.armSkeletons.push(o.skeleton);});
const cameraBone=viewer.armBones.find(b=>b.name==='tag_view'),cameraBind=cameraBone.position.clone();
const mapping=await viewer.playSource(clip);
const cameraOffset=clip.tracks.find(t=>t.bone==='tag_view').translations[0];
assert(cameraBone.position.distanceTo(cameraBind.add(new THREE.Vector3(...cameraOffset)))<1e-5,'Native translations must be relative to local bind position');
const wrist=viewer.armBones.find(b=>b.name==='j_wrist_le');
const before=wrist.getWorldPosition(new THREE.Vector3());
const first=viewer.skeletonRoot.matrix.clone();
const magazine=viewer.skeleton.bones.find(b=>b.name==='j_mag1');
const magazineBefore=magazine.position.clone();
viewer.mixer.update(1.0);viewer.updateAnimationPose();
const handDistance=wrist.getWorldPosition(new THREE.Vector3()).distanceTo(before);
assert(handDistance>0.1,'Native reload must visibly move the left hand');
assert(!first.equals(viewer.skeletonRoot.matrix),'Native reload must move the weapon');
viewer.pauseAnimation();const held=viewer.action.time;viewer.mixer.update(.25);assert.equal(viewer.action.time,held);
viewer.seekAnimation(.5);assert.equal(viewer.action.time,.5);
assert(magazine.position.distanceTo(magazineBefore)>.1,'Magazine must move independently');
assert(viewer.skeletonRoot.matrix.equals(viewer.nativeDriver.matrixWorld),'Stock root must follow native gun bone');
viewer.resetPose();assert(viewer.skeletonRoot.matrix.equals(new THREE.Matrix4()));

console.log(JSON.stringify({status:'passed',matched_bones:mapping.matched,independent_magazine_motion:true,native_gun_alignment:true,hand_motion:handDistance}));
