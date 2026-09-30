// 매뉴얼 11~14장 기준값: 문자 크기 기본 70(4~900), 장평/자간/줄간은 추후 단계에서 추가.
// 투명도는 매뉴얼과 동일하게 0(불투명)~100(투명) 스케일로 보여주고 내부적으로 opacity(0~1)로 저장.
const FONTS = ["sans-serif", "serif", "monospace", "Nanum Gothic", "Nanum Myeongjo"];

export default function PropertiesPanel({ obj, onChange, onDelete, onReorder, onAlign, onSaveTemplate }) {
  if (!obj) return <div className="props-empty">개체를 선택하세요</div>;

  const set = (patch) => onChange(obj.id, patch);

  return (
    <div className="props">
      <div className="props-head">
        <span>{{ text: "문자", rect: "사각형", ellipse: "원형", image: "이미지" }[obj.type]}</span>
        <button className="danger" onClick={() => onDelete(obj.id)}>삭제</button>
      </div>

      {(obj.type === "text" || obj.type === "rect" || obj.type === "ellipse") && onSaveTemplate && (
        <button className="template-add-btn" onClick={() => onSaveTemplate(obj)}>💾 템플릿에 추가 (F2로 목록)</button>
      )}

      <div className="props-section">
        <div className="props-label">레이어(매뉴얼 10장)</div>
        <div className="icon-row">
          <button title="가장 앞으로" onClick={() => onReorder(obj.id, "front")}>⏫</button>
          <button title="한 단계 앞으로" onClick={() => onReorder(obj.id, "forward")}>▲</button>
          <button title="한 단계 뒤로" onClick={() => onReorder(obj.id, "backward")}>▼</button>
          <button title="가장 뒤로" onClick={() => onReorder(obj.id, "back")}>⏬</button>
        </div>
      </div>

      <div className="props-section">
        <div className="props-label">정렬(페이지 기준)</div>
        <div className="icon-row">
          <button title="좌측 정렬" onClick={() => onAlign(obj.id, "left")}>⇤</button>
          <button title="수평 중앙" onClick={() => onAlign(obj.id, "centerH")}>↔</button>
          <button title="우측 정렬" onClick={() => onAlign(obj.id, "right")}>⇥</button>
          <button title="위로 정렬" onClick={() => onAlign(obj.id, "top")}>⇧</button>
          <button title="수직 중앙" onClick={() => onAlign(obj.id, "centerV")}>↕</button>
          <button title="아래로 정렬" onClick={() => onAlign(obj.id, "bottom")}>⇩</button>
          <button title="전체 가운데" onClick={() => onAlign(obj.id, "centerBoth")}>⊹</button>
        </div>
      </div>

      <div className="props-row">
        <label>X</label><input type="number" value={obj.x} onChange={(e) => set({ x: +e.target.value })} />
        <label>Y</label><input type="number" value={obj.y} onChange={(e) => set({ y: +e.target.value })} />
      </div>
      <div className="props-row">
        <label>W</label><input type="number" value={obj.w} onChange={(e) => set({ w: +e.target.value })} />
        <label>H</label><input type="number" value={obj.h} onChange={(e) => set({ h: +e.target.value })} />
      </div>
      <div className="props-row">
        <label>회전</label>
        <input type="number" value={obj.rotation || 0} onChange={(e) => set({ rotation: +e.target.value })} />
      </div>

      {obj.type === "text" && (
        <>
          <div className="props-row">
            <label>내용</label>
            <textarea rows={3} value={obj.text || ""} onChange={(e) => set({ text: e.target.value })} />
          </div>
          <div className="props-row">
            <label>폰트</label>
            <select value={obj.fontFamily || "sans-serif"} onChange={(e) => set({ fontFamily: e.target.value })}>
              {FONTS.map((f) => <option key={f} value={f}>{f}</option>)}
            </select>
          </div>
          <div className="props-row">
            <label>크기</label>
            <input type="number" min={4} max={900} value={obj.fontSize ?? 70}
                   onChange={(e) => set({ fontSize: +e.target.value })} />
          </div>
          <div className="props-row">
            <label>색상</label>
            <input type="color" value={obj.color || "#ffffff"} onChange={(e) => set({ color: e.target.value })} />
          </div>
          <div className="props-row">
            <label>정렬</label>
            <select value={obj.textAlign || "left"} onChange={(e) => set({ textAlign: e.target.value })}>
              <option value="left">좌측</option>
              <option value="center">중앙</option>
              <option value="right">우측</option>
            </select>
          </div>
          <div className="props-row">
            <label>굵게</label>
            <input type="checkbox" checked={!!obj.bold} onChange={(e) => set({ bold: e.target.checked })} />
          </div>
        </>
      )}

      {(obj.type === "rect" || obj.type === "ellipse") && (
        <>
          <div className="props-row">
            <label>채우기</label>
            <input type="color" value={obj.fill || "#ffffff"} onChange={(e) => set({ fill: e.target.value })} />
          </div>
          <div className="props-row">
            <label>투명도</label>
            <input type="range" min={0} max={100}
                   value={Math.round((1 - (obj.opacity ?? 1)) * 100)}
                   onChange={(e) => set({ opacity: 1 - e.target.value / 100 })} />
          </div>
          <div className="props-row">
            <label>테두리색</label>
            <input type="color" value={obj.stroke || "#000000"} onChange={(e) => set({ stroke: e.target.value })} />
          </div>
          <div className="props-row">
            <label>테두리두께</label>
            <input type="number" min={0} max={50} value={obj.strokeWidth ?? 0}
                   onChange={(e) => set({ strokeWidth: +e.target.value })} />
          </div>
          {obj.type === "rect" && (
            <div className="props-row">
              <label>모서리</label>
              <input type="number" min={0} value={obj.borderRadius ?? 0}
                     onChange={(e) => set({ borderRadius: +e.target.value })} />
            </div>
          )}
        </>
      )}

      {obj.type === "image" && (
        <div className="props-row">
          <label>경로</label>
          <input value={obj.src || ""} onChange={(e) => set({ src: e.target.value })}
                 placeholder="/root/work/CGServer/bin/web/assets/xxx.png" />
        </div>
      )}
    </div>
  );
}
