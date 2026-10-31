"""Execute the embedded Wi-Fi portal JavaScript against a deterministic fake DOM."""

import ast
import json
from pathlib import Path
import re
import shutil
import subprocess
import unittest


ROOT = Path(__file__).resolve().parents[1]
PAGE_HEADER = ROOT / "main/wifi_portal_page.h"


def production_script():
    source = PAGE_HEADER.read_text()
    match = re.search(r"static const char wifi_portal_page\[\]\s*=\s*(.*?)\s*;\s*#endif", source, re.S)
    if not match:
        raise AssertionError("Could not extract wifi_portal_page C string")
    literals = re.findall(r'"(?:\\.|[^"\\])*"', match.group(1))
    html = "".join(ast.literal_eval(item) for item in literals)
    scripts = re.findall(r"<script>(.*?)</script>", html, re.S)
    if len(scripts) != 1:
        raise AssertionError(f"Expected one inline script, found {len(scripts)}")
    return scripts[0]


NODE_HARNESS = r"""
const vm = require('node:vm');
const assert = require('node:assert/strict');
const input = require('node:fs').readFileSync(0, 'utf8');
const {script, scenario} = JSON.parse(input);

class Element {
  constructor(id) {
    this.id=id; this.value=''; this.textContent=''; this.disabled=false;
    this.type=''; this.style={}; this.children=[]; this.options=this.children;
  }
  replaceChildren(...items) { this.children.length=0; this.children.push(...items); }
  append(item) { this.children.push(item); }
}
function response(status, data, text='') {
  return {status, ok:status>=200&&status<300,
    async json(){return data}, async text(){return text || (data===null ? '' : JSON.stringify(data))}};
}
function environment(fetchHandler) {
  const elements = new Map();
  for (const id of ['net','message','connect','manualBox','manual','password','show',
                    'refresh','heading','check-result']) elements.set(id,new Element(id));
  elements.get('manualBox').style.display='block';
  elements.get('manual').value='  Office Wi-Fi  ';
  elements.get('password').value=' pass word+& ';
  const timers=[]; const calls=[];
  const document={getElementById(id){if(!elements.has(id))elements.set(id,new Element(id));return elements.get(id)},
    createElement(){return new Element('option')}};
  const context={
    document, location:{pathname:'/w/'+('a'.repeat(32))}, URLSearchParams,
    AbortController, sessionStorage:{data:new Map(),getItem(k){return this.data.get(k)||null},
      setItem(k,v){this.data.set(k,String(v))},removeItem(k){this.data.delete(k)}},
    setTimeout(fn,ms){const t={fn,ms,cleared:false};timers.push(t);return t},
    clearTimeout(t){if(t)t.cleared=true},
    fetch(url,options={}){const call={url:String(url),options,body:options.body?String(options.body):''};calls.push(call);return fetchHandler(call,calls.length-1)},
    console
  };
  vm.runInNewContext(script,context,{filename:'wifi_portal_page.js'});
  const el=id=>elements.get(id);
  const flush=async()=>{for(let i=0;i<12;i++)await Promise.resolve()};
  const fire=async ms=>{const t=timers.find(x=>!x.cleared&&x.ms===ms);assert.ok(t,`expected timer ${ms}ms; timers=${timers.map(x=>x.ms)}`);t.cleared=true;t.fn();await flush()};
  return {el,calls,timers,flush,fire,context};
}
const idle={attempt:4,state:'idle',message:'',device:'hello-box-8556'};
const queued=(attempt,state='testing')=>({attempt,state,message:'Testing router connection…',device:'hello-box-8556'});
async function scenarioRun(){
  if(scenario==='resume'){
    let statuses=0;
    const env=environment(async call=>{
      assert.match(call.url,/\/status$/); statuses++;
      return response(200,statuses===1?queued(8):{attempt:8,state:'failed',message:'Connection failed. Try again.',device:'hello-box-8556'});
    }); await env.flush();
    assert.equal(env.el('connect').disabled,true,'active test must resume as busy');
    assert.match(env.el('message').textContent,/Testing/);
    assert.equal(env.calls.filter(c=>c.options.method==='POST').length,0);
    assert.ok(env.calls.some(c=>c.url.endsWith('/status')));
    assert.equal(env.calls.some(c=>c.url.endsWith('/scan')),false,'do not scan while a trial is active');
    await env.fire(2000);
    assert.equal(statuses,2,'reload resumes polling the same attempt');
    assert.equal(env.el('connect').disabled,false,'terminal status releases the resumed gate');
    return;
  }
  if(scenario==='exact-retry'){
    let posts=0, statuses=0;
    const env=environment(async call=>{
      if(call.options.method==='POST'){posts++; if(posts===1)return response(400,null,'Bad credentials'); return response(202,{attempt:5});}
      if(call.url.endsWith('/status')){statuses++;return response(200,statuses===1?idle:{attempt:4,state:'failed',message:'Previous test failed',device:'hello-box-8556'});}
      if(call.url.endsWith('/scan'))return response(200,{state:'ready',networks:[]});
      return response(404,null,'missing');
    }); await env.flush();
    await env.el('connect').onclick(); await env.flush();
    assert.equal(env.el('connect').disabled,false,'definite 400 rejection should be retryable');
    await env.el('connect').onclick(); await env.flush();
    assert.equal(posts,2,'each explicit user click makes at most one POST');
    assert.equal(env.calls.filter(c=>c.options.method==='POST').length,2);
    assert.match(env.calls.find(c=>c.options.method==='POST').body,/ssid=\+\+Office\+Wi-Fi\+\+/);
    assert.match(env.calls.find(c=>c.options.method==='POST').body,/password=\+pass\+word%2B%26\+/);
    assert.equal(env.el('connect').disabled,true,'accepted test remains busy while polling');
    return;
  }
  if(scenario==='status-recovery'){
    let posts=0,statuses=0;
    const env=environment(async call=>{
      if(call.options.method==='POST'){posts++;return response(202,{attempt:12});}
      if(call.url.endsWith('/status')){statuses++; if(statuses===1)return response(200,idle); if(statuses===2)return response(200,idle); if(statuses===3)throw Error('AP roam'); return response(200,{attempt:12,state:'failed',message:'Connection failed. Try again.',device:'hello-box-8556'});}
      if(call.url.endsWith('/scan'))return response(200,{state:'ready',networks:[]});
      return response(404,null,'missing');
    }); await env.flush();
    await env.el('connect').onclick(); await env.flush();
    assert.equal(posts,1); assert.equal(env.el('connect').disabled,true);
    await env.fire(1000);
    assert.equal(posts,1,'status transport failure must not create a second POST');
    assert.equal(env.el('connect').textContent,'Check result','keep attempt gate while disconnected');
    assert.match(env.el('message').textContent,/rejoin|check|connection/i);
    assert.equal(env.el('connect').textContent,'Check result');
    await env.fire(3000);
    assert.equal(posts,1); assert.equal(env.el('connect').disabled,false,'same attempt terminal failure releases gate');
    assert.match(env.el('message').textContent,/Connection failed/);
    return;
  }
  if(scenario==='unknown-post'){
    let statusCount=0,postCount=0,aborted=false;
    const env=environment((call)=>{
      if(call.options.method==='POST'){
        postCount++;
        return Promise.resolve({status:202,ok:true,text(){return new Promise((resolve,reject)=>call.options.signal.addEventListener('abort',()=>{aborted=true;reject(new Error('AbortError'))},{once:true}))}});
      }
      if(call.url.endsWith('/status')){
        statusCount++;
        if(statusCount<=2)return Promise.resolve(response(200,idle));
        if(statusCount===3)return Promise.resolve(response(200,{...idle,state:'failed',message:'Old attempt failed'}));
        return Promise.resolve(response(200,queued(5,'connected')));
      }
      if(call.url.endsWith('/scan'))return Promise.resolve(response(200,{state:'ready',networks:[]}));
      return Promise.resolve(response(404,null,'missing'));
    }); await env.flush();
    await env.el('connect').onclick(); await env.flush();
    assert.equal(postCount,1);
    assert.ok(env.timers.some(t=>t.ms===5000),'POST should have a bounded deadline');
    await env.fire(5000);
    assert.equal(aborted,true); assert.equal(env.el('connect').textContent,'Check result','unknown acceptance after a stuck response body offers status recovery, not a new POST');
    await env.el('connect').onclick(); await env.flush();
    assert.equal(postCount,1,'Check result must never POST again');
    await env.fire(3000);
    assert.equal(postCount,1,'old terminal baseline must not be treated as the new attempt');
    assert.equal(env.el('connect').textContent,'Check result');
    assert.match(env.el('message').textContent,/not confirmed|rejoin|check/i);
    await env.fire(3000);
    assert.equal(postCount,1); assert.match(env.el('message').textContent,/connected|complete/i);
    return;
  }
  if(scenario==='post-server-error'){
    let statusCount=0,posts=0;
    const env=environment(async call=>{
      if(call.options.method==='POST'){
        posts++;
        return response(500,null,'Internal server error'); // Backend may already have queued this attempt.
      }
      if(call.url.endsWith('/status')){
        statusCount++;
        if(statusCount<=2)return response(200,idle);
        if(statusCount===3)return response(200,{attempt:4,state:'failed',message:'Old attempt failed',device:'hello-box-8556'});
        if(statusCount===4)return response(200,queued(5,'testing'));
        return response(200,{attempt:5,state:'failed',message:'Connection failed. Try again.',device:'hello-box-8556'});
      }
      if(call.url.endsWith('/scan'))return response(200,{state:'ready',networks:[]});
      return response(404,null,'missing');
    }); await env.flush();
    await env.el('connect').onclick(); await env.flush();
    assert.equal(posts,1);
    assert.equal(env.el('connect').textContent,'Check result','500 is uncertain, not a retryable rejection');
    await env.el('connect').onclick(); await env.flush();
    assert.equal(posts,1,'checking an uncertain 500 must not submit another POST');
    assert.equal(env.el('connect').textContent,'Check result','old terminal baseline cannot prove acceptance');
    await env.fire(3000);
    assert.equal(posts,1);
    assert.equal(env.el('connect').disabled,true,'new queued attempt resumes the active gate');
    assert.match(env.el('message').textContent,/Testing/);
    await env.fire(2000);
    assert.equal(posts,1);
    assert.equal(env.el('connect').disabled,false,'only terminal status for the accepted attempt ends the gate');
    assert.match(env.el('message').textContent,/Connection failed/);
    return;
  }
  if(scenario==='body-timeout'){
    let aborted=false;
    const env=environment(call=>{
      if(call.url.endsWith('/status'))return Promise.resolve({status:200,ok:true,text(){return new Promise((resolve,reject)=>call.options.signal.addEventListener('abort',()=>{aborted=true;reject(new Error('AbortError'))},{once:true}))}});
      throw Error('scan should wait until baseline status succeeds');
    }); await env.flush();
    assert.ok(env.timers.some(t=>t.ms===5000),'body read must share bounded request timeout');
    await env.fire(5000);
    assert.equal(aborted,true); assert.match(env.el('message').textContent,/rejoin|check|connection/i);
    assert.equal(env.el('connect').textContent,'Check result','unknown baseline must not permit a new POST');
    await env.el('connect').onclick(); await env.flush();
    assert.equal(env.calls.filter(c=>c.options.method==='POST').length,0,'unknown startup status can only be checked, never submitted');
    return;
  }
  if(scenario==='scan-timeout'){
    let statusDone=false,aborted=false;
    const env=environment(call=>{
      if(call.url.endsWith('/status')){statusDone=true;return Promise.resolve(response(200,idle));}
      if(call.url.includes('/scan'))return new Promise((resolve,reject)=>call.options.signal.addEventListener('abort',()=>{aborted=true;reject(new Error('AbortError'))},{once:true}));
      throw Error('unexpected request '+call.url);
    }); await env.flush();
    assert.equal(statusDone,true);
    assert.ok(env.timers.some(t=>t.ms===5000),'scan fetch must have a bounded deadline');
    await env.fire(5000);
    assert.equal(aborted,true); assert.match(env.el('message').textContent,/scan|network|manual/i);
    assert.equal(env.el('connect').disabled,false,'scan timeout leaves manual entry usable');
    return;
  }
  throw new Error('unknown scenario '+scenario);
}
scenarioRun().then(()=>process.stdout.write('ok\n')).catch(e=>{console.error(e.stack||e);process.exitCode=1});
"""


