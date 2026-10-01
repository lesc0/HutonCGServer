'use client';
import {useState,useEffect} from 'react';
import {ctlStatus,ctlCommand,ctlSwitch,listProjectFiles,CtlStatus,ProjectFile} from './files';
// 실행 중인 cg-streamer 를 파일 서버의 /ctl 중계를 통해 제어한다 (next/prev/goto/play/stop/switch/...).
export function useStreamControl(onError: (s: string) => void) {
 const [status,setStatus]=useState<CtlStatus|null>(null),[online,setOnline]=useState(false),[busy,setBusy]=useState(false),[gotoPage,setGotoPage]=useState(1);
 const [projects,setProjects]=useState<ProjectFile[]>([]),[selected,setSelected]=useState('');
 useEffect(()=>{let stop=false;const poll=async()=>{if(document.hidden)return;try{const s=await ctlStatus();if(!stop){setStatus(s);setOnline(true)}}catch{if(!stop){setOnline(false);setStatus(null)}}};poll();const id=setInterval(poll,1500);return()=>{stop=true;clearInterval(id)}},[]);
 const refreshProjects=async()=>{try{const d=await listProjectFiles();setProjects(d.projects);if(!selected&&d.projects.length)setSelected(d.projects[0].name)}catch(e){onError((e as Error).message)}};
 // 처음 뜰 때 파일 서버가 아직 재시작 중이면 조용히 실패할 수 있어서, 목록이 빌 때까지 자동 재시도한다("Refresh list"은 수동 재시도용).
 useEffect(()=>{if(projects.length)return;let stop=false;const tryLoad=async()=>{if(document.hidden)return;try{const d=await listProjectFiles();if(!stop&&d.projects.length){setProjects(d.projects);setSelected(s=>s||d.projects[0].name)}}catch{/* 조용히 재시도 */}};void tryLoad();const id=setInterval(tryLoad,3000);return()=>{stop=true;clearInterval(id)}},[projects.length]);
 // busy 는 React 18+ 에서 언마운트 후 setState 해도 안전(조용히 무시)하므로 별도 mounted 가드를 두지 않는다
 // (예전의 mounted ref 가드는 Fast Refresh/StrictMode 이중 실행에서 false 로 고정돼버려 그 다음부터 모든 버튼이 영구히 disable 되는 버그가 있었음).
 const send=async(cmd:string,arg?:number|string)=>{setBusy(true);try{await ctlCommand(cmd,arg)}catch(e){onError((e as Error).message)}finally{setBusy(false)}};
 // --autoplay 로 띄운 엔진은 reload/전환 후 바로 재생되고, 아니면(수동 제어) 1페이지 정지 상태로 남는다.
 const reload=async()=>{setBusy(true);try{await ctlCommand('reload');onError('cg-streamer reloaded the saved project (plays immediately with --autoplay, otherwise stops at page 1)')}catch(e){onError((e as Error).message)}finally{setBusy(false)}};
 const switchProject=async()=>{if(!selected)return;setBusy(true);try{await ctlSwitch(selected);onError('Switched to "'+selected+'" (plays immediately with --autoplay, otherwise stops at page 1)')}catch(e){onError((e as Error).message)}finally{setBusy(false)}};
 const panel=<div className="streamcontrol">
  <div className={'stream-status '+(online?'online':'offline')}>{!online?'Cannot connect to cg-streamer (check that it is running)':!status?.ready?'Engine idle (no project)':
   <>{status.project} · Page {status.page}/{status.pages} · {status.playing?'Playing':'Stopped'} · {(status.time||0).toFixed(1)}s</>}</div>
  <button className="primary" disabled={busy||!online} onClick={reload}>Reload saved project</button>
  <div className="stream-switch">
   <span className="stream-label">Output Project:</span>
   <select aria-label="Project to switch to" disabled={busy||!projects.length} value={selected} onChange={e=>setSelected(e.target.value)}>
    {!projects.length&&<option value="">No projects saved in bin/project</option>}
    {projects.map(p=><option key={p.name} value={p.name}>{p.name}</option>)}
   </select>
   <button disabled={busy||!selected||!online} onClick={switchProject}>Switch project</button>
   <button disabled={busy} onClick={refreshProjects}>Refresh list</button>
  </div>
  <div className="playcontrols">
   <button disabled={busy} onClick={()=>send('play')}><span>▶</span>Play</button>
   <button disabled={busy} onClick={()=>send('pause')}>Ⅱ Pause</button>
   <button disabled={busy} onClick={()=>send('stop')}>■ Stop</button>
   <button disabled={busy} onClick={()=>send('cut')}>Cut</button>
   <button disabled={busy} onClick={()=>send('prev')}>◀ Prev</button>
   <button disabled={busy} onClick={()=>send('next')}>Next ▶</button>
   <button disabled={busy} onClick={()=>send('skip')}>Skip</button>
   <button disabled={busy} onClick={()=>send('clear')}>Clear</button>
  </div>
  <label>Go to page<input type="number" min={1} max={status?.pages||9999} value={gotoPage} onChange={e=>setGotoPage(Math.max(1,Math.round(+e.target.value)))}/><button disabled={busy} onClick={()=>send('goto',gotoPage)}>Go</button></label>
  <button className="danger" disabled={busy} onClick={()=>{if(window.confirm('Quit the streaming engine (cg-streamer)?'))send('quit')}}>Quit engine</button>
  <p className="muted">Start cg-streamer first with a project saved in bin/project (e.g. ./cg-streamer --run --project=... --udp=host:port). This panel only controls an engine that is already running. After editing, click "Save project", then "Reload saved project" above for the change to reach the output (it is not applied automatically). To replace the whole project, pick one above and click "Switch project".</p>
 </div>;
 return {panel,online,status};
}
