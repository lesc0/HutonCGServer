// 파일 서버(scripts/files-server.mjs) 클라이언트.
//   이미지·영상·음성은 bin/media 에서만 가져오고, 프로젝트 파일은 bin/project 에 저장한다.
//   프로젝트 안의 미디어 src 는 "../media/<파일>" (project 폴더 기준 상대경로)로 기록한다.
//   실행엔진(cg-streamer)은 이 경로를 project 파일 위치 기준으로 그대로 읽는다.
import type {Project} from './model';

export const MEDIA_REF = '../media/';
export const FILES_PORT = 8081;   // files-server.mjs 의 기본 포트 (CG_FILES_PORT 를 바꾸면 여기도 맞춰야 함)

export type MediaFile = {name: string; kind: 'image' | 'video' | 'audio'; size: number; updated: number};
export type ProjectFile = {name: string; size: number; updated: number};

// 에디터를 연 호스트와 같은 호스트의 파일 서버 (localhost 로 열었으면 localhost, LAN 주소면 그 주소)
export const filesBase = () => `${location.protocol}//${location.hostname}:${FILES_PORT}`;

// 저장된 src 를 브라우저가 읽을 수 있는 URL 로 바꾼다 (../media/x.png -> 파일 서버 주소)
export function mediaUrl(src: string | undefined): string {
  if (!src) return '';
  return src.startsWith(MEDIA_REF) ? `${filesBase()}/media/${encodeURIComponent(src.slice(MEDIA_REF.length))}` : src;
}
export const mediaRef = (name: string) => MEDIA_REF + name;

async function call(path: string, init?: RequestInit) {
  let r: Response;
  try { r = await fetch(filesBase() + path, {cache: 'no-store', ...init}); }
  catch { throw new Error('파일 서버에 연결하지 못했습니다. start.sh(또는 start.bat)로 파일 서버를 함께 실행했는지 확인하세요.'); }
  let data: any;
  try { data = await r.json(); } catch { throw new Error('파일 서버 응답을 읽지 못했습니다.'); }
  if (!r.ok) throw new Error(data?.error || '파일 서버 요청에 실패했습니다.');
  return data;
}

export const listMedia = async (kind?: MediaFile['kind']): Promise<{dir: string; files: MediaFile[]}> => call('/media' + (kind ? '?kind=' + kind : ''));
export const listProjectFiles = async (): Promise<{dir: string; projects: ProjectFile[]}> => call('/projects');
export const loadProjectFile = async (name: string): Promise<unknown> => call('/projects/' + encodeURIComponent(name));
export const saveProjectFile = async (name: string, project: Project): Promise<{name: string; updated: number; extractedMedia: number; project: Project}> =>
  call('/projects/' + encodeURIComponent(name), {method: 'PUT', headers: {'Content-Type': 'application/json'}, body: JSON.stringify(project)});
