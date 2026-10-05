// Style Catalog > Page 탭의 기본 제공 템플릿(문자발생기에서 자주 쓰는 구성). 프로젝트의 사용자 템플릿(project.templates) 앞에 표시된다.
// 클릭하면 현재 페이지의 내용이 이 템플릿으로 바뀐다(page.tsx). 글자/색/위치는 적용 후 자유롭게 수정.
import {Item,Page,Kind,make,blankPage} from './model';

const FONT='NotoSansKR';
const NAVY='#0a2a52',DARK='#101830',RED='#c8202c',GOLD='#ffd23c',WHITE='#ffffff';
const bar=(x:number,y:number,w:number,h:number,fill:string,p:Partial<Item>={})=>make('rect',{name:'바탕',x,y,w,h,fill,effect:'wipe',outEffect:'wipe',inDuration:.6,outDuration:.6,...p});
const txt=(text:string,x:number,y:number,w:number,size:number,p:Partial<Item>={})=>make('text',{name:text.split('\n')[0].slice(0,12)||'자막',text,x,y,w,h:Math.round(size*1.35)*(text.split('\n').length),size,fill:WHITE,family:FONT,bold:true,effect:'fade',outEffect:'fade',inDuration:.6,outDuration:.6,...p});
const tpl=(name:string,items:Item[],duration=10):Page=>({...blankPage(),name,duration,items:items.map(i=>({...i,duration:Math.min(i.duration,duration)}))});
const k=(type:Kind,p:Partial<Item>)=>make(type,p);

const rows=[['1','홍길동','1:02.35'],['2','김철수','1:03.10'],['3','이영희','1:03.84'],['4','박민수','1:04.21'],['5','최지은','1:05.02']];

export const builtinTemplates:Page[]=[
 tpl('하단 자막 1줄',[
  bar(0,900,1920,120,NAVY,{opacity:.92}),bar(0,900,16,120,GOLD),
  txt('여기에 자막을 입력하세요',60,918,1800,60,{align:'left'}),
 ]),
 tpl('하단 자막 2줄',[
  bar(0,820,1920,220,DARK,{opacity:.92}),bar(0,820,1920,8,GOLD),
  txt('제목을 입력하세요',80,845,1760,52,{fill:GOLD}),
  txt('내용을 입력하세요',80,925,1760,66),
 ]),
 tpl('인터뷰 이름표',[
  bar(100,820,900,170,WHITE,{opacity:.96}),bar(100,820,14,170,RED),
  txt('홍길동',140,832,840,76,{fill:'#101010'}),
  txt('OO대학교 교수',140,925,840,42,{fill:'#444444',bold:false}),
 ]),
 tpl('뉴스 속보',[
  bar(0,900,300,130,RED),txt('속보',0,925,300,80,{align:'center'}),
  bar(300,900,1620,130,'#111111',{opacity:.92}),
  txt('속보 내용을 입력하세요',340,925,1560,64),
 ]),
 tpl('하단 가로 스크롤',[
  bar(0,960,1920,100,NAVY,{opacity:.95}),
  txt('가로로 흘러가는 문구를 입력하세요  ●  여러 문장을 이어서 넣을 수 있습니다  ●  속도는 지속 시간으로 조절합니다',1921,975,5200,60,{h:72,effect:'crawl',outEffect:'none',speed:.7,duration:20}),
 ],20),
 tpl('타이틀 (중앙)',[
  txt('제목을 입력하세요',160,400,1600,130,{align:'center',stroke:'#000000',strokeWidth:4,outline:true}),
  bar(760,555,400,6,GOLD),
  txt('부제목을 입력하세요',160,580,1600,56,{align:'center',fill:GOLD}),
 ]),
 tpl('로고 + 시계',[
  k('ellipse',{name:'로고',x:60,y:50,w:110,h:110,fill:RED,effect:'fade',outEffect:'fade'}),
  txt('CG',60,78,110,52,{align:'center'}),
  txt('채널명',190,72,600,56,{stroke:'#000000',strokeWidth:3,outline:true}),
  k('clock',{name:'시계',x:1500,y:70,w:360,h:76,size:56,align:'right',fill:WHITE,family:FONT,bold:true,stroke:'#000000',strokeWidth:3,outline:true,clockFormat:'HH:mm:ss'}),
 ]),
 tpl('스코어보드',[
  bar(560,40,800,110,'#111827',{opacity:.95}),
  txt('HOME',580,62,250,52,{align:'right'}),
  bar(850,55,110,80,RED,{effect:'fade'}),txt('0',850,62,110,60,{align:'center'}),
  txt(':',955,58,50,60,{align:'center'}),
  bar(1010,55,110,80,'#1c5cc8',{effect:'fade'}),txt('0',1010,62,110,60,{align:'center'}),
  txt('AWAY',1140,62,220,52,{align:'left'}),
 ]),
 tpl('정보 박스 (우측)',[
  bar(1320,200,520,520,'#0b1530',{opacity:.92,radius:24}),
  txt('오늘의 날씨',1350,222,460,54,{fill:GOLD}),
  ...['서울  12°C  맑음','부산  15°C  구름','광주  14°C  흐림','제주  17°C  비'].map((t,n)=>txt(t,1350,320+n*90,460,44,{bold:false})),
 ]),
 tpl('순위표 5위',[
  bar(560,160,800,100,RED,{radius:0}),txt('순위',560,180,800,56,{align:'center'}),
  ...rows.flatMap(([r,n,rec],i)=>{const y=260+i*110;return [
   bar(560,y,800,110,i%2?'#0f1a3c':'#13204a',{effect:'fade'}),
   txt(r,590,y+22,80,56,{align:'center',fill:GOLD}),
   txt(n,700,y+22,420,56),
   txt(rec,1100,y+22,230,56,{align:'right'}),
  ]}),
 ]),
 tpl('자막방송 (대사 2줄)',[
  bar(260,880,1400,160,'#000000',{opacity:.55,radius:16,effect:'fade',outEffect:'fade'}),
  txt('첫 번째 줄\n두 번째 줄',300,890,1320,56,{align:'center',stroke:'#000000',strokeWidth:3,outline:true}),
 ]),
 tpl('장소 + LIVE',[
  bar(60,60,640,90,'#000000',{opacity:.6,radius:12}),
  txt('서울 · 2026.10.01',90,80,580,46,{bold:false}),
  bar(1700,60,160,70,RED,{radius:12,effect:'fade'}),txt('LIVE',1700,72,160,48,{align:'center'}),
 ]),
 tpl('공지 박스 (중앙)',[
  bar(360,300,1200,480,WHITE,{opacity:.96,radius:30,effect:'fade',outEffect:'fade'}),
  bar(360,300,1200,120,NAVY,{radius:0,effect:'fade',outEffect:'fade'}),
  txt('공지사항',360,325,1200,66,{align:'center'}),
  txt('공지 내용을 입력하세요\n두 줄까지 넣을 수 있습니다',420,470,1080,54,{align:'center',fill:'#222222',bold:false}),
 ]),
];
