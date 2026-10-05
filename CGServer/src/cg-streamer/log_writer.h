// 로그 파일 기록: stdout/stderr 를 파이프로 받아 줄마다 시간("YYYY-MM-DD HH:MM:SS.mmm")을 붙여
// <dir>/<prefix>-YYYY-MM-DD.log 에 남긴다(일별 파일, 자정이 지나면 새 파일, keep_days 보다 오래된 파일은 삭제).
// printf/fprintf(stderr) 와 자식 프로세스(CEF 등)의 출력도 같은 파일로 모인다.
#pragma once
#include <string>

// 터미널(tty)에 연결돼 있으면(직접 실행) 아무것도 하지 않고 false 를 반환한다. CG_LOG_STDOUT=1 이면 항상 건너뜀.
bool LogWriterStart(const std::string& dir, const std::string& prefix, int keep_days);
