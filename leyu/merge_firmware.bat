@echo off
chcp 65001 >nul
echo ----------------------------------------------
echo     ESP32 Firmware Merge Tool
echo ----------------------------------------------

python merge_firmware.py

pause