@unittest.skipUnless(shutil.which("node"), "Node.js is required to execute the production portal script")
class WifiPortalPageBehaviorTests(unittest.TestCase):
    def run_script(self, scenario):
        completed = subprocess.run(
            [shutil.which("node"), "-e", NODE_HARNESS],
            input=json.dumps({"script": production_script(), "scenario": scenario}),
            text=True,
            capture_output=True,
            timeout=10,
            check=False,
        )
        self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)

    def test_load_resumes_existing_attempt_without_submitting_or_scanning(self):
        self.run_script("resume")

    def test_known_rejection_allows_explicit_retry_and_preserves_input_bytes(self):
        self.run_script("exact-retry")

    def test_status_transport_failure_keeps_attempt_gate_and_recovers_same_attempt(self):
        self.run_script("status-recovery")

    def test_post_timeout_recovers_new_attempt_without_reusing_old_terminal_result(self):
        self.run_script("unknown-post")

    def test_post_500_after_acceptance_recovers_only_new_attempt_terminal_state(self):
        self.run_script("post-server-error")

    def test_timeout_covers_status_body_read_and_keeps_unknown_state_locked(self):
        self.run_script("body-timeout")

    def test_scan_timeout_aborts_and_leaves_manual_connect_available(self):
        self.run_script("scan-timeout")


if __name__ == "__main__":
    unittest.main()
