@echo off
echo Cleaning up Velix processes on Windows...

:: Kill processes holding ports 5170 to 5174
FOR /L %%P IN (5170,1,5174) DO (
    FOR /F "tokens=5" %%A IN ('netstat -a -n -o ^| findstr :%%P') DO (
        if "%%A" NEQ "0" (
            echo Killing process on port %%P (PID: %%A)
            taskkill /F /PID %%A >nul 2>&1
        )
    )
)

echo Stopping velix-related binaries...
taskkill /F /IM integration_kernel.exe >nul 2>&1
taskkill /F /IM chat_handler.exe >nul 2>&1
taskkill /F /IM velix.exe >nul 2>&1

echo Cleanup complete.
