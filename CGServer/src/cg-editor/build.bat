@echo off
chcp 65001 >nul
setlocal
rem cg-editor 빌드: 의존성 설치(필요할 때) + 프로덕션 빌드(dist\)
rem   build.bat            node_modules 가 없으면 npm ci 후 빌드
rem   build.bat /install   node_modules 를 항상 npm ci 로 새로 설치한 뒤 빌드
rem   build.bat /check     빌드 전에 타입 검사(tsc --noEmit)도 실행
cd /d "%~dp0"

set INSTALL=0
set CHECK=0
:args
if "%~1"=="" goto argsdone
if /i "%~1"=="/install" set INSTALL=1& shift & goto args
if /i "%~1"=="--install" set INSTALL=1& shift & goto args
if /i "%~1"=="/check" set CHECK=1& shift & goto args
if /i "%~1"=="--check" set CHECK=1& shift & goto args
echo 알 수 없는 옵션: %~1
exit /b 1
:argsdone

where node >nul 2>nul || (echo node 가 없습니다 ^(22.13 이상 필요^) & exit /b 1)
node -e "const [a,b]=process.versions.node.split('.').map(Number);process.exit(a>22||(a===22&&b>=13)?0:1)"
if errorlevel 1 (
  for /f %%v in ('node -v') do echo Node.js 22.13 이상이 필요합니다 ^(현재 %%v^)
  exit /b 1
)

if "%INSTALL%"=="1" goto doinstall
if not exist node_modules goto doinstall
goto afterinstall
:doinstall
echo [build] npm ci
call npm ci
if errorlevel 1 exit /b 1
:afterinstall

if "%CHECK%"=="1" (
  echo [build] tsc --noEmit
  call npx tsc --noEmit
  if errorlevel 1 exit /b 1
)

echo [build] npm run build
call npm run build
if errorlevel 1 exit /b 1
echo [build] 완료: dist\  ^(실행: start.bat /prod^)
endlocal
