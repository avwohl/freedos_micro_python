@echo off
rem ============================================================
rem DOSBox-X autoexec for the MicroPython network test rig.
rem
rem Loads the Crynwr ne2000 packet driver at INT 0x60 (matches
rem pktdrv_uc386dos.c's IVT scan target), then runs MP.EXE under
rem the bundled DOS extender. SLIRP's DHCP server hands out
rem 10.0.2.15, same lease the dos_emu sim does, so the
rem MicroPython REPL's DHCP probe should succeed identically in
rem both environments.
rem
rem All output is teed into RIG.LOG so run.sh can read it after
rem DOSBox-X exits in -silent mode.
rem ============================================================

echo --- DOSBox-X test rig --- > RIG.LOG

if not exist NE2000.COM goto :baseline
echo Loading Crynwr ne2000 packet driver at INT 0x60 ... >> RIG.LOG
NE2000 0x60 9 0x300 >> RIG.LOG
goto :baseline

:no_pktdrv
echo NE2000.COM not found in C: >> RIG.LOG

:baseline
rem ECHOTEST.EXE is a minimal printf-only smoke (built from
rem addons/gnu/echo via exe.py). Establishes "PMODE/W INT 21h
rem AH=40 passthrough reaches RIG.LOG" before we try MP.EXE.
rem If this baseline doesn't print, the issue is the rig itself
rem (DOSBox-X config, autoexec redirect, PMODE/W bridge); if it
rem does print, MP.EXE-specific runtime is the next thing to dig.
if not exist ECHOTEST.EXE goto :run_mp
echo --- before-echo-baseline --- >> RIG.LOG
ECHOTEST.EXE hello dos rig >> RIG.LOG
echo --- after-echo-baseline --- >> RIG.LOG

:run_mp
if not exist MP.EXE goto :no_exe
echo --- before-mp marker --- >> RIG.LOG
if exist SCRIPT.PY goto :run_scripted
echo Running MP.EXE interactively ... >> RIG.LOG
MP.EXE >> RIG.LOG
goto :after_mp

:run_scripted
echo Running MP.EXE with SCRIPT.PY ... >> RIG.LOG
rem Append MP.EXE's stdout directly to RIG.LOG so its output is
rem visible even when MP.EXE hangs and dosbox-x gets SIGKILLed
rem before the autoexec can run a `type` step. The previous
rem "MP.EXE > MP_OUT.TXT; type MP_OUT.TXT >> RIG.LOG" pattern
rem hid all of MP's output when the timeout fired (which is
rem exactly the case we need diagnostics for). The trade-off is
rem that the "after-mp marker" line appears only when MP.EXE
rem actually returns; if it doesn't, RIG.LOG ends with whatever
rem bytes MP printed last before the kill.
MP.EXE < SCRIPT.PY >> RIG.LOG
goto :after_mp

:after_mp
echo --- after-mp marker --- >> RIG.LOG
goto :done

:no_exe
echo MP.EXE not found in C: >> RIG.LOG
echo Build it on a Linux box via addons/harness/exe.py >> RIG.LOG

:done
echo --- Test rig finished --- >> RIG.LOG
