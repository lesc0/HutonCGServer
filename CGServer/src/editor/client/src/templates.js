// 매뉴얼 18장: 템플릿에 저장되는 개체 속성
//  - 문자: 폰트종류/크기, 채우기(색상), 정렬, 굵게
//  - 도형: 채우기, 테두리(색상/두께)
export const STYLE_KEYS = {
  text: ["fontFamily", "fontSize", "color", "textAlign", "bold"],
  rect: ["fill", "opacity", "stroke", "strokeWidth", "borderRadius"],
  ellipse: ["fill", "opacity", "stroke", "strokeWidth"],
};

export function extractStyle(obj) {
  const keys = STYLE_KEYS[obj.type] || [];
  const style = {};
  for (const k of keys) if (obj[k] !== undefined) style[k] = obj[k];
  return style;
}

let uid = 1;
export function buildTemplate(obj, name) {
  return {
    id: `t${Date.now()}_${uid++}`,
    type: obj.type,
    name: name || "템플릿",
    style: extractStyle(obj),
  };
}
