// rect/ellipse 도형의 확장 속성(모서리 반경·다각형 종류·그라데이션·줄무늬) 그리기.
// 송출 쪽(bin/web/cg-runtime.js 의 drawShape/shapePath)과 동일한 로직이므로 한쪽을 고치면 다른 쪽도 같이 고칠 것.
//   gradient: 'linear:<각도>:<색1>:<색2>[:<색3>…]' | 'radial:0:<중심색>:<바깥색>'  (각도는 CSS 와 같음: 0=위로, 90=오른쪽, 180=아래)
//   stripe:   '<각도>:<굵기px>:<색>'  (그라데이션/단색 위에 덧그리는 줄무늬, 각도는 줄무늬가 진행하는 방향)
//   shapeKind: 아래 SHAPE_POINTS 의 이름. 비어 있으면 rect/ellipse 기본 모양(+radius).
import type {Item} from './model';

const arc=(cx:number,cy:number,r:number,a0:number,a1:number,n=18):[number,number][]=>Array.from({length:n+1},(_,i)=>{const a=(a0+(a1-a0)*i/n)*Math.PI/180;return [cx+r*Math.cos(a),cy+r*Math.sin(a)] as [number,number]});
export const SHAPE_POINTS:Record<string,[number,number][]>={
 triangle:[[.5,0],[1,1],[0,1]],
 wedge:[[0,.62],[1,0],[.5,1]],
 quarter:[[0,1],...arc(0,1,1,-90,0)],
 parallelogram:[[.2,0],[1,0],[.8,1],[0,1]],
 trapezoid:[[.15,0],[.85,0],[1,1],[0,1]],
 slant:[[.04,0],[1,0],[.96,1],[0,1]],
 diamond:[[.5,0],[1,.5],[.5,1],[0,.5]],
 pentagon:[[.5,0],[1,.38],[.82,1],[.18,1],[0,.38]],
 hexagon:[[.25,0],[.75,0],[1,.5],[.75,1],[.25,1],[0,.5]],
 chevron:[[0,0],[.7,0],[1,.5],[.7,1],[0,1],[.3,.5]],
 blob:[[.1,.2],[.55,0],[1,.15],[.9,.8],[.45,1],[0,.7]],
 leaf:[[0,1],[.05,.45],[.3,.12],[1,0],[.92,.6],[.55,.92]],
};
export const SHAPE_KINDS=Object.keys(SHAPE_POINTS);

const COLOR=/^#[0-9a-f]{6}([0-9a-f]{2})?$/i;
export type Grad={type:'linear'|'radial';angle:number;colors:string[]};
export function parseGradient(s?:string):Grad|null{if(!s)return null;const p=s.split(':');if((p[0]!=='linear'&&p[0]!=='radial')||p.length<4)return null;const angle=Number(p[1]);const colors=p.slice(2);if(!Number.isFinite(angle)||colors.length<2||colors.length>8||!colors.every(c=>COLOR.test(c)))return null;return {type:p[0],angle,colors}}
export type Stripe={angle:number;size:number;color:string};
export function parseStripe(s?:string):Stripe|null{if(!s)return null;const p=s.split(':');if(p.length!==3)return null;const angle=Number(p[0]),size=Number(p[1]);if(!Number.isFinite(angle)||!Number.isFinite(size)||size<2||size>200||!COLOR.test(p[2]))return null;return {angle,size,color:p[2]}}

export function shapePath(c:CanvasRenderingContext2D,i:Item){
 c.beginPath();
 const pts=i.shapeKind?SHAPE_POINTS[i.shapeKind]:undefined;
 if(pts){pts.forEach(([x,y],n)=>n?c.lineTo(x*i.w,y*i.h):c.moveTo(x*i.w,y*i.h));c.closePath()}
 else if(i.type==='ellipse')c.ellipse(i.w/2,i.h/2,i.w/2,i.h/2,0,0,Math.PI*2);
 else if(i.radius>0)c.roundRect(0,0,i.w,i.h,Math.min(i.radius,i.w/2,i.h/2));
 else c.rect(0,0,i.w,i.h);
}
function paint(c:CanvasRenderingContext2D,i:Item):string|CanvasGradient{
 const g=parseGradient(i.gradient);if(!g)return i.fill;
 let grad:CanvasGradient;
 if(g.type==='radial')grad=c.createRadialGradient(i.w/2,i.h/2,0,i.w/2,i.h/2,Math.max(i.w,i.h)/2);
 else{const a=g.angle*Math.PI/180,dx=Math.sin(a),dy=-Math.cos(a),len=Math.abs(i.w*dx)+Math.abs(i.h*dy);grad=c.createLinearGradient(i.w/2-dx*len/2,i.h/2-dy*len/2,i.w/2+dx*len/2,i.h/2+dy*len/2)}
 g.colors.forEach((col,n)=>grad.addColorStop(n/(g.colors.length-1),col));
 return grad;
}
export function drawShape(c:CanvasRenderingContext2D,i:Item){
 c.save();
 if(i.shadow){c.shadowColor=i.shadowColor;c.shadowBlur=i.shadowBlur}
 shapePath(c,i);c.fillStyle=paint(c,i);c.fill();
 c.restore();
 const st=parseStripe(i.stripe);
 if(st){
  c.save();shapePath(c,i);c.clip();
  const d=Math.hypot(i.w,i.h);
  c.translate(i.w/2,i.h/2);c.rotate((st.angle-90)*Math.PI/180);c.fillStyle=st.color;
  for(let x=-d,n=0;x<d&&n<600;x+=st.size*2,n++)c.fillRect(x,-d,st.size,d*2);
  c.restore();
 }
 if(i.strokeWidth>0){shapePath(c,i);c.lineWidth=i.strokeWidth;c.strokeStyle=i.stroke;c.stroke()}
}

// 카탈로그/썸네일 미리보기용 CSS (캔버스 결과와 같은 모양을 흉내냄)
export function shapeCss(i:Pick<Item,'fill'|'gradient'|'stripe'|'shapeKind'|'radius'|'type'|'w'|'h'>):React.CSSProperties{
 const g=parseGradient(i.gradient),st=parseStripe(i.stripe),layers:string[]=[];
 if(st)layers.push(`repeating-linear-gradient(${st.angle}deg,${st.color} 0 ${st.size}px,transparent ${st.size}px ${st.size*2}px)`);
 if(g)layers.push(g.type==='radial'?`radial-gradient(circle closest-side,${g.colors.join(',')})`:`linear-gradient(${g.angle}deg,${g.colors.join(',')})`);
 const css:React.CSSProperties={background:layers.length?layers.join(',')+(g?'':','+i.fill):i.fill};
 const pts=i.shapeKind?SHAPE_POINTS[i.shapeKind]:undefined;
 if(pts)css.clipPath='polygon('+pts.map(([x,y])=>`${(x*100).toFixed(1)}% ${(y*100).toFixed(1)}%`).join(',')+')';
 else if(i.type==='ellipse')css.borderRadius='50%';
 else if(i.radius>0)css.borderRadius=Math.min(50,i.radius/Math.max(1,Math.min(i.w,i.h))*100)+'%';
 return css;
}
