import { objectToHtml } from "./objectRender";
import { PROJECT_W, PROJECT_H } from "./Canvas";

// 매뉴얼 15/21장 "페이지 미리 보기 창": 텍스트 설명 대신 objects를 실제로 축소 렌더링.
const THUMB_W = 84;
const THUMB_H = Math.round((THUMB_W * PROJECT_H) / PROJECT_W);
export { THUMB_W, THUMB_H };

export default function PageThumbnail({ page }) {
  const scale = THUMB_W / PROJECT_W;
  const objects = page.objects || [];

  return (
    <div className="page-thumb" style={{ width: THUMB_W, height: THUMB_H }}>
      <div className="page-thumb-stage" style={{ width: PROJECT_W, height: PROJECT_H, transform: `scale(${scale})` }}>
        {objects.map((o) => (
          <div key={o.id} style={{ position: "absolute" }} dangerouslySetInnerHTML={{ __html: objectToHtml(o) }} />
        ))}
      </div>
      {page.video && <span className="page-thumb-video" title="영상 배경">🎬</span>}
    </div>
  );
}
