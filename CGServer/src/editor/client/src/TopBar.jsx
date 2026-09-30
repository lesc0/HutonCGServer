import { useState } from "react";

// 매뉴얼 6-1/7-1장의 Main Menu(프로젝트) + 19장(환경설정)에 대응.
// 매뉴얼 7-1장 프로젝트 메뉴 순서: 새 프로젝트 / 프로젝트 열기 / 프로젝트 저장 / 프로젝트 다른이름으로 저장 / 프로젝트 백업 / 프로젝트 종료
export default function TopBar({
  projectPath, onNew, onSave, onSaveAs, onOpenList, onOpenListSelect, onReload, status, bgMode, bgColor, onBgChange,
}) {
  const [menu, setMenu] = useState(null); // null | 'project' | 'env'
  const [openList, setOpenList] = useState(null); // 프로젝트 열기 목록 (null = 안 보임)

  const toggle = (name) => setMenu((m) => (m === name ? null : name));

  const showOpenList = async () => {
    const list = await onOpenList();
    setOpenList(list);
  };

  return (
    <header className="topbar" onMouseLeave={() => setMenu(null)}>
      <div className="topbar-title">CGServer 저작</div>

      <div className="topbar-menu">
        <button className={"topbar-icon-btn" + (menu === "project" ? " active" : "")} title="프로젝트"
                onClick={() => toggle("project")}>📁</button>
        {menu === "project" && (
          <div className="topbar-dropdown">
            <button onClick={() => { if (window.confirm("새 프로젝트를 만들까요? 지금 편집 중인 모든 페이지가 지워집니다(저장 전이면 서버 내용은 그대로 남아있습니다).")) onNew(); setMenu(null); }}>
              새 프로젝트
            </button>
            <button onClick={() => { setMenu(null); showOpenList(); }}>프로젝트 열기</button>
            <button onClick={() => { onSave(); setMenu(null); }}>프로젝트 저장</button>
            <button onClick={() => {
              const name = window.prompt("다른 이름으로 저장 - 프로젝트 이름");
              if (name) onSaveAs(name);
              setMenu(null);
            }}>
              프로젝트 다른이름으로 저장
            </button>
            <div className="topbar-dropdown-sep" />
            <button onClick={() => { if (window.confirm("서버에 저장된 활성 파일 내용으로 다시 불러올까요? (지금 편집 중인 변경사항은 사라집니다)")) onReload(); setMenu(null); }}>
              다시 불러오기(활성 파일)
            </button>
          </div>
        )}
      </div>

      <div className="topbar-menu">
        <button className={"topbar-icon-btn" + (menu === "env" ? " active" : "")} title="환경설정"
                onClick={() => toggle("env")}>⚙️</button>
        {menu === "env" && (
          <div className="topbar-dropdown wide">
            <div className="topbar-dropdown-label">편집화면 배경 (매뉴얼 19-2장)</div>
            <label className="topbar-radio">
              <input type="radio" name="bg" checked={bgMode === "check"} onChange={() => onBgChange("check", bgColor)} />
              체크 패턴
            </label>
            <label className="topbar-radio">
              <input type="radio" name="bg" checked={bgMode === "solid"} onChange={() => onBgChange("solid", bgColor)} />
              단일 색상
              <input type="color" value={bgColor} disabled={bgMode !== "solid"}
                     onChange={(e) => onBgChange("solid", e.target.value)} />
            </label>
          </div>
        )}
      </div>

      <span className="topbar-sep" />
      <span className="topbar-path" title={projectPath}>{projectPath}</span>
      <span className="topbar-status">{status}</span>

      {openList && (
        <div className="template-overlay" onClick={() => setOpenList(null)}>
          <div onClick={(e) => e.stopPropagation()}>
            <div className="template-panel-head">
              <span>프로젝트 열기 (다른 이름으로 저장한 목록)</span>
              <button onClick={() => setOpenList(null)}>✕</button>
            </div>
            <div className="open-list">
              {openList.length === 0 && <div className="template-empty">저장된 프로젝트 없음 (먼저 "다른이름으로 저장" 해보세요)</div>}
              {openList.map((p) => (
                <button key={p.name} className="open-list-item" onClick={() => { setOpenList(null); onOpenListSelect(p.name); }}>
                  {p.name}
                  <span className="open-list-time">{new Date(p.mtime).toLocaleString()}</span>
                </button>
              ))}
            </div>
          </div>
        </div>
      )}
    </header>
  );
}
