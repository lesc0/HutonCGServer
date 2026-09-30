import { useEffect, useState, useRef } from "react";
import "./App.css";
import Canvas, { PROJECT_W, PROJECT_H } from "./Canvas";
import PropertiesPanel from "./PropertiesPanel";
import TemplatePanel from "./TemplatePanel";
import BroadcastControl from "./BroadcastControl";
import TopBar from "./TopBar";
import PageThumbnail from "./PageThumbnail";
import { objectsToHtml } from "./objectRender";
import { buildTemplate } from "./templates";

const emptyPage = () => ({ html: "", css: "", video: "", videoRect: [0, 0, PROJECT_W, PROJECT_H], objects: [] });

let uid = 1;
const newId = () => `o${Date.now()}_${uid++}`;

const DEFAULTS = {
  text: { type: "text", w: 500, h: 120, text: "새 문자", fontFamily: "sans-serif", fontSize: 70, color: "#ffffff", textAlign: "left" },
  rect: { type: "rect", w: 300, h: 200, fill: "#3388ff", opacity: 1, strokeWidth: 0 },
  ellipse: { type: "ellipse", w: 220, h: 220, fill: "#ffcc00", opacity: 1, strokeWidth: 0 },
  image: { type: "image", w: 300, h: 300, src: "" },
};

