@echo off
rem Launch Cinema 4D 2026 without the machine-wide OCIO env var.
rem The system OCIO (ACES 1.2 config) breaks the C4D 2026 viewport for new
rem documents ("failed instantiating monitor color spaces"), leaving it blank.
set OCIO=
start "" "C:\Program Files\Maxon Cinema 4D 2026\Cinema 4D.exe" %*
