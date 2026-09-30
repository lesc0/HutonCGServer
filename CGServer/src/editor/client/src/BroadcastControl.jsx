import { useEffect, useState } from "react";

// 매뉴얼 20~21장: 송출 제어(이전/다음/이동/정지) + 수동/자동(반복)/무한 송출 설정.
export default function BroadcastControl({ pageCount }) {
  const [status, setStatus] = useState(null);
  const [gotoN, setGotoN] = useState(1);
  const [rangeStart, setRangeStart] = useState(1);
  const [rangeEnd, setRangeEnd] = useState(pageCount || 1);
  const [intervalSec, setIntervalSec] = useState(5);
  const [repeat, setRepeat] = useState(1);
  const [infinite, setInfinite] = useState(true);

  const refresh = () => fetch("/api/control/status").then((r) => r.json()).then(setStatus).catch(() => {});
  useEffect(() => {
    refresh();
    const t = setInterval(refresh, 2000);
    return () => clearInterval(t);
  }, []);

  const cmd = (c) => fetch("/api/control/cmd", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ cmd: c }) }).then(refresh);

  const startAuto = () =>
    fetch("/api/control/auto/start", {
      method: "POST", headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ start: +rangeStart, end: +rangeEnd, intervalSec: +intervalSec, repeat: +repeat, infinite }),
    }).then(refresh);
  const stopAuto = () => fetch("/api/control/auto/stop", { method: "POST" }).then(refresh);

  return (
    <div className="bcast">
      <div className="bcast-title">송출 제어</div>
      <div className="bcast-status">
        {status ? (
          <>
            <span className={status.running ? "dot on" : "dot off"} />
            {status.running ? "cef_mpp 실행 중" : "cef_mpp 없음"}
            {status.lastStat && <div className="bcast-stat">{status.lastStat.replace("[stat] ", "")}</div>}
          </>
        ) : "확인 중..."}
      </div>

      <div className="icon-row">
        <button title="이전" onClick={() => cmd("prev")}>◀</button>
        <button title="다음" onClick={() => cmd("next")}>▶</button>
      </div>
      <div className="props-row">
        <label>이동</label>
        <input type="number" min={1} value={gotoN} onChange={(e) => setGotoN(e.target.value)} />
        <button onClick={() => cmd(`goto ${gotoN}`)}>이동</button>
      </div>

      <div className="props-label" style={{ marginTop: 10 }}>자동(반복) 송출</div>
      <div className="props-row">
        <label>범위</label>
        <input type="number" min={1} value={rangeStart} onChange={(e) => setRangeStart(e.target.value)} style={{ width: 50 }} />
        <span>~</span>
        <input type="number" min={1} value={rangeEnd} onChange={(e) => setRangeEnd(e.target.value)} style={{ width: 50 }} />
      </div>
      <div className="props-row">
        <label>간격(초)</label>
        <input type="number" min={1} value={intervalSec} onChange={(e) => setIntervalSec(e.target.value)} />
      </div>
      <div className="props-row">
        <label>무한</label>
        <input type="checkbox" checked={infinite} onChange={(e) => setInfinite(e.target.checked)} />
        {!infinite && (
          <>
            <label>반복</label>
            <input type="number" min={1} value={repeat} onChange={(e) => setRepeat(e.target.value)} />
          </>
        )}
      </div>
      {status?.auto?.running ? (
        <button className="bcast-stop" onClick={stopAuto}>자동송출 중지 (페이지 {status.auto.page})</button>
      ) : (
        <button className="bcast-start" onClick={startAuto}>자동송출 시작</button>
      )}
    </div>
  );
}
