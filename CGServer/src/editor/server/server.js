// CG 저작 서버: project.json 을 읽고/쓰고, 빌드된 React 클라이언트를 서빙한다.
// cef_mpp 의 --edit(cefQuery save:/load) 와 같은 project.json 을 다뤄서,
// HDMI-1 로컬 저작(cef_mpp --edit)과 원격 브라우저 저작이 같은 파일을 공유한다.
const express = require("express");
const fs = require("fs");
const path = require("path");
const dgram = require("dgram");

const PORT = process.env.CGEDITOR_PORT || 3300;
const PROJECT_PATH = process.env.CGEDITOR_PROJECT ||
  path.join(__dirname, "..", "..", "..", "build", "project.json");
// 템플릿(매뉴얼 18장)은 프로젝트 종속이 아니라 저작 서버 전체가 공유하는 라이브러리라 별도 파일에 저장.
const TEMPLATES_PATH = process.env.CGEDITOR_TEMPLATES || path.join(__dirname, "templates.json");
// cef_mpp 실행 중 제어(UDP 127.0.0.1:5555, cgctl.sh 와 동일 프로토콜) + 상태 확인용 로그.
const CG_CTL_PORT = Number(process.env.CG_CTL_PORT) || 5555;
const CG_LOG = process.env.CGEDITOR_LOG || path.join(path.dirname(PROJECT_PATH), "run_cgserver.log");
// 매뉴얼 7-1/7-2장 "프로젝트 다른 이름으로 저장" / "프로젝트 열기"용 스냅샷 저장소.
// PROJECT_PATH(cef_mpp가 실제로 읽는 파일)와는 별개 — 여기 저장/열기는 "저장"(활성 파일 덮어쓰기)과 분리된
// 이름 붙은 사본이다. 활성화(실제 송출에 쓰기)하려면 cef_mpp를 그 파일로 재시작해야 한다.
const PROJECTS_DIR = process.env.CGEDITOR_PROJECTS_DIR || path.join(path.dirname(PROJECT_PATH), "cg_projects");
if (!fs.existsSync(PROJECTS_DIR)) fs.mkdirSync(PROJECTS_DIR, { recursive: true });
function safeName(name) {
  return String(name || "").replace(/[^\w가-힣\-. ]/g, "").trim().slice(0, 80);
}

const app = express();
app.use(express.json({ limit: "50mb" })); // html/css 가 길 수 있어 넉넉히

app.get("/api/meta", (req, res) => {
  res.json({ projectPath: PROJECT_PATH });
});

app.get("/api/project", (req, res) => {
  fs.readFile(PROJECT_PATH, "utf8", (err, data) => {
    if (err) {
      if (err.code === "ENOENT") return res.json({ bg: "transparent", pages: [] });
      return res.status(500).json({ error: err.message });
    }
    try {
      res.json(JSON.parse(data));
    } catch (e) {
      res.status(500).json({ error: "project.json 파싱 실패: " + e.message });
    }
  });
});

app.post("/api/project", (req, res) => {
  const project = req.body;
  if (!project || !Array.isArray(project.pages)) {
    return res.status(400).json({ error: "invalid project (pages 배열 필요)" });
  }
  fs.writeFile(PROJECT_PATH, JSON.stringify(project, null, 2), "utf8", (err) => {
    if (err) return res.status(500).json({ error: err.message });
    res.json({ ok: true });
  });
});

app.get("/api/projects", (req, res) => {
  fs.readdir(PROJECTS_DIR, (err, files) => {
    if (err) return res.json([]);
    const list = files.filter((f) => f.endsWith(".json")).map((f) => {
      const stat = fs.statSync(path.join(PROJECTS_DIR, f));
      return { name: f.replace(/\.json$/, ""), mtime: stat.mtimeMs };
    }).sort((a, b) => b.mtime - a.mtime);
    res.json(list);
  });
});

app.get("/api/projects/:name", (req, res) => {
  const name = safeName(req.params.name);
  fs.readFile(path.join(PROJECTS_DIR, name + ".json"), "utf8", (err, data) => {
    if (err) return res.status(404).json({ error: "찾을 수 없음" });
    try { res.json(JSON.parse(data)); } catch (e) { res.status(500).json({ error: e.message }); }
  });
});

app.post("/api/projects/:name", (req, res) => {
  const name = safeName(req.params.name);
  if (!name) return res.status(400).json({ error: "이름을 입력하세요" });
  const project = req.body;
  if (!project || !Array.isArray(project.pages)) return res.status(400).json({ error: "invalid project" });
  fs.writeFile(path.join(PROJECTS_DIR, name + ".json"), JSON.stringify(project, null, 2), "utf8", (err) => {
    if (err) return res.status(500).json({ error: err.message });
    res.json({ ok: true, name });
  });
});

