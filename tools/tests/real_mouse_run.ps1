# The PC build's raw mouse path (WM_INPUT) end to end: a --windowed test run loads the Quick Save
# (tools/scripts/mp2_real_mouse.txt) while real_mouse.ps1, 54 s in, brings its window to the front and moves the mouse
# by SendInput between screenshots; then tools/stick_shift.py build/mp2-pc/shots 0 3 0.9 measures the turns.
# It takes over the PC's mouse for about 12 s.
#   powershell -ExecutionPolicy Bypass -File tools\tests\real_mouse_run.ps1
$root = (Resolve-Path "$PSScriptRoot\..\..").Path
$args1 = "-ExecutionPolicy Bypass -File `"$root\tools\run_pc.ps1`" -Seconds 72 -Tail 1 -Script `"$root\tools\scripts\mp2_real_mouse.txt`""
$run = Start-Process -FilePath powershell -ArgumentList $args1 -PassThru -WindowStyle Hidden
Start-Sleep -Seconds 54
& "$PSScriptRoot\real_mouse.ps1"
$run.WaitForExit()
