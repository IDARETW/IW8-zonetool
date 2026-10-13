// Exercise the actual startup code with cookies omitted and storage throwing SecurityError.
// Run: node check_access.mjs
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import vm from 'node:vm';

const startup=readFileSync(new URL('./web/app.js',import.meta.url),'utf8')
  .replace(/^import .*;\r?\n/,'').split('const $=s=>')[0];
const key='0123456789abcdef'.repeat(4);
for(const mode of ['protected-path','fragment','query']){
  let destination='',requests=0;
  const location=new URL('https://editor.example/'+(mode==='fragment'?'#access='+key:mode==='query'?'?token='+key:'access/'+key));
  const brand={};
  const context={URL,URLSearchParams,Headers,Request,DOMException,location,
    document:{querySelector(selector){
      if(selector==='meta[name="replay-access"]')return mode==='fragment'?null:{content:key};
      if(selector==='.brand')return brand;
      throw new Error('Unexpected element: '+selector);
    }},
    history:{replaceState(_state,_title,path){destination=path;}},
    async fetch(input,init){
      requests++;
      assert.equal(input,'/api/bootstrap');
      assert.equal(new Headers(init.headers).get('X-Replay-Access'),key);
      return {ok:true,status:200,json:async()=>({projects:[{id:'test'}]})};
    }
  };
  for(const name of ['sessionStorage','localStorage'])Object.defineProperty(context,name,{
    get(){throw new DOMException('Storage disabled','SecurityError');}
  });
  context.window=context;
  const result=await vm.runInNewContext('(async()=>{'+startup+'; return {initialBootstrap,preferences};})()',context);
  assert.equal(result.initialBootstrap.projects[0].id,'test');
  assert.equal(requests,1);
  assert.equal(destination,'/access/'+key);
  assert.equal(brand.href,location.pathname);
  assert.equal(result.preferences.getItem('test'),null);
  result.preferences.setItem('test','1');
  result.preferences.removeItem('test');
  console.log(mode+': authenticated with storage disabled; reloadable path retained');
}
