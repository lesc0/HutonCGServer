// 캔버스의 objects 배열을 cef_mpp(player.html)가 읽는 html/css 문자열로 변환한다.
// player.html은 project.json의 page.html을 innerHTML로 그대로 삽입하므로
// 여기서 만든 것이 곧 실제 방송 화면이 된다.

function escapeHtml(s) {
  return String(s ?? "")
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;");
}

function baseStyle(o) {
  let s = `position:absolute;left:${o.x}px;top:${o.y}px;width:${o.w}px;height:${o.h}px;`;
  if (o.rotation) s += `transform:rotate(${o.rotation}deg);`;
  return s;
}

export function objectToHtml(o) {
  const base = baseStyle(o);
  switch (o.type) {
    case "text":
      return `<div style="${base}font-family:'${o.fontFamily || "sans-serif"}';font-size:${o.fontSize ?? 48}px;color:${o.color || "#ffffff"};text-align:${o.textAlign || "left"};font-weight:${o.bold ? "bold" : "normal"};white-space:pre-wrap;line-height:1.1;">${escapeHtml(o.text)}</div>`;
    case "rect":
      return `<div style="${base}background:${o.fill || "#ffffff"};opacity:${o.opacity ?? 1};border-radius:${o.borderRadius ?? 0}px;${o.strokeWidth ? `border:${o.strokeWidth}px solid ${o.stroke || "#000"};` : ""}"></div>`;
    case "ellipse":
      return `<div style="${base}background:${o.fill || "#ffffff"};opacity:${o.opacity ?? 1};border-radius:50%;${o.strokeWidth ? `border:${o.strokeWidth}px solid ${o.stroke || "#000"};` : ""}"></div>`;
    case "image":
      return `<img src="${o.src ? `file://${o.src}` : ""}" style="${base}object-fit:contain;">`;
    default:
      return "";
  }
}

// objects 배열 -> player.html 에 들어갈 html 문자열 (css는 인라인 스타일로 처리하므로 빈 문자열)
export function objectsToHtml(objects) {
  return (objects || []).map(objectToHtml).join("\n");
}
