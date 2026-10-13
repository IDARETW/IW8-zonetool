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
const project=JSON.parse(readFileSync(resolve(root,'projects/bc64f9df6f2a/project.json')));
const clip=JSON.parse(readFileSync(resolve(root,'tunnel/native-reload-preview.json')));
const viewer=Object.create(WeaponViewer.prototype);
Object.assign(viewer,{project,view:'view_model',scene:new THREE.Scene(),model:new THREE.Group(),
  template:new THREE.Group(),skeletonRoot:new THREE.Group(),attachments:new THREE.Group(),arms:new THREE.Group(),bones:new THREE.Group(),gizmo:{detach(){},attach(){}},showArms:true,previewLoop:false});
viewer.scene.add(viewer.model,viewer.skeletonRoot,viewer.attachments,viewer.arms);
viewer.drawRig(project.rig.view_model);
const obj=readFileSync(resolve(root,'projects',project.id,project.model),'utf8');
const object=new OBJLoader().parse(obj);viewer.model.add(object);
const matrix=project.rig.view_model.transform;
viewer.model.matrixAutoUpdate=false;viewer.model.matrix.set(...matrix[0],...matrix[1],...matrix[2],0,0,0,1);
viewer.skinModel(object,obj,project.rig.view_model);
const glb=readFileSync(resolve(root,'library/viewhands.glb'));
globalThis.fetch=async()=>new Response(glb,{status:200});
await viewer.loadArms();
assert(viewer.handIK,'Both hand targets must have a live arm IK chain');
const cameraBone=viewer.armBones.find(b=>b.name==='tag_view'),cameraBind=cameraBone.position.clone();
const mapping=await viewer.playSource(clip);
const cameraOffset=clip.tracks.find(t=>t.bone==='tag_view').translations[0];
assert(cameraBone.position.distanceTo(cameraBind.add(new THREE.Vector3(...cameraOffset)))<1e-5,'Native translations must be relative to local bind position');
const weaponDriver=viewer.nativeDriver;
assert(weaponDriver?.name==='tag_weapon','Custom native previews must drive the weapon from tag_weapon');
assert(weaponDriver.getWorldPosition(new THREE.Vector3()).length()<1e-4,'The clip’s first tag_weapon pose must align to the weapon origin');
assert(viewer.skeletonRoot.matrix.equals(new THREE.Matrix4()),'The weapon must begin at its authored pose instead of applying the bind offset twice');
const wrist=viewer.armBones.find(b=>b.name==='j_wrist_le');
const before=wrist.getWorldPosition(new THREE.Vector3());
const first=viewer.skeletonRoot.matrix.clone();
let handDistance=0;const sampledWeaponPoses=new Set();
for(const time of [0,.25,.5,.75,1,1.5,2,clip.duration]){
  viewer.seekAnimation(time);
  handDistance=Math.max(handDistance,wrist.getWorldPosition(new THREE.Vector3()).distanceTo(before));
  const drivenPose=weaponDriver.matrixWorld.clone().multiply(viewer.nativeDriverBindInverse);
  assert(drivenPose.equals(viewer.skeletonRoot.matrix),`Weapon root must stay flush with the animated tag_weapon at ${time}s`);
  sampledWeaponPoses.add(viewer.skeletonRoot.matrix.elements.map(v=>v.toFixed(5)).join(','));
  if(time===.5){
    viewer.pauseAnimation();const held=viewer.action.time;viewer.mixer.update(.25);assert.equal(viewer.action.time,held);
    viewer.pauseAnimation();
  }
}
assert(handDistance>0.1,'Native reload must visibly move the left hand');
assert(sampledWeaponPoses.size>3,'Native reload must move the weapon throughout the clip');
assert(!first.equals(viewer.skeletonRoot.matrix),'Native reload must move the weapon');
viewer.seekAnimation(.5);assert.equal(viewer.action.time,.5);
const nativeArmPose=wrist.getWorldPosition(new THREE.Vector3());
viewer.setAnimationEdit(true);
viewer.setHandPlacementMode(true);
viewer.selectBone('tag_ik_loc_le',{preserveAnimation:true});
const nativeBefore=project.rig.view_model.bind_pose[project.rig.view_model.bones.indexOf('tag_ik_loc_le')].translation;
const animatedMarkerBefore=viewer.selected.position.clone();
const animatedLocalBefore=viewer.skeleton.bones.find(b=>b.name==='tag_ik_loc_le').position.clone();
viewer.setBoneComponent('position',0,animatedMarkerBefore.x+1);
const animatedLocalAfter=viewer.skeleton.bones.find(b=>b.name==='tag_ik_loc_le').position.clone();
const expectedNativeX=nativeBefore[0]+animatedLocalAfter.x-animatedLocalBefore.x;
const savedPose=viewer.boneNativePose();
assert(Math.abs(savedPose.translation[0]-expectedNativeX)<1e-4,'Paused edits must save in native bind space, not animated world space');
assert(viewer.selected.position.distanceTo(animatedMarkerBefore)>.05,'Paused numeric edits must move the live marker');
viewer.setHandPlacementMode(false);
const nativeAnchors=['le','ri'].map(side=>{
  const arm=viewer.armBones.find(b=>b.name==='j_wrist_'+side);
  return arm.getWorldPosition(new THREE.Vector3()).toArray();
});
const modelForCapture=viewer.model.children[0];viewer.model.remove(modelForCapture);
viewer.setHandPlacementMode(true);
const captured=[];viewer.onChange=async action=>captured.push(action);
assert.equal(await viewer.captureNativeHandTargets(),2);
viewer.model.add(modelForCapture);
assert.deepEqual(captured.map(action=>action.bone),['tag_ik_loc_le','tag_ik_loc_ri']);
for(let i=0;i<2;i++)assert(viewer.skeleton.bones.find(b=>b.name===captured[i].bone)
  .getWorldPosition(new THREE.Vector3()).distanceTo(new THREE.Vector3(...nativeAnchors[i]))<1e-3,
  'Native hand capture must align both weapon tags to the animated wrists');
