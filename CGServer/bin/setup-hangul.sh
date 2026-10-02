#!/bin/bash
# Debian 11(XFCE, X11)에서 한글 입력(fcitx5 + fcitx5-hangul)을 설정한다. 단말에서 한 번만 실행하면 됨(여러 번 실행해도 안전).
#   ./setup-hangul.sh
# - 시스템 로케일(LANG)은 바꾸지 않는다.
# - 패키지 설치에 sudo 가 필요하다(비밀번호 없이 sudo 가능해야 함).
# - 끝난 뒤 **로그아웃 후 다시 로그인**(또는 재부팅)하면 세션 전체가 새 입력기 환경이 된다. 그 전에는 아래 Chromium 런처로 열면 바로 한글이 된다.
# 설명서: readme-env.md 의 "한글 입력" 절.
set -eu
[ "$(id -u)" != 0 ] || { echo "일반 사용자(pi)로 실행하세요 (설정이 사용자 홈에 저장됨)"; exit 1; }

echo "[1/6] 패키지 설치 (fcitx5, 한글 엔진, GTK/Qt 연동, 설정 도구, 나눔 글꼴)"
sudo apt-get update -qq
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
  fcitx5 fcitx5-hangul fcitx5-frontend-gtk2 fcitx5-frontend-gtk3 fcitx5-frontend-qt5 fcitx5-config-qt fcitx5-module-xorg im-config fonts-nanum

echo "[2/6] 입력기를 fcitx5 로 선택 (로그인할 때 GTK_IM_MODULE/QT_IM_MODULE/XMODIFIERS=fcitx 와 fcitx5 자동 시작)"
im-config -n fcitx5

echo "[3/6] 입력 방식과 전환 키 설정"
mkdir -p ~/.config/fcitx5/conf ~/.config/autostart
# 영문(us)과 한글(hangul) 두 가지, 시작은 영문
cat > ~/.config/fcitx5/profile <<'CFG'
[Groups/0]
Name=Default
Default Layout=us
DefaultIM=keyboard-us

[Groups/0/Items/0]
Name=keyboard-us
Layout=

[Groups/0/Items/1]
Name=hangul
Layout=

[GroupOrder]
0=Default
CFG
# 한/영 전환 키: 키보드의 한/영 키, Shift+Space, Ctrl+Space
cat > ~/.config/fcitx5/config <<'CFG'
[Hotkey]
EnumerateWithTriggerKeys=True
AltTriggerKeys=
EnumerateForwardKeys=
EnumerateBackwardKeys=
EnumerateSkipFirst=False

[Hotkey/TriggerKeys]
0=Hangul
1=Shift+space
2=Control+space
CFG
# 한글 자판: 두벌식
cat > ~/.config/fcitx5/conf/hangul.conf <<'CFG'
Keyboard=2
HanjaMode=True
CFG
# 로그인 때 확실히 뜨도록 자동 시작 항목도 둔다(이미 떠 있으면 두 번째는 알아서 종료됨)
cat > ~/.config/autostart/fcitx5.desktop <<'CFG'
[Desktop Entry]
Type=Application
Name=Fcitx 5
Comment=한글 입력기
Exec=fcitx5 -d
X-GNOME-Autostart-enabled=true
CFG

