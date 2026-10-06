@echo off
REM 一键编译 demo 模块
REM 注意：编辑器需关闭，否则 dll 被占用会失败
REM -NoUBA：本工程环境下 UBA 会导致 PCH 访问冲突，禁用后稳定
REM 目标用 demoEditor：编辑器里跑必须编编辑器 DLL（demo 是独立 Game 目标，两者不同）
set ENGINE_DIR=K:\UE_5.7\Engine
set PROJECT=K:\selfgame\demo\demo.uproject
set LOG=K:\selfgame\demo\build_log.txt

call "%ENGINE_DIR%\Build\BatchFiles\Build.bat" demoEditor Win64 Development -Project="%PROJECT%" -WaitMutex -FromMsBuild -NoUBA > "%LOG%" 2>&1

echo.
echo ==== 编译结果 ====
findstr /C:"Result:" /C:"error C" /C:"Error:" "%LOG%"
echo.
echo 完整日志: %LOG%
pause
