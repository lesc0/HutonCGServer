@echo off
chcp 65001 >nul
setlocal EnableDelayedExpansion
rem cg-editor 시작 (백그라운드 실행, PID/로그는 .run\ 에 저장)
rem   start.bat                개발 서버(npm run dev, 기본 포트 5173)
rem   start.bat /prod          빌드 결과(dist\)로 실행(npm start). 먼저 build.bat 필요
rem   start.bat /port 8080     포트 지정
rem   start.bat /host 0.0.0.0   다른 PC 에서도 접속 (기본은 127.0.0.1 전용)
rem   start.bat /fg            백그라운드로 보내지 않고 이 창에서 실행 (Ctrl+C 로 종료)
rem 종료: stop.bat     로그: type .run\cg-editor.log
cd /d "%~dp0"

set MODE=dev
set PORT=
set HOST=
set FG=0
:args
if "%~1"=="" goto argsdone
if /i "%~1"=="/prod" set MODE=prod& shift & goto args
if /i "%~1"=="--prod" set MODE=prod& shift & goto args
if /i "%~1"=="/fg" set FG=1& shift & goto args
if /i "%~1"=="--fg" set FG=1& shift & goto args
if /i "%~1"=="/port" set PORT=%~2& shift & shift & goto args
if /i "%~1"=="--port" set PORT=%~2& shift & shift & goto args
if /i "%~1"=="/host" set HOST=%~2& shift & shift & goto args
if /i "%~1"=="--host" set HOST=%~2& shift & shift & goto args
echo 알 수 없는 옵션: %~1
exit /b 1
:argsdone

if defined PORT (
  echo !PORT!| findstr /r "^[0-9][0-9]*$" >nul || (echo /port 는 숫자여야 합니다 & exit /b 1)
)

if not exist .run mkdir .run
set PIDF=.run\cg-editor.pid
set LOG=.run\cg-editor.log
set FPIDF=.run\cg-files.pid
set FLOG=.run\cg-files.log

if exist "%PIDF%" (
  set /p OLDPID=<"%PIDF%"
  tasklist /fi "PID eq !OLDPID!" 2>nul | find "!OLDPID!" >nul
  if not errorlevel 1 (
    echo 이미 실행 중입니다 ^(pid !OLDPID!^). 먼저 stop.bat
    exit /b 1
  )
  del "%PIDF%" >nul 2>nul
)

if not exist node_modules (
  echo node_modules 가 없습니다. 먼저 build.bat 를 실행하세요.
  exit /b 1
)

rem 환경변수: .env(공통) -> prod 모드면 .env.production 이 덮어씀 -> /port, /host 옵션이 최우선
if exist .env (
  for /f "usebackq eol=# tokens=1,* delims==" %%a in (".env") do set "%%a=%%b"
)
if /i "%MODE%"=="prod" if exist .env.production (
  for /f "usebackq eol=# tokens=1,* delims==" %%a in (".env.production") do set "%%a=%%b"
)
if not defined PORT if defined CG_EDITOR_PORT set PORT=!CG_EDITOR_PORT!
if not defined HOST if defined CG_EDITOR_HOST set HOST=!CG_EDITOR_HOST!

set ARGS=
if /i "%MODE%"=="prod" (
  if not exist dist (
    echo dist\ 가 없습니다. 먼저 build.bat 를 실행하세요.
    exit /b 1
  )
  set CMD=npm start
  if defined PORT set ARGS=!ARGS! --port !PORT!
  if defined HOST set ARGS=!ARGS! --ip !HOST!
) else (
  set CMD=npm run dev
  if defined PORT set ARGS=!ARGS! --port !PORT!
  if defined HOST set ARGS=!ARGS! --host !HOST!
)
if defined ARGS set CMD=!CMD! --!ARGS!

rem 파일 서버(bin\media 목록·스트리밍, bin\project 저장/열기). 포트 8081(CG_FILES_PORT).
rem 에디터를 /host 로 열면 파일 서버도 같은 주소로 연다.
if defined HOST set CG_FILES_HOST=!HOST!
set FRUN=0
if exist "%FPIDF%" (
  set /p FOLD=<"%FPIDF%"
  tasklist /fi "PID eq !FOLD!" 2>nul | find "!FOLD!" >nul
  if not errorlevel 1 set FRUN=1
)
if "!FRUN!"=="1" (
  echo 파일 서버는 이미 실행 중 ^(pid !FOLD!^)
) else (
  powershell -NoProfile -Command "$p = Start-Process -FilePath node.exe -ArgumentList 'scripts/files-server.mjs' -RedirectStandardOutput '%FLOG%' -RedirectStandardError '%FLOG%.err' -WindowStyle Hidden -PassThru; $p.Id | Set-Content -Encoding ascii '%FPIDF%'"
)

if "%FG%"=="1" (
  call !CMD!
  set RC=!errorlevel!
  if exist "%FPIDF%" ( set /p FPID=<"%FPIDF%" & taskkill /PID !FPID! /T /F >nul 2>nul & del "%FPIDF%" >nul 2>nul )
  exit /b !RC!
)

rem 숨김 창으로 띄우고 PID 를 저장 (stop.bat 가 taskkill /T 로 하위 프로세스까지 종료)
powershell -NoProfile -Command "$p = Start-Process -FilePath cmd.exe -ArgumentList '/c', '!CMD! > %LOG% 2>&1' -WindowStyle Hidden -PassThru; $p.Id | Set-Content -Encoding ascii '%PIDF%'"
if errorlevel 1 (
  echo 시작 실패
  exit /b 1
)
ping -n 4 127.0.0.1 >nul

set /p NEWPID=<"%PIDF%"
tasklist /fi "PID eq !NEWPID!" 2>nul | find "!NEWPID!" >nul
if errorlevel 1 (
  echo 시작 실패. 로그:
  type "%LOG%"
  del "%PIDF%" >nul 2>nul
  exit /b 1
)
echo 시작됨 ^(%MODE%, pid !NEWPID!^). 주소는 로그에 표시됩니다:
findstr /i /c:"http://" /c:"Local:" "%LOG%" || echo   ^(아직 준비 중^) type %LOG%
endlocal