echo "[4/6] Chromium 런처를 한글 입력 환경으로 (바탕화면 아이콘 + 응용 프로그램 메뉴)"
# 지금 떠 있는 데스크탑 세션은 ibus 환경으로 시작됐을 수 있어서, 런처가 직접 입력기 환경변수를 넣는다. 재로그인 후에도 문제 없음.
LAUNCHER=$(mktemp)
cat > "$LAUNCHER" <<'CFG'
[Desktop Entry]
Version=1.0
Type=Application
Name=Chromium Browser
GenericName=Web Browser
Comment=한글 입력(fcitx5) 환경으로 Chromium 실행
Exec=env GTK_IM_MODULE=fcitx QT_IM_MODULE=fcitx XMODIFIERS=@im=fcitx /usr/bin/chromium-browser %U
Icon=chromium-browser
Terminal=false
StartupNotify=true
Categories=Network;WebBrowser;
MimeType=text/html;text/xml;application/xhtml+xml;x-scheme-handler/http;x-scheme-handler/https;
CFG
mkdir -p ~/.local/share/applications ~/.config/cg-backup
cp "$LAUNCHER" ~/.local/share/applications/chromium-browser.desktop
if [ -d ~/Desktop ]; then
  # 바탕화면 아이콘은 원래 시스템 런처로 가는 링크였음 -> 원본은 백업 폴더에 한 번만 보관
  [ -f ~/.config/cg-backup/Desktop-chromium-browser.desktop.orig ] || { [ -f ~/Desktop/chromium-browser.desktop ] && cp ~/Desktop/chromium-browser.desktop ~/.config/cg-backup/Desktop-chromium-browser.desktop.orig; }
  cp "$LAUNCHER" ~/Desktop/chromium-browser.desktop
  chmod +x ~/Desktop/chromium-browser.desktop
  gio set ~/Desktop/chromium-browser.desktop metadata::trusted true 2>/dev/null || true
fi
chmod +x ~/.local/share/applications/chromium-browser.desktop
rm -f "$LAUNCHER"
update-desktop-database ~/.local/share/applications 2>/dev/null || true

echo "[5/6] 오른쪽 Alt 를 한/영 키로 (미국식 키보드에는 한/영 키가 없음)"
# 한/영 키가 없는 키보드(예: Logitech K370s 미국 배열)용: 오른쪽 Alt = Hangul(한/영), 오른쪽 Ctrl = Hangul_Hanja(한자).
# fcitx5 의 전환 키 목록에 Hangul 이 들어 있어서 이것만 연결하면 된다. 로그인마다 적용되도록 자동 시작 항목으로 둔다.
cat > ~/.config/autostart/xkb-hangul.desktop <<'CFG'
[Desktop Entry]
Type=Application
Name=Korean keys (Right Alt = Hangul)
Comment=오른쪽 Alt 를 한/영, 오른쪽 Ctrl 을 한자 키로
Exec=setxkbmap -option korean:ralt_hangul,korean:rctrl_hanja
X-GNOME-Autostart-enabled=true
CFG
[ -n "${DISPLAY:-}" ] && setxkbmap -option korean:ralt_hangul,korean:rctrl_hanja 2>/dev/null || true

echo "[6/6] 지금 세션에서도 fcitx5 시작(이미 떠 있으면 건너뜀)"
if [ -n "${DISPLAY:-}" ]; then
  if pgrep -u "$(id -u)" -x fcitx5 >/dev/null; then
    echo "  이미 실행 중"
  else
    _sp=$(pgrep -u "$(id -u)" -x xfce4-session | head -1)
    [ -z "${DBUS_SESSION_BUS_ADDRESS:-}" ] && [ -n "$_sp" ] && export DBUS_SESSION_BUS_ADDRESS=$(tr '\0' '\n' < "/proc/$_sp/environ" 2>/dev/null | sed -n 's/^DBUS_SESSION_BUS_ADDRESS=//p')
    GTK_IM_MODULE=fcitx QT_IM_MODULE=fcitx XMODIFIERS=@im=fcitx setsid nohup fcitx5 -d >/dev/null 2>&1 < /dev/null || true
    sleep 3
    pgrep -u "$(id -u)" -x fcitx5 >/dev/null && echo "  시작됨" || echo "  시작 실패(로그아웃 후 다시 로그인하면 자동으로 뜸)"
  fi
else
  echo "  DISPLAY 가 없어 건너뜀(로그인하면 자동으로 뜸)"
fi

cat <<'MSG'

완료. 사용법:
  - 한글 전환: 오른쪽 Alt(한/영 키가 있는 키보드는 그 키), Shift+Space, Ctrl+Space (시작은 영문). 오른쪽 Ctrl 은 한자 키.
  - 브라우저(Chromium)는 먼저 열려 있는 창을 모두 닫고, 바탕화면의 Chromium 아이콘으로 여세요(이미 떠 있는 Chromium 에 붙으면 예전 환경이라 한글이 안 됩니다).
  - 로그아웃 후 다시 로그인하면 모든 프로그램에서 한글이 됩니다.
MSG