export default function App() {
  const [project, setProject] = useState(null);
  const [selected, setSelected] = useState(0);
  const [selectedObjId, setSelectedObjId] = useState(null);
  const [status, setStatus] = useState("");
  const stageWrapRef = useRef(null);
  const [scale, setScale] = useState(0.4);
  const [templates, setTemplates] = useState([]);
  const [templatesOpen, setTemplatesOpen] = useState(false);
  const [projectPath, setProjectPath] = useState("");
  // 편집화면 배경(매뉴얼 19-2장)은 방송 데이터가 아니라 이 브라우저에서의 작업 편의 설정이라 localStorage에 저장.
  const [bgMode, setBgMode] = useState(() => { try { return localStorage.getItem("cg_bgMode") || "check"; } catch { return "check"; } });
  const [bgColor, setBgColor] = useState(() => { try { return localStorage.getItem("cg_bgColor") || "#2a2a2a"; } catch { return "#2a2a2a"; } });
  const changeBg = (mode, color) => {
    setBgMode(mode); setBgColor(color);
    try { localStorage.setItem("cg_bgMode", mode); localStorage.setItem("cg_bgColor", color); } catch {}
  };

  useEffect(() => {
    fetch("/api/meta").then((r) => r.json()).then((m) => setProjectPath(m.projectPath)).catch(() => {});
  }, []);

  // 매뉴얼 7-1장 "새 프로젝트 만들기": 프로젝트 파일을 여러 개 관리하진 않지만(단일 파일 구조),
  // 지금 편집 중인 내용을 빈 페이지로 초기화하는 것으로 동일한 효과를 낸다. 저장 전까지는 서버 원본이 보존됨.
  const newProject = () => {
    setProject({ bg: "transparent", pages: [] });
    setSelected(0);
    setSelectedObjId(null);
    setStatus("새 프로젝트(저장 전)");
  };

  const loadProject = () =>
    fetch("/api/project")
      .then((r) => r.json())
      .then((data) => {
        if (!Array.isArray(data.pages)) data.pages = [];
        setProject(data);
        setSelected(0);
        setSelectedObjId(null);
        setStatus("불러옴");
      })
      .catch((e) => setStatus("불러오기 실패: " + e.message));

  // 매뉴얼 18-2장: F2 로 템플릿 창 열기/닫기
  useEffect(() => {
    const onKey = (e) => {
      if (e.key === "F2") { e.preventDefault(); setTemplatesOpen((v) => !v); }
    };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, []);

  useEffect(() => {
    fetch("/api/templates").then((r) => r.json()).then(setTemplates).catch(() => {});
  }, []);

  useEffect(() => { loadProject(); }, []);

  // 편집 영역 폭에 맞춰 캔버스 배율 자동 계산
  useEffect(() => {
    const el = stageWrapRef.current;
    if (!el) return;
    const ro = new ResizeObserver(() => {
      setScale(Math.max(0.1, (el.clientWidth - 32) / PROJECT_W));
    });
    ro.observe(el);
    return () => ro.disconnect();
  }, [project]);

  if (!project) return <div className="app">불러오는 중...</div>;

  const page = project.pages[selected];
  const objects = page?.objects || [];
  const selectedObj = objects.find((o) => o.id === selectedObjId) || null;

  const updatePage = (patch) => {
    const pages = project.pages.slice();
    pages[selected] = { ...pages[selected], ...patch };
    setProject({ ...project, pages });
  };

  const updateObjects = (nextObjects) => updatePage({ objects: nextObjects });

  const addObject = (type) => {
    const obj = { id: newId(), x: (PROJECT_W - DEFAULTS[type].w) / 2, y: (PROJECT_H - DEFAULTS[type].h) / 2, rotation: 0, ...DEFAULTS[type] };
    updateObjects([...objects, obj]);
    setSelectedObjId(obj.id);
  };

  // 함수형 업데이트로 작성: Canvas.jsx의 onMouseMove가 useCallback([scale])이라
  // 드래그 도중엔 오래된(stale) objects 클로저를 쓸 수 있음 -> setProject(prev=>...)로
  // 항상 "최신 state"를 기준으로 갱신해서 그 문제를 원천적으로 피한다.
  const changeObject = (id, patch) => {
    setProject((prev) => {
      const pages = prev.pages.slice();
      const pg = pages[selected];
      const objs = (pg.objects || []).map((o) => (o.id === id ? { ...o, ...patch } : o));
      pages[selected] = { ...pg, objects: objs };
      return { ...prev, pages };
    });
  };

  // 레이어 순서: objects 배열의 뒤쪽(나중 렌더)이 화면 위로 오므로, 배열 내 위치를 옮기는 것 = 레이어 변경.
  // 매뉴얼 10장: 가장 앞으로/가장 뒤로/한 단계 앞으로/한 단계 뒤로.
  const reorderObject = (id, mode) => {
    setProject((prev) => {
      const pages = prev.pages.slice();
      const pg = pages[selected];
      const objs = (pg.objects || []).slice();
      const i = objs.findIndex((o) => o.id === id);
      if (i < 0) return prev;
      const [o] = objs.splice(i, 1);
      if (mode === "front") objs.push(o);
      else if (mode === "back") objs.unshift(o);
      else if (mode === "forward") objs.splice(Math.min(i + 1, objs.length), 0, o);
      else if (mode === "backward") objs.splice(Math.max(i - 1, 0), 0, o);
      pages[selected] = { ...pg, objects: objs };
      return { ...prev, pages };
    });
  };

  // 정렬: 매뉴얼 10-1장, 페이지(캔버스) 기준 좌/우/상/하/수직중앙/수평중앙/전체가운데.
  const alignObject = (id, mode) => {
    setProject((prev) => {
      const pages = prev.pages.slice();
      const pg = pages[selected];
      const objs = (pg.objects || []).map((o) => {
        if (o.id !== id) return o;
        const patch = {};
        if (mode === "left") patch.x = 0;
        if (mode === "right") patch.x = PROJECT_W - o.w;
        if (mode === "top") patch.y = 0;
        if (mode === "bottom") patch.y = PROJECT_H - o.h;
        if (mode === "centerH") patch.x = (PROJECT_W - o.w) / 2;
        if (mode === "centerV") patch.y = (PROJECT_H - o.h) / 2;
        if (mode === "centerBoth") { patch.x = (PROJECT_W - o.w) / 2; patch.y = (PROJECT_H - o.h) / 2; }
        return { ...o, ...patch, x: Math.round(patch.x ?? o.x), y: Math.round(patch.y ?? o.y) };
      });
      pages[selected] = { ...pg, objects: objs };
      return { ...prev, pages };
    });
  };

  // 템플릿(매뉴얼 18장): 선택된 개체의 스타일을 저장하거나, 저장된 템플릿을 선택 개체에 적용.
  const saveTemplate = (obj) => {
    const name = window.prompt("템플릿 이름", obj.type === "text" ? (obj.text || "문자").slice(0, 10) : obj.type);
    if (name === null) return;
    const t = buildTemplate(obj, name);
    const next = [...templates, t];
    setTemplates(next);
    fetch("/api/templates", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(next) }).catch(() => {});
  };

  const deleteTemplate = (id) => {
    const next = templates.filter((t) => t.id !== id);
    setTemplates(next);
    fetch("/api/templates", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(next) }).catch(() => {});
  };

  const applyTemplate = (t) => {
    if (!selectedObjId || !selectedObj || selectedObj.type !== t.type) return;
    changeObject(selectedObjId, t.style);
  };

  const deleteObject = (id) => {
    updateObjects(objects.filter((o) => o.id !== id));
    setSelectedObjId(null);
  };

  const addPage = () => {
    const pages = [...project.pages, emptyPage()];
    setProject({ ...project, pages });
    setSelected(pages.length - 1);
    setSelectedObjId(null);
  };

  const removePage = (idx) => {
    const pages = project.pages.filter((_, i) => i !== idx);
    setProject({ ...project, pages });
    setSelected((s) => Math.max(0, Math.min(s, pages.length - 1)));
    setSelectedObjId(null);
  };

  // 매뉴얼 8-1장 "페이지 삽입": 현재 선택된 페이지번호에 새 페이지가 삽입되고 원래 페이지는 아래로 이동.
  const insertPage = () => {
    const pages = project.pages.slice();
    pages.splice(selected, 0, emptyPage());
    setProject({ ...project, pages });
    setSelectedObjId(null);
  };

  const prevPage = () => { setSelected((s) => Math.max(0, s - 1)); setSelectedObjId(null); };
  const nextPage = () => { setSelected((s) => Math.min(project.pages.length - 1, s + 1)); setSelectedObjId(null); };

  const save = () => {
    setStatus("저장 중...");
    // objects 를 가진 페이지는 html 을 objects 로부터 재생성(cef_mpp 재생 파이프라인용).
    const pages = project.pages.map((p) =>
      Array.isArray(p.objects) ? { ...p, html: objectsToHtml(p.objects) } : p
    );
    const toSave = { ...project, pages };
    fetch("/api/project", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(toSave),
    })
      .then((r) => r.json())
      .then((r) => setStatus(r.ok ? "저장됨" : "실패: " + r.error))
      .catch((e) => setStatus("실패: " + e.message));
  };

  // 매뉴얼 7-1/7-2장 "프로젝트 다른 이름으로 저장" — 활성 파일(PROJECT_PATH)과 별개로 이름 붙은 사본을 저장.
  const saveAs = (name) => {
    const pages = project.pages.map((p) => (Array.isArray(p.objects) ? { ...p, html: objectsToHtml(p.objects) } : p));
    setStatus("다른 이름으로 저장 중...");
    fetch(`/api/projects/${encodeURIComponent(name)}`, {
      method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ ...project, pages }),
    })
      .then((r) => r.json())
      .then((r) => setStatus(r.ok ? `"${r.name}" 으로 저장됨` : "실패: " + r.error))
      .catch((e) => setStatus("실패: " + e.message));
  };

  const openProjectList = () => fetch("/api/projects").then((r) => r.json()).catch(() => []);

  const openProjectByName = (name) => {
    fetch(`/api/projects/${encodeURIComponent(name)}`)
      .then((r) => r.json())
      .then((data) => {
        if (!Array.isArray(data.pages)) data.pages = [];
        setProject(data);
        setSelected(0);
        setSelectedObjId(null);
        setStatus(`"${name}" 불러옴(저장 전까지는 활성 파일에 반영 안 됨)`);
      })
      .catch((e) => setStatus("불러오기 실패: " + e.message));
  };

  return (
    <div className="app-root">
      <TopBar
        projectPath={projectPath}
        onNew={newProject}
        onSave={save}
        onSaveAs={saveAs}
        onOpenList={openProjectList}
        onOpenListSelect={openProjectByName}
        onReload={loadProject}
        status={status}
        bgMode={bgMode}
        bgColor={bgColor}
        onBgChange={changeBg}
      />
      <div className="app">
      <aside className="sidebar">
        <div className="sidebar-head">
          <span>페이지</span>
        </div>
        <ul className="page-list">
          {project.pages.map((p, i) => (
            <li key={i} className={i === selected ? "active" : ""}
                onClick={() => { setSelected(i); setSelectedObjId(null); }}>
              <span className="page-num">{i + 1}</span>
              <PageThumbnail page={p} />
              <span className="page-effect-badge" title="IN 효과(매뉴얼 8-3장)">
                {p.effectIn?.type === "fade" ? "Fa" : "CUT"}
                {p.effectIn?.type === "fade" && <span className="page-effect-num">{p.effectIn.speed ?? 5}</span>}
              </span>
            </li>
          ))}
        </ul>
        {/* 매뉴얼 8-1/8-2장: 페이지추가(맨 마지막)/삭제/삽입/이전/다음 아이콘 바 */}
        <div className="page-iconbar">
          <button onClick={addPage} title="페이지 추가(맨 마지막)">➕</button>
          <button onClick={() => removePage(selected)} title="페이지 삭제" disabled={!project.pages.length}>➖</button>
          <button onClick={insertPage} title="페이지 삽입(현재 위치)">⎘</button>
          <button onClick={prevPage} title="이전 페이지로" disabled={selected <= 0}>◀</button>
          <button onClick={nextPage} title="다음 페이지로" disabled={selected >= project.pages.length - 1}>▶</button>
        </div>
        <button className="save-btn" onClick={save}>저장</button>
        <div className="status">{status}</div>
        <BroadcastControl pageCount={project.pages.length} />
      </aside>

      <main className="editor">
        {!page ? (
          <div className="empty">왼쪽에서 페이지를 추가하세요</div>
        ) : (
          <>
            <div className="toolbar">
              <button onClick={() => addObject("text")}>T 문자</button>
              <button onClick={() => addObject("rect")}>▭ 사각형</button>
              <button onClick={() => addObject("ellipse")}>◯ 원형</button>
              <button onClick={() => addObject("image")}>🖼 이미지</button>
              <span className="toolbar-sep" />
              <label>video</label>
              <input className="video-field" value={page.video || ""} onChange={(e) => updatePage({ video: e.target.value })}
                     placeholder="mp4 경로 또는 hdmirx" />
              <label>IN효과</label>
              <select value={page.effectIn?.type || "cut"}
                      onChange={(e) => updatePage({ effectIn: { ...(page.effectIn || {}), type: e.target.value } })}>
                <option value="cut">CUT (즉시)</option>
                <option value="fade">Fade</option>
              </select>
              {page.effectIn?.type === "fade" && (
                <>
                  <label>빠르기</label>
                  <input type="range" min={1} max={10} value={page.effectIn?.speed ?? 5}
                         onChange={(e) => updatePage({ effectIn: { ...(page.effectIn || {}), speed: +e.target.value } })} />
                </>
              )}
            </div>
            <div className="stage-wrap" ref={stageWrapRef}>
              <Canvas
                objects={objects}
                selectedId={selectedObjId}
                onSelect={setSelectedObjId}
                onChange={changeObject}
                scale={scale}
                bgMode={bgMode}
                bgColor={bgColor}
              />
            </div>
          </>
        )}
      </main>

      <aside className="props-sidebar">
        <PropertiesPanel obj={selectedObj} onChange={changeObject} onDelete={deleteObject}
                         onReorder={reorderObject} onAlign={alignObject} onSaveTemplate={saveTemplate} />
      </aside>
      </div>

      {templatesOpen && (
        <div className="template-overlay" onClick={() => setTemplatesOpen(false)}>
          <div onClick={(e) => e.stopPropagation()}>
            <div className="template-panel-head">
              <span>템플릿 (F2로 닫기)</span>
              <button onClick={() => setTemplatesOpen(false)}>✕</button>
            </div>
            <TemplatePanel
              templates={templates}
              onApply={(t) => applyTemplate(t)}
              onDelete={deleteTemplate}
              canApplyType={selectedObj?.type}
            />
          </div>
        </div>
      )}
    </div>
  );
}
