import { useRef, useCallback } from "react";
import { objectToHtml } from "./objectRender";

// 프로젝트 좌표계(1920x1080, main.cpp의 kW/kH와 동일)를 그대로 쓰고, 화면에는
// canvas-stage 전체에 CSS transform:scale() 한 번만 적용해 축소한다.
// (이전 버전은 각 개체 좌표에 scale을 곱했는데, 개체 내부의 HTML 콘텐츠는 원본
//  크기로 렌더링돼서 선택 핸들보다 훨씬 크게 삐져나오는 버그가 있었다.)
export const PROJECT_W = 1920;
export const PROJECT_H = 1080;

export default function Canvas({ objects, selectedId, onSelect, onChange, scale, bgMode = "check", bgColor = "#2a2a2a" }) {
  const dragRef = useRef(null); // { id, kind: 'move'|'resize', handle, startClientX, startClientY, orig }

  const onMouseDown = (e, obj, kind, handle) => {
    e.stopPropagation();
    onSelect(obj.id);
    dragRef.current = {
      id: obj.id,
      kind,
      handle,
      startClientX: e.clientX,
      startClientY: e.clientY,
      orig: { x: obj.x, y: obj.y, w: obj.w, h: obj.h },
    };
    window.addEventListener("mousemove", onMouseMove);
    window.addEventListener("mouseup", onMouseUp);
  };

  // 화면(클라이언트) 픽셀 이동량 -> 프로젝트 좌표 이동량 (scale로 나눔, canvas-stage 전체가 scale배 축소돼 있으므로)
  const onMouseMove = useCallback(
    (e) => {
      const d = dragRef.current;
      if (!d) return;
      const dx = (e.clientX - d.startClientX) / scale;
      const dy = (e.clientY - d.startClientY) / scale;
      if (d.kind === "move") {
        onChange(d.id, { x: Math.round(d.orig.x + dx), y: Math.round(d.orig.y + dy) });
      } else if (d.kind === "resize") {
        const patch = {};
        if (d.handle.includes("e")) patch.w = Math.max(4, Math.round(d.orig.w + dx));
        if (d.handle.includes("s")) patch.h = Math.max(4, Math.round(d.orig.h + dy));
        if (d.handle.includes("w")) {
          patch.w = Math.max(4, Math.round(d.orig.w - dx));
          patch.x = Math.round(d.orig.x + dx);
        }
        if (d.handle.includes("n")) {
          patch.h = Math.max(4, Math.round(d.orig.h - dy));
          patch.y = Math.round(d.orig.y + dy);
        }
        onChange(d.id, patch);
      }
    },
    [scale, onChange]
  );

  const onMouseUp = useCallback(() => {
    dragRef.current = null;
    window.removeEventListener("mousemove", onMouseMove);
    window.removeEventListener("mouseup", onMouseUp);
  }, [onMouseMove]);

  const handles = ["nw", "n", "ne", "e", "se", "s", "sw", "w"];
  // 핸들은 화면에서 항상 같은 크기(8px)로 보여야 하므로, scale로 나눠 프로젝트 좌표계 기준 크기로 역보정한다.
  const handlePx = 8 / scale;

  return (
    <div
      className={"canvas-stage-wrap" + (bgMode === "solid" ? " solid" : "")}
      style={{ width: PROJECT_W * scale, height: PROJECT_H * scale, ...(bgMode === "solid" ? { backgroundColor: bgColor, backgroundImage: "none" } : {}) }}
    >
      <div
        className="canvas-stage"
        style={{ width: PROJECT_W, height: PROJECT_H, transform: `scale(${scale})` }}
        onMouseDown={() => onSelect(null)}
      >
        {objects.map((o) => {
          const sel = o.id === selectedId;
          return (
            <div key={o.id}>
              <div
                className={"canvas-obj" + (sel ? " selected" : "")}
                style={{
                  left: o.x,
                  top: o.y,
                  width: o.w,
                  height: o.h,
                  outlineWidth: sel ? 2 / scale : 1 / scale,
                  transform: o.rotation ? `rotate(${o.rotation}deg)` : undefined,
                }}
                onMouseDown={(e) => onMouseDown(e, o, "move")}
                dangerouslySetInnerHTML={{ __html: objectToHtml({ ...o, x: 0, y: 0, rotation: 0 }) }}
              />
              {sel &&
                handles.map((h) => (
                  <div
                    key={h}
                    className={`handle handle-${h}`}
                    style={{
                      width: handlePx,
                      height: handlePx,
                      marginLeft: -handlePx / 2,
                      marginTop: -handlePx / 2,
                      left: o.x + (h.includes("w") ? 0 : h.includes("e") ? o.w : o.w / 2),
                      top: o.y + (h.includes("n") ? 0 : h.includes("s") ? o.h : o.h / 2),
                    }}
                    onMouseDown={(e) => onMouseDown(e, o, "resize", h)}
                  />
                ))}
            </div>
          );
        })}
      </div>
    </div>
  );
}