app.get("/api/templates", (req, res) => {
  fs.readFile(TEMPLATES_PATH, "utf8", (err, data) => {
    if (err) {
      if (err.code === "ENOENT") return res.json([]);
      return res.status(500).json({ error: err.message });
    }
    try {
      res.json(JSON.parse(data));
    } catch (e) {
      res.status(500).json({ error: "templates.json 파싱 실패: " + e.message });
    }
  });
});

app.post("/api/templates", (req, res) => {
  const templates = req.body;
  if (!Array.isArray(templates)) return res.status(400).json({ error: "invalid templates (배열 필요)" });
  fs.writeFile(TEMPLATES_PATH, JSON.stringify(templates, null, 2), "utf8", (err) => {
    if (err) return res.status(500).json({ error: err.message });
    res.json({ ok: true });
  });
});

// ---- 송출 제어 (매뉴얼 20~21장: 페이지 이동, 수동/자동-반복/무한 송출) ----
// cef_mpp 자체는 next/prev/goto N/quit 만 받는 "리모컨"이라, 자동(반복) 타이머는
// 여기(Node)서 돌리면서 goto 를 순서대로 보낸다 — cef_mpp(C++) 는 건드리지 않는다.
const udpSock = dgram.createSocket("udp4");
function sendCg(cmd) {
  const buf = Buffer.from(cmd);
  udpSock.send(buf, 0, buf.length, CG_CTL_PORT, "127.0.0.1");
}

const autoState = { running: false, start: 1, end: 1, page: 1, intervalSec: 5, repeat: 1, infinite: false, cyclesDone: 0 };
let autoTimer = null;

function stopAuto() {
  if (autoTimer) clearInterval(autoTimer);
  autoTimer = null;
  autoState.running = false;
}

function advanceAuto() {
  autoState.page++;
  if (autoState.page > autoState.end) {
    autoState.page = autoState.start;
    autoState.cyclesDone++;
    if (!autoState.infinite && autoState.cyclesDone >= autoState.repeat) {
      sendCg(`goto ${autoState.page}`);
      stopAuto();
      return;
    }
  }
  sendCg(`goto ${autoState.page}`);
}

app.post("/api/control/cmd", (req, res) => {
  const { cmd } = req.body || {};
  if (!cmd || !/^(next|prev|quit|goto \d+)$/.test(cmd)) return res.status(400).json({ error: "invalid cmd" });
  if (cmd !== "quit") stopAuto(); // 수동 조작하면 자동송출은 중단(매뉴얼상 수동/자동은 배타적 모드)
  sendCg(cmd);
  if (/^goto (\d+)$/.test(cmd)) autoState.page = Number(cmd.split(" ")[1]);
  res.json({ ok: true });
});

app.post("/api/control/auto/start", (req, res) => {
  const { start, end, intervalSec, repeat, infinite } = req.body || {};
  if (!(start >= 1) || !(end >= start)) return res.status(400).json({ error: "start/end 페이지 범위 확인" });
  stopAuto();
  Object.assign(autoState, {
    running: true, start, end, page: start,
    intervalSec: Math.max(1, intervalSec || 5),
    repeat: Math.max(1, repeat || 1),
    infinite: !!infinite,
    cyclesDone: 0,
  });
  sendCg(`goto ${start}`);
  autoTimer = setInterval(advanceAuto, autoState.intervalSec * 1000);
  res.json({ ok: true, autoState });
});

app.post("/api/control/auto/stop", (req, res) => {
  stopAuto();
  res.json({ ok: true });
});

app.get("/api/control/status", (req, res) => {
  fs.readFile(CG_LOG, "utf8", (err, data) => {
    const running = require("child_process").execSync("pgrep -x cef_mpp || true").toString().trim().length > 0;
    let lastStat = null, lastCg = null;
    if (!err) {
      const lines = data.split("\n");
      for (let i = lines.length - 1; i >= 0 && (!lastStat || !lastCg); i--) {
        if (!lastStat && lines[i].startsWith("[stat]")) lastStat = lines[i];
        if (!lastCg && lines[i].startsWith("[cg]")) lastCg = lines[i];
      }
    }
    res.json({ running, lastStat, lastCg, auto: autoState });
  });
});

app.use(express.static(path.join(__dirname, "..", "client", "dist")));

app.listen(PORT, "0.0.0.0", () => {
  console.log(`[cgeditor] http://0.0.0.0:${PORT}  project=${PROJECT_PATH}`);
});
