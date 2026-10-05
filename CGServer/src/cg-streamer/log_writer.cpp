#include "log_writer.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <thread>

namespace {

std::string DateStr(const tm& t) {
  char b[64];
  snprintf(b, sizeof b, "%04d-%02d-%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
  return b;
}

// <prefix>-YYYY-MM-DD.log 중 오늘 기준 keep_days 일보다 오래된 것을 지운다 (날짜 문자열 비교).
void RemoveOld(const std::string& dir, const std::string& prefix, int keep_days) {
  const time_t limit = time(nullptr) - (time_t)keep_days * 86400;
  tm lt;
  localtime_r(&limit, &lt);
  const std::string oldest = DateStr(lt);
  DIR* d = opendir(dir.c_str());
  if (!d) return;
  const std::string head = prefix + "-";
  while (dirent* e = readdir(d)) {
    const std::string n = e->d_name;
    if (n.size() != head.size() + 10 + 4 || n.compare(0, head.size(), head) != 0 || n.compare(n.size() - 4, 4, ".log") != 0) continue;
    if (n.substr(head.size(), 10) < oldest) unlink((dir + "/" + n).c_str());
  }
  closedir(d);
}

void Run(int rfd, std::string dir, std::string prefix, int keep_days) {
  FILE* fp = nullptr;
  std::string cur;
  std::string pending;   // 줄바꿈이 아직 안 온 조각
  char buf[8192];
  auto emit = [&](const std::string& line) {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const time_t sec = system_clock::to_time_t(now);
    const int ms = (int)(duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000);
    tm t;
    localtime_r(&sec, &t);
    const std::string day = DateStr(t);
    if (day != cur || !fp) {   // 첫 줄이거나 날짜가 바뀜 -> 새 파일
      if (fp) fclose(fp);
      cur = day;
      fp = fopen((dir + "/" + prefix + "-" + day + ".log").c_str(), "a");
      RemoveOld(dir, prefix, keep_days);
    }
    if (!fp) return;
    fprintf(fp, "%s %02d:%02d:%02d.%03d %s\n", day.c_str(), t.tm_hour, t.tm_min, t.tm_sec, ms, line.c_str());
    fflush(fp);
  };
  for (;;) {
    const ssize_t n = read(rfd, buf, sizeof buf);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    pending.append(buf, (size_t)n);
    size_t pos;
    while ((pos = pending.find('\n')) != std::string::npos) {
      std::string line = pending.substr(0, pos);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      emit(line);
      pending.erase(0, pos + 1);
    }
  }
  if (!pending.empty()) emit(pending);
  if (fp) fclose(fp);
}

}  // namespace

bool LogWriterStart(const std::string& dir, const std::string& prefix, int keep_days) {
  if (isatty(STDOUT_FILENO) || getenv("CG_LOG_STDOUT")) return false;
  mkdir(dir.c_str(), 0755);
  int fds[2];
  if (pipe(fds) != 0) return false;
  fflush(stdout);
  fflush(stderr);
  if (dup2(fds[1], STDOUT_FILENO) < 0 || dup2(fds[1], STDERR_FILENO) < 0) { close(fds[0]); close(fds[1]); return false; }
  close(fds[1]);
  setvbuf(stdout, nullptr, _IOLBF, 0);   // 파이프는 기본이 전체 버퍼링 -> 줄 단위로 바로 내보냄
  std::thread(Run, fds[0], dir, prefix, keep_days).detach();
  // 종료 직전에 찍은 마지막 줄이 파이프에 남은 채 사라지지 않도록 잠깐 비워 줄 시간을 준다
  atexit([] { fflush(stdout); fflush(stderr); usleep(100 * 1000); });
  return true;
}
