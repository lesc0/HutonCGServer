import { useState } from "react";
import { objectToHtml } from "./objectRender";

// 매뉴얼 18-2장: F2 로 열고/닫는 템플릿 창, 도형/문자 탭, 2번 클릭(여기선 더블클릭)으로 적용.
export default function TemplatePanel({ templates, onApply, onDelete, canApplyType }) {
  const [tab, setTab] = useState("text");
  const list = templates.filter((t) => t.type === tab || (tab === "shape" && t.type !== "text"));

  const previewStyle = (t) => {
    if (t.type === "text") {
      return { ...t.style, type: "text", x: 0, y: 0, w: 68, h: 48, text: "가나", fontSize: Math.min(t.style.fontSize || 28, 28) };
    }
    return { ...t.style, type: t.type, x: 0, y: 0, w: 68, h: 48 };
  };

  return (
    <div className="template-panel">
      <div className="template-tabs">
        <button className={tab === "shape" ? "active" : ""} onClick={() => setTab("shape")}>도형</button>
        <button className={tab === "text" ? "active" : ""} onClick={() => setTab("text")}>문자</button>
      </div>
      <div className="template-grid">
        {list.length === 0 && <div className="template-empty">저장된 템플릿 없음</div>}
        {list.map((t) => (
          <div
            key={t.id}
            className={"template-item" + (canApplyType && canApplyType !== t.type ? " disabled" : "")}
            title={t.name + " (더블클릭: 적용)"}
            onDoubleClick={() => onApply(t)}
          >
            <div className="template-thumb" dangerouslySetInnerHTML={{ __html: objectToHtml(previewStyle(t)) }} />
            <button className="template-del" onClick={(e) => { e.stopPropagation(); onDelete(t.id); }}>✕</button>
          </div>
        ))}
      </div>
    </div>
  );
}
