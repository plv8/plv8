powershell -File generate_upgrade.ps1
mkdir vendor
cd vendor
powershell -File ..\wget_depot_tools.ps1
mkdir depot_tools
cd depot_tools
unzip ..\depot_tools.zip
cd ..
set path=%~dp0vendor\depot_tools;%path%
set DEPOT_TOOLS_WIN_TOOLCHAIN=0
set SKIP_V8_GYP_ENV=1
set GYP_CHROMIUM_NO_ACTION=1
call fetch v8
cd v8
call git checkout 8.6.405
call gclient sync
tools\dev\v8gen.py x64.release -- is_component_build=true v8_use_snapshot=true v8_use_external_startup_data=false v8_enable_i18n_support=false
call ninja -C out.gn\x64.release d8
cd ..\..
