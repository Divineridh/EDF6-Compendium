@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
if not exist build mkdir build
cl /nologo /std:c++17 /EHsc /O2 /MT /LD /W3 ^
   /D "IMGUI_DISABLE_OBSOLETE_FUNCTIONS" ^
   /I "deps\EDF6Plugins" /I "deps\imgui" /I "deps\minhook\include" ^
   src\compendium.cpp src\overlay.cpp src\savedata.cpp ^
   deps\imgui\imgui.cpp deps\imgui\imgui_draw.cpp deps\imgui\imgui_tables.cpp deps\imgui\imgui_widgets.cpp ^
   deps\imgui\backends\imgui_impl_dx11.cpp deps\imgui\backends\imgui_impl_win32.cpp ^
   deps\minhook\src\buffer.c deps\minhook\src\hook.c deps\minhook\src\trampoline.c deps\minhook\src\hde\hde64.c ^
   /Fo:build\ /Fe:build\EDF6Compendium.dll ^
   /link d3d11.lib dxgi.lib user32.lib gdi32.lib /IMPLIB:build\EDF6Compendium.lib
