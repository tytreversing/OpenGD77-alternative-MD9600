@echo off
del /f /q OpenMD9600RUS.zip
7z.exe a -tzip -mx9 -r0 OpenMD9600RUS.zip MD9600_HW_V1\OpenMD9600_V1HW_RUS.bin MD9600_HW_V2\OpenMD9600_V2HW_RUS.bin MD9600_HW_V4\OpenMD9600_V4HW_RUS.bin MD9600_HW_V5\OpenMD9600_V5HW_RUS.bin

