import {Item,Run,clockText} from './model';
type Glyph={char:string;width:number;font:string;fill:string;size:number;rotate:number;start:number;underline:boolean};type Line={chars:Glyph[];width:number;height:number};
// 글자 폭/줄바꿈 계산은 비싸므로 (내용+스타일) 키로 결과를 캐시한다. 시계/타이머는 text 가 바뀔 때만 다시 계산된다.
const layoutCache=new Map<string,Line[]>(),widthCache=new Map<string,number>();
const measure=(c:CanvasRenderingContext2D,font:string,char:string)=>{const k=font+'|'+char;let w=widthCache.get(k);if(w===undefined){c.font=font;w=c.measureText(char).width;if(widthCache.size>5000)widthCache.clear();widthCache.set(k,w)}return w};
function layoutLines(c:CanvasRenderingContext2D,item:Item,text:string,scale:number):Line[]{
const key=JSON.stringify([text,item.runs,item.size,item.italic,item.bold,item.family,item.space,item.kerning,item.textWidth,item.w,item.align,item.cRotate,item.fill,item.underline]);const hit=layoutCache.get(key);if(hit)return hit;
const lines:{chars:{char:string;width:number;font:string;fill:string;size:number;rotate:number;start:number;underline:boolean}[];width:number;height:number}[]=[];let line={chars:[] as any[],width:0,height:item.size};let at=0;for(const char of text){const run=item.runs.reduce<Partial<Run>>((style,r)=>at>=r.start&&at<r.end?{...style,...r}:style,{});const size=run?.size??item.size;const font=`${(run?.italic??item.italic)?'italic ':''}${(run?.bold??item.bold)?'bold ':''}${size}px "${run.family??item.family}"`;const width=(measure(c,font,char)*(char===' '?item.space/100:1)+item.kerning)*scale;if(char==='\n'||(line.width+width>item.w-8&&line.chars.length)){lines.push(line);line={chars:[],width:0,height:item.size}}if(char!=='\n'){line.chars.push({char,width,font,fill:run?.fill??item.fill,size,rotate:run?.rotate??item.cRotate,underline:run.underline??item.underline,start:at});line.width+=width;line.height=Math.max(line.height,size)}at+=char.length}lines.push(line);
if(layoutCache.size>200)layoutCache.clear();layoutCache.set(key,lines);return lines}
export function drawText(c:CanvasRenderingContext2D,item:Item,time:number){const text=['clock','timer'].includes(item.type)?clockText(item,time):item.text;const scale=item.textWidth/100;const lines=layoutLines(c,item,text,scale);let y=4;for(const l of lines){if(y>item.h)break;let x=item.align==='center'?(item.w-l.width)/2:item.align==='right'?item.w-l.width-4:4;for(const g of l.chars){c.save();c.font=g.font;c.textBaseline='top';c.lineJoin='round';c.translate(x+g.width/2,y+g.size/2);c.rotate(-g.rotate*Math.PI/180);c.scale(scale,1);c.translate(-g.width/(2*scale),-g.size/2);if(item.shadow){c.shadowColor=item.shadowColor;c.shadowBlur=item.shadowBlur;c.shadowOffsetX=Math.cos(item.shadowAngle*Math.PI/180)*item.shadowDepth;c.shadowOffsetY=Math.sin(item.shadowAngle*Math.PI/180)*item.shadowDepth}const e1=item.strokeWidth,e2=e1+item.edge2Width,e3=e2+item.edge3Width;for(const [width,color] of [[e3,item.edge3],[e2,item.edge2],[e1,item.stroke]] as [number,string][]){if(width>0){c.lineWidth=width*2;c.strokeStyle=color;c.strokeText(g.char,0,0)}}if(!item.outline){c.fillStyle=g.fill;c.fillText(g.char,0,0);if(item.thickness>0){c.lineWidth=item.thickness;c.strokeStyle=g.fill;c.strokeText(g.char,0,0)}}if(g.underline){c.fillStyle=g.fill;c.fillRect(0,g.size,Math.max(1,g.width/scale),Math.max(1,g.size/20))}c.restore();x+=g.width}y+=l.height*(1+item.leading/100)}}

// 글자 내용에 딱 맞는 영역 크기. drawText 의 줄바꿈/글자 폭 계산과 같은 규칙을 쓴다(한쪽을 바꾸면 여기도 맞출 것).
// maxW: 줄바꿈 기준 최대 폭(보통 화면 폭 - x). 그보다 길면 줄바꿈해서 높이를 늘린다.
export function fitText(item:Item,maxW:number):{w:number;h:number}{
  const c=document.createElement('canvas').getContext('2d')!;
  const text=['clock','timer'].includes(item.type)?clockText(item,0):item.text;
  const scale=item.textWidth/100,gap=1+item.leading/100;
  const lines:{width:number;height:number;n:number}[]=[];
  let line={width:0,height:item.size,n:0},at=0;
  for(const char of text){
    const run=item.runs.reduce<Partial<Run>>((style,r)=>at>=r.start&&at<r.end?{...style,...r}:style,{});
    const size=run.size??item.size;
    c.font=`${(run.italic??item.italic)?'italic ':''}${(run.bold??item.bold)?'bold ':''}${size}px "${run.family??item.family}"`;
    const width=(c.measureText(char).width*(char===' '?item.space/100:1)+item.kerning)*scale;
    if(char==='\n'||(line.width+width>maxW-8&&line.n)){lines.push(line);line={width:0,height:item.size,n:0}}
    if(char!=='\n'){line.width+=width;line.height=Math.max(line.height,size);line.n++}
    at+=char.length;
  }
  lines.push(line);
  const edge=item.strokeWidth+item.edge2Width+item.edge3Width;   // 외곽선이 번지는 만큼만 여유(그림자는 영역 밖으로 나가도 됨)
  const h=6+edge+lines.reduce((sum,l,i)=>sum+(i<lines.length-1?l.height*gap:l.height*1.1),0);
  const w=8+edge+Math.max(...lines.map(l=>l.width));   // 폭 여백 8 미만이면 drawText 가 마지막 글자를 줄바꿈함(width > item.w-8)
  return {w:Math.ceil(Math.max(item.size*.6,w)),h:Math.ceil(Math.max(item.size*1.1,h))};
}
