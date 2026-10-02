import type Konva from 'konva';
import {Item,Page,W,H,effectState} from './model';
export type Visual={opacity:number;x:number;y:number;scaleX:number;scaleY:number;rotation:number;clip?: (c:Konva.Context)=>void;fold?:{x:number;width:number;height:number}};
export function visual(i:Item,time:number,mode:Page['mode'],duration:number):Visual{const state=effectState(i,time,mode,duration),q=state.progress,f=state.effect,d=i.direction,preset=i.effectPreset;const v:Visual={opacity:1,x:0,y:0,scaleX:1,scaleY:1,rotation:0};if(f==='fade')v.opacity=q;if(f==='blink')v.opacity=Math.floor(state.elapsed*Math.max(1,i.blinkCount)*2/Math.max(1,i.duration))%2?0:1;if(f==='move'){v.x=(d==='left'?-W:d==='right'?W:0)*(1-q);v.y=(d==='up'?-H:d==='down'?H:0)*(1-q);if(preset>=4){v.x=(preset%2?-W:W)*(1-q);v.y=(preset%4<2?-H:H)*(1-q)}}if(f==='scale'){v.scaleX=preset===5?1:q;v.scaleY=preset===4?1:q;v.x=d==='right'?i.w*(1-v.scaleX):d==='left'?0:i.w*(1-v.scaleX)/2;v.y=d==='down'?i.h*(1-v.scaleY):d==='up'?0:i.h*(1-v.scaleY)/2}if(f==='crawl')v.x=(d==='right'?1:-1)*q*(W+i.w)*i.speed;if(f==='roll')v.y=(d==='down'?1:-1)*q*(H+i.h)*i.speed;
if(f==='wipe'||f==='banner'||f==='curl'){v.clip=c=>{c.beginPath();if(preset%8===4)c.rect(i.w*(1-q)/2,0,i.w*q,i.h);else if(preset%8===5)c.rect(0,i.h*(1-q)/2,i.w,i.h*q);else if(preset%8===6){c.moveTo(0,0);c.lineTo(i.w*q*2,0);c.lineTo(0,i.h*q*2)}else if(preset%8===7){c.moveTo(i.w,0);c.lineTo(i.w-i.w*q*2,0);c.lineTo(i.w,i.h*q*2)}else if(d==='up'||d==='down')c.rect(0,d==='up'?i.h*(1-q):0,i.w,i.h*q);else c.rect(d==='left'?i.w*(1-q):0,0,i.w*q,i.h);c.closePath()};if(f==='curl'){v.rotation=i.effectAngle*(1-q);if(q>0&&q<1)v.fold={x:i.w*q,width:Math.min(i.curlRadius*2,i.w*q,i.w*(1-q)),height:Math.min(i.h,i.curlRadius*2)}}if(f==='banner'){v.y=(1-q)*i.h*(d==='up'?-1:1);v.scaleY=Math.max(.01,q);v.rotation=Math.sin(q*Math.PI)*(preset%2?8:-8)}}
if((f==='wipe'||f==='organic')&&i.softness>0&&q<1)v.opacity=Math.min(1,q*4);
if(f==='organic'){v.clip=c=>{c.beginPath();const nx=16,ny=12;for(let a=0;a<nx;a++)for(let b=0;b<ny;b++){const seed=((a*73856093)^(b*19349663)^((preset+1)*83492791))>>>0;const t=(seed%1000)/1000;const k=Math.max(0,Math.min(1,(q-t)*4));if(k>0){const w=i.w/nx,h=i.h/ny;if(preset%3===0)c.rect(a*w,b*h,w*k,h*k);else if(preset%3===1)c.rect(a*w+(1-k)*w/2,b*h,w*k,h);else{c.moveTo(a*w+w/2+w*k,b*h+h/2);c.arc(a*w+w/2,b*h+h/2,Math.hypot(w,h)*k/2,0,Math.PI*2)}}}if(q===1)c.rect(0,0,i.w,i.h);c.closePath()}}
if(f==='tile'){v.clip=c=>{c.beginPath();const nx=Math.min(32,Math.max(1,i.tileX)),ny=Math.min(32,Math.max(1,i.tileY));for(let a=0;a<nx;a++)for(let b=0;b<ny;b++){const offset=preset%3===0?0:preset%3===1?(a+b)/(nx+ny)*.7:(((a*17+b*31)%97)/97)*.7;const k=Math.max(0,Math.min(1,(q-offset)/(1-offset)));c.rect(a*i.w/nx+(1-k)*i.w/nx/2,b*i.h/ny+(1-k)*i.h/ny/2,i.w*k/nx,i.h*k/ny)}c.closePath()}}
return v}

