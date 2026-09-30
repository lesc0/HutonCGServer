import { useEffect, useState } from "react";
import "./App.css";

const emptyPage = () => ({
  html: "",
  css: "",
  video: "",
  videoRect: [0, 0, 1920, 1080],
});

export default function App() {
  const [project, setProject] = useState(null);
  const [selected, setSelected] = useState(0);
  const [status, setStatus] = useState("");

  useEffect(() => {
    fetch("/api/project")
      .then((r) => r.json())
      .then((data) => {
        if (!Array.isArray(data.pages)) data.pages = [];
        setProject(data);
      })
      .catch((e) => setStatus("불러오기 실패: " + e.message));
  }, []);

  if (!project) return <div className="app">불러오는 중...</div>;

  const page = project.pages[selected];

  const updatePage = (patch) => {
    const pages = project.pages.slice();
    pages[selected] = { ...pages[selected], ...patch };
    setProject({ ...project, pages });
  };

  const addPage = () => {
    const pages = [...project.pages, emptyPage()];
    setProject({ ...project, pages });
    setSelected(pages.length - 1);
  };

  const removePage = (idx) => {
    const pages = project.pages.filter((_, i) => i !== idx);
    setProject({ ...project, pages });
    setSelected((s) => Math.max(0, Math.min(s, pages.length - 1)));
  };

  const movePage = (idx, dir) => {
    const j = idx + dir;
    if (j < 0 || j >= project.pages.length) return;
    const pages = project.pages.slice();
    [pages[idx], pages[j]] = [pages[j], pages[idx]];
    setProject({ ...project, pages });
    setSelected(j);
  };

  const save = () => {
    setStatus("저장 중...");
    fetch("/api/project", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(project),
    })
      .then((r) => r.json())
      .then((r) => setStatus(r.ok ? "저장됨" : "실패: " + r.error))
      .catch((e) => setStatus("실패: " + e.message));
  };

  const setRect = (i, v) => {
    const rect = page.videoRect.slice();
    rect[i] = Number(v) || 0;
    updatePage({ videoRect: rect });
  };

  return (
    <div className="app">
      <aside className="sidebar">
        <div className="sidebar-head">
          <span>페이지</span>
          <button onClick={addPage}>+ 추가</button>
        </div>
        <ul className="page-list">
          {project.pages.map((p, i) => (
            <li key={i} className={i === selected ? "active" : ""}>
              <span onClick={() => setSelected(i)}>
                {i + 1}. {p.video ? "🎬 " : ""}
                {(p.html || "").replace(/<[^>]+>/g, "").slice(0, 20) || "(빈 페이지)"}
              </span>
              <div className="page-actions">
                <button onClick={() => movePage(i, -1)} title="위로">↑</button>
                <button onClick={() => movePage(i, 1)} title="아래로">↓</button>
                <button onClick={() => removePage(i)} title="삭제">✕</button>
              </div>
            </li>
          ))}
        </ul>
        <label className="bg-field">
          bg
          <input
            value={project.bg || ""}
            onChange={(e) => setProject({ ...project, bg: e.target.value })}
          />
        </label>
        <button className="save-btn" onClick={save}>저장</button>
        <div className="status">{status}</div>
      </aside>

      <main className="editor">
        {!page ? (
          <div className="empty">왼쪽에서 페이지를 추가하세요</div>
        ) : (
          <>
            <div className="field">
              <label>HTML</label>
              <textarea
                rows={10}
                value={page.html}
                onChange={(e) => updatePage({ html: e.target.value })}
              />
            </div>
            <div className="field">
              <label>CSS</label>
              <textarea
                rows={10}
                value={page.css}
                onChange={(e) => updatePage({ css: e.target.value })}
              />
            </div>
            <div className="field">
              <label>video (경로 또는 "hdmirx")</label>
              <input
                value={page.video}
                onChange={(e) => updatePage({ video: e.target.value })}
                placeholder="예: /root/work/test_cef_mpp/web/girsday.mp4 또는 hdmirx"
              />
            </div>
            <div className="field">
              <label>videoRect (x, y, w, h)</label>
              <div className="rect-row">
                {page.videoRect.map((v, i) => (
                  <input
                    key={i}
                    type="number"
                    value={v}
                    onChange={(e) => setRect(i, e.target.value)}
                  />
                ))}
              </div>
            </div>
          </>
        )}
      </main>
    </div>
  );
}
