'use client';
// Style Catalog > Symbol 탭: 특수문자를 누르면 선택한 문자 개체의 커서 위치에 들어간다(page.tsx 의 insertSymbol).
// 방송 자막에서 자주 쓰는 기호 위주. 글꼴에 없는 글자는 대체 글꼴로 그려진다.

export const SYMBOL_GROUPS:{name:string;chars:string}[]=[
  {name:'일반 기호',chars:'※★☆●○◎◇◆□■△▲▽▼◁◀▷▶♠♤♣♧♥♡♦◈▣▒▤▥▦▧▨▩'},
  {name:'화살표',chars:'←→↑↓↔↕↖↗↘↙⇐⇒⇑⇓⇔➔➜➡➤▲▼◀▶↩↪↺↻'},
  {name:'숫자·글자 묶음',chars:'①②③④⑤⑥⑦⑧⑨⑩⑪⑫⑬⑭⑮⑯⑰⑱⑲⑳㉠㉡㉢㉣㉤㉥㉦㉧㉨㉩㉪㉫㉬㉭㉮㉯㉰㉱㉲㉳㉴㉵㉶㉷㉸㉹㉺㉻㈀㈁㈂㈃㈄㈅㈆㈇㈈㈉㈊㈋㈌㈍㈎㈏'},
  {name:'괄호·인용',chars:'「」『』〔〕〈〉《》【】〖〗‘’“”‹›«»‚„〝〞〃'},
  {name:'단위·통화',chars:'℃℉㎏㎎㎖㎘㎜㎝㎞㎡㎥㎧㎨㎳㎲㎾㎿％‰‱￦＄€£¥¢₩µΩ㏊㏈㏘㏜㏐'},
  {name:'수학',chars:'±×÷≠≈≤≥≦≧∞√∑∏∫∂∆∇∈∋∩∪⊂⊃∧∨¬∴∵∠⊥∥∽∝≡≒≪≫'},
  {name:'로마 숫자·그리스',chars:'ⅠⅡⅢⅣⅤⅥⅦⅧⅨⅩⅰⅱⅲⅳⅴⅵⅶⅷⅸⅹαβγδεζηθικλμνξοπρστυφχψωΑΒΓΔΩΠΣ'},
  {name:'음표·날씨·기타',chars:'♩♪♬♫☀☁☂☃☎☏☜☞☝☟☺☻☹♀♂✓✔✕✖✗✘✚☑☒⚠⚡✿❀❁❤❥✈☕♨'},
  {name:'점·선·구분',chars:'·•‥…‧∙─━│┃┌┐└┘├┤┬┴┼═║╔╗╚╝╠╣╦╩╬‐‑‒–—―～〜'},
  {name:'상표·저작권',chars:'©®™℠℗№℡‼⁇⁈⁉§¶†‡′″‴'},
];

export default function SymbolPicker({onPick,enabled}:{onPick:(ch:string)=>void;enabled:boolean}){
  return <div className="symbol-picker">
    <div className="symbol-hint">{enabled?'누르면 선택한 문자 개체의 커서 위치에 들어갑니다(드래그로 선택한 글자는 바뀝니다)':'문자 개체를 선택하세요'}</div>
    {SYMBOL_GROUPS.map(g=><div key={g.name} className="symbol-group">
      <div className="symbol-group-name">{g.name}</div>
      <div className="symbol-grid">{[...new Set([...g.chars])].map(c=><button key={c} title={`${c}  U+${c.codePointAt(0)!.toString(16).toUpperCase().padStart(4,'0')}`} disabled={!enabled}
        onMouseDown={e=>e.preventDefault()} onClick={()=>onPick(c)}>{c}</button>)}</div>
    </div>)}
  </div>
}
