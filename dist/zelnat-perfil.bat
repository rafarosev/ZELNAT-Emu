@echo off
cd /d "%~dp0"
set EDEN_NX2_GPU_PROFILE=1
set EDEN_NX2_GUEST_THREAD_STATS=1
set EDEN_NX2_STUTTER_TRACE=1
set EDEN_NX2_LAZY_COMMIT=1
start "" "zelnat.exe"