viewer.model.remove(modelForCapture);
const distantModel=new THREE.Mesh(new THREE.BoxGeometry(1,1,1));distantModel.position.x=1000;viewer.model.add(distantModel);
await assert.rejects(viewer.captureNativeHandTargets(),/A hand is away from the custom model/);
assert.equal(viewer.handPlacementMode,true,'Rejected grip capture must restore paused hand editing');
viewer.model.remove(distantModel);viewer.model.add(modelForCapture);
viewer.onChange=null;
viewer.setAnimationEdit(false);
viewer.setHandPlacementMode(false);
const restoredDistance=wrist.getWorldPosition(new THREE.Vector3()).distanceTo(nativeArmPose);
assert(restoredDistance<.05,
  `Leaving hand placement must restore the native arm animation pose (${restoredDistance})`);
viewer.resetPose();assert(viewer.skeletonRoot.matrix.equals(new THREE.Matrix4()));
viewer.setHandPlacementMode(true);
const handTarget=viewer.skeleton.bones.find(b=>b.name==='tag_ik_loc_le');
const gripBefore=wrist.getWorldPosition(new THREE.Vector3());
handTarget.position.x+=2;handTarget.updateMatrixWorld(true);viewer.updateHandIK();
const gripAfter=wrist.getWorldPosition(new THREE.Vector3());
assert(gripAfter.distanceTo(gripBefore)>.05,'Editing the native left-hand tag must move the visible hand');
assert(Number.isFinite(gripAfter.x+gripAfter.y+gripAfter.z),'IK must keep the hand pose finite');
const rightWrist=viewer.armBones.find(b=>b.name==='j_wrist_ri');
const rightTarget=viewer.skeleton.bones.find(b=>b.name==='tag_ik_loc_ri');
const rightBefore=rightWrist.getWorldPosition(new THREE.Vector3());
rightTarget.position.y+=2;rightTarget.updateMatrixWorld(true);viewer.updateHandIK();
const rightAfter=rightWrist.getWorldPosition(new THREE.Vector3());
assert(rightAfter.distanceTo(rightBefore)>.05,'Editing the native right-hand tag must move the visible hand');

