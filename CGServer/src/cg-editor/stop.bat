@echo off
chcp 65001 >nul
setlocal EnableDelayedExpansion
rem cg-editor 종료 (start.bat 가 만든 .run\cg-editor.pid 의 프로세스와 그 하위 프로세스를 종료)
rem   stop.bat
cd /d "%~dp0"
set PIDF=.run\cg-editor.pid
set FPIDF=.run\cg-files.pid

rem 파일 서버 종료 (에디터 pid 파일이 없어도 정리)
if exist "%FPIDF%" (
  set /p FPID=<"%FPIDF%"
  taskkill /PID !FPID! /T /F >nul 2>nul
  del "%FPIDF%" >nul 2>nul
  echo 파일 서버 종료
)

if not exist "%PIDF%" (
  echo 실행 중이 아닙니다 ^(pid 파일 없음^)
  exit /b 0
)
set /p PID=<"%PIDF%"
tasklist /fi "PID eq !PID!" 2>nul | find "!PID!" >nul
if errorlevel 1 (
  echo 이미 종료되어 있습니다 ^(오래된 pid !PID! 정리^)
  del "%PIDF%" >nul 2>nul
  exit /b 0
)

rem /T: 하위 프로세스(node 등)까지, /F: 강제
taskkill /PID !PID! /T /F >nul
del "%PIDF%" >nul 2>nul
echo 종료했습니다
endlocal
