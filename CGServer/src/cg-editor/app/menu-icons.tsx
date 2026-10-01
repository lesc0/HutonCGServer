// 메인 메뉴(펼침) 항목별 아이콘. 항목 이름(label)으로 찾고, 없으면 아이콘 자리만 비워 정렬을 맞춘다.
import type {ComponentType} from 'react';
import {FilePlus,FilePlus2,FolderOpen,Save,SaveAll,ImageDown,Undo2,Redo2,Copy,ClipboardPaste,Scissors,Trash2,BoxSelect,Scan,Group,Ungroup,Frame,Grid2X2,Magnet,Maximize,Play,Type,Square,Circle,ImagePlus,Film,Cast,Music,Clock,Timer,Lock,LockOpen,Link,RotateCcw,ExternalLink,HelpCircle} from 'lucide-react';

type Icon=ComponentType<{size?:number;strokeWidth?:number}>;
const icons:Record<string,Icon>={
  // 파일
  '새 프로젝트':FilePlus,'프로젝트 열기':FolderOpen,'프로젝트 저장':Save,'프로젝트 다른 이름으로 저장':SaveAll,'PNG 내보내기':ImageDown,
  // 편집
  '실행 취소':Undo2,'다시 실행':Redo2,'복사':Copy,'붙여넣기':ClipboardPaste,'잘라내기':Scissors,'삭제':Trash2,'모두 선택':BoxSelect,'텍스트 영역 맞춤':Scan,'그룹':Group,'그룹 해제':Ungroup,
  // 보기
  'Safe Area':Frame,'Grid':Grid2X2,'Snap':Magnet,'화면 맞춤':Maximize,'미리보기':Play,
  // 삽입
  '새 페이지':FilePlus2,'문자':Type,'사각형':Square,'원':Circle,'이미지':ImagePlus,'동영상':Film,'HDMI 입력(라이브)':Cast,'음성':Music,'시계':Clock,'계수기':Timer,
  // 도구
  '객체 잠금':Lock,'객체 락 모두 해제':LockOpen,'Text Link':Link,
  // 창 / 도움말
  '레이아웃 초기화':RotateCcw,'사용 방법':HelpCircle,
};
export function MenuIcon({label}:{label:string}){
  const I=icons[label]||(label.endsWith('별도 패널')?ExternalLink:null);
  return <span className="menuicon" aria-hidden="true">{I&&<I size={16} strokeWidth={1.8}/>}</span>;
}