const source=readFileSync(new URL('./web/app.js',import.meta.url),'utf8');
const callback=source.split("const viewer=new WeaponViewer($('#viewport'),")[1].split(',(')[0];
let saves=0;
const state={project};
viewer.onChange=vm.runInNewContext('('+callback+')',{viewer,state,async mutation(action){
  saves++;state.project=structuredClone(state.project);state.project.rig.view_model.transform=action.matrix;
}});
viewer.load=()=>{throw new Error('Transform save must not reload geometry');};
viewer.selectModel=()=>{};
viewer.selected=viewer.model;viewer.model.matrix.decompose(viewer.model.position,viewer.model.quaternion,viewer.model.scale);viewer.model.matrixAutoUpdate=true;
const identity=viewer.model.children[0];
const initialX=viewer.model.position.x;
for(let i=0;i<3;i++){viewer.model.position.x+=1;await viewer.commit();}
assert.equal(saves,3);assert.equal(viewer.selected,viewer.model);assert.equal(viewer.model.children[0],identity);
assert.equal(state.project.rig.view_model.transform[0][3],initialX+3);
const armLoader=Object.create(WeaponViewer.prototype);armLoader.arms=new THREE.Group();
let armLoads=0,finishArms;
armLoader.loadArms=async()=>{armLoads++;await new Promise(resolve=>{finishArms=resolve;});armLoader.arms.add(new THREE.Group());};
const visibleLoad=armLoader.setArms(true),playbackLoad=armLoader.setArms(true,true);
finishArms();await Promise.all([visibleLoad,playbackLoad]);
assert.equal(armLoads,1);assert.equal(armLoader.arms.children.length,1);
// Keep the visible assembly during slow, failed, and superseded resource loads.
delete viewer.load;delete viewer.selectModel;viewer.selected=null;viewer.bones=new THREE.Group();
viewer.grid=new THREE.Group();viewer.ground=new THREE.Group();
const stats={textContent:'Existing geometry'},empty={hidden:true};
globalThis.document={querySelector:s=>s==='#geometry-stats'?stats:empty};
const pending=[];globalThis.fetch=url=>new Promise((resolve,reject)=>pending.push({url,resolve,reject}));
const loadProject={...project,rig:null,material:{},owned_assets:[],model:'first.obj'};
const oldModel=viewer.model;
const slow=viewer.load(loadProject);assert.equal(viewer.model,oldModel);assert(viewer.model.children.length);
pending.shift().resolve(new Response('v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n'));await slow;
assert.notEqual(viewer.model,oldModel);assert.equal(viewer.model.children.length,1);
const retained=viewer.model;
const failure=viewer.load({...loadProject,model:'missing.obj'});pending.shift().resolve(new Response('',{status:404}));
await assert.rejects(failure,/Could not load/);assert.equal(viewer.model,retained);
const stale=viewer.load({...loadProject,model:'stale.obj'});
const newest=viewer.load({...loadProject,model:'newest.obj'});
pending[1].resolve(new Response('v 0 0 0\nv 2 0 0\nv 0 2 0\nf 1 2 3\n'));await newest;const newestModel=viewer.model;
pending[0].resolve(new Response('v 0 0 0\nv 3 0 0\nv 0 3 0\nf 1 2 3\n'));await stale;
assert.equal(viewer.model,newestModel);assert.equal(viewer.project.model,'newest.obj');
pending.length=0;await viewer.load({...loadProject,model:'newest.obj'});assert.equal(pending.length,0);
console.log(JSON.stringify({status:'passed',matched_bones:mapping.matched,hand_motion:handDistance,
  weapon_motion:true,relative_translation:true,pause_seek_reset:true,successive_transform_saves:saves,geometry_retained:true,concurrent_arm_loads:armLoads,
  native_wrist_capture:true,off_model_capture_rejected:true,retained_during_slow_or_failed_load:true,
  stale_load_discarded:true,resource_cache:true}));
