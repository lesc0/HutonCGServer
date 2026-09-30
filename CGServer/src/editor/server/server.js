// CG 저작 서버: project.json 을 읽고/쓰고, 빌드된 React 클라이언트를 서빙한다.
// cef_mpp 의 --edit(cefQuery save:/load) 와 같은 project.json 을 다뤄서,
// HDMI-1 로컬 저작(cef_mpp --edit)과 원격 브라우저 저작이 같은 파일을 공유한다.
const express = require("express");
const fs = require("fs");
const path = require("path");

const PORT = process.env.CGEDITOR_PORT || 3300;
const PROJECT_PATH = process.env.CGEDITOR_PROJECT ||
  path.join(__dirname, "..", "..", "..", "build", "project.json");

const app = express();
app.use(express.json({ limit: "50mb" })); // html/css 가 길 수 있어 넉넉히

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

app.use(express.static(path.join(__dirname, "..", "client", "dist")));

app.listen(PORT, "0.0.0.0", () => {
  console.log(`[cgeditor] http://0.0.0.0:${PORT}  project=${PROJECT_PATH}`);
});