// ---- Effects 탭의 번호(001~) 버튼 설명 ----
// 번호(effectPreset)가 실제로 하는 일을 visual() 코드 기준으로 정리한 것. 버튼에 기호(glyph)와 툴팁(title)으로 보여 준다.
// 방향(direction)은 번호를 누를 때 함께 정해지고(presetDirection) 아래 Direction 콤보로 다시 바꿀 수 있다.
const pad3=(n:number)=>String(n+1).padStart(3,'0');
const SHAPES:[string,string][]=[['←','오른쪽에서 왼쪽으로 열림'],['→','왼쪽에서 오른쪽으로 열림'],['↑','아래에서 위로 열림'],['↓','위에서 아래로 열림'],['↔','가운데에서 좌우로 열림'],['↕','가운데에서 위아래로 열림'],['◤','왼쪽 위 모서리에서 대각선으로 열림'],['◥','오른쪽 위 모서리에서 대각선으로 열림']];
const SHAPE_SHORT=['좌','우','상','하','가로','세로','대각L','대각R'];   // 모양 이름(짧게)
const CORNERS:[string,string][]=[['◥','오른쪽 위'],['◤','왼쪽 위'],['◢','오른쪽 아래'],['◣','왼쪽 아래']];
const SIDES=['왼쪽','오른쪽','위','아래'];
// 효과별 번호 개수. 번호가 방향 말고는 아무 영향이 없는 효과(Crawl/Roll/Text)는 의미 있는 개수만 보여 준다.
export function presetCount(effect:string):number{return effect==='none'||effect==='fade'||effect==='blink'||effect==='text'?1:effect==='crawl'||effect==='roll'?2:effect==='scale'?6:effect==='banner'?7:effect==='move'?12:effect==='tile'?11:effect==='curl'?16:15}
// 번호를 눌렀을 때 정해지는 방향. 기본은 번호%4 = 왼쪽/오른쪽/위/아래, Crawl 은 왼쪽/오른쪽, Roll 은 위/아래.
export function presetDirection(effect:string,n:number):Item['direction']{return effect==='crawl'?(['left','right'] as const)[n%2]:effect==='roll'?(['up','down'] as const)[n%2]:(['left','right','up','down'] as const)[n%4]}
export function presetInfo(effect:string,n:number):{glyph:string;short:string;title:string}{
  const dup=(_base:number,period:number)=>n>=period?` (${pad3(n%period)}과 같은 모양)`:'';
  let glyph='',title='',short='';
  switch(effect){
    case 'none':glyph='Cut';short='바로';title='효과 없이 바로 나타남/사라짐';break;
    case 'fade':glyph='Fade';short='서서히';title='투명하게 시작해서 서서히 나타남(나갈 때는 서서히 사라짐)';break;
    case 'blink':glyph='ABC';short='깜박';title='깜박임(아래 Blink Count 만큼 켜졌다 꺼짐)';break;
    case 'text':glyph='Abc';short='타자';title='글자가 한 글자씩 차례로 나타남';break;
    case 'crawl':glyph=['←','→'][n%2];short=['좌로 흐름','우로 흐름'][n%2];title=n%2?'왼쪽에서 오른쪽으로 흘러감(Speed 로 속도 조절)':'오른쪽에서 왼쪽으로 흘러감(Speed 로 속도 조절)';break;
    case 'roll':glyph=['↑','↓'][n%2];short=['위로 올림','아래로'][n%2];title=n%2?'위에서 아래로 내려감(Speed 로 속도 조절)':'아래에서 위로 올라감(Speed 로 속도 조절)';break;
    case 'move':
      if(n<4){glyph=['←','→','↑','↓'][n];title=`${SIDES[n]}에서 들어옴`}
      else{const c=CORNERS[n%4];glyph=c[0];title=`${c[1]} 모서리에서 대각선으로 들어옴`+(n>=8?` (${pad3(n-4)}과 같음)`:'')}
      break;
    case 'scale':
      if(n<4){glyph=['←','→','↑','↓'][n];title=`${SIDES[n]} 끝을 기준으로 커짐`}
      else if(n===4){glyph='↔';title='왼쪽 끝을 기준으로 가로로만 커짐(세로 100% 고정)'}
      else{glyph='↕';title='가운데를 기준으로 세로로만 커짐(가로 100% 고정)'}
      break;
    case 'wipe':{const s=SHAPES[n%8];glyph=s[0];short=SHAPE_SHORT[n%8];title=`닦아내듯 ${s[1]}`+dup(0,8);break}
    case 'banner':{const s=SHAPES[n%8];glyph=s[0];short=SHAPE_SHORT[n%8]+(n%2?'↻':'↺');title=`배너처럼 펼쳐지며 ${s[1]}, ${n%2?'+':'-'}8° 기울어졌다 펴짐`;break}
    case 'curl':{const s=SHAPES[n%8];glyph=s[0];short=SHAPE_SHORT[n%8];title=`페이지가 말려 넘어가듯 ${s[1]} (Curl Radius/Angle 로 조절)`+dup(0,8);break}
    case 'organic':glyph=['■','▤','●'][n%3];short=['사각형','가로','원'][n%3];title=`${['작은 사각형이 커지며','가로로 열리는 조각들이','원이 커지며'][n%3]} 무작위 순서로 나타남(번호마다 순서가 다름)`;break;
    case 'tile':glyph=['▦','◪','▩'][n%3];short=['동시','대각선','무작위'][n%3];title=`타일로 나뉘어 ${['모두 동시에','대각선으로 번져가며','무작위 순서로'][n%3]} 나타남(Tile X/Y 로 개수 조절)`;break;
    default:glyph='';title='';
  }
  return {glyph,short,title:`${pad3(n)} · ${title}`};
}
