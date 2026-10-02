@echo off
rem Installs the SudoVDA virtual display driver. Exits with 1 when a step fails.
pushd %~dp0

set "CERTUTIL=certutil"
where certutil >nul 2>&1 || set "CERTUTIL=%SystemRoot%\System32\certutil.exe"

echo ================
echo Installing cert for the SudoVDA driver...

%CERTUTIL% -addstore -f root "sudovda.cer"
if errorlevel 1 goto failed
%CERTUTIL% -addstore -f TrustedPublisher "sudovda.cer"
if errorlevel 1 goto failed

echo ================
echo Removing the old driver... It's OK to show an error if you're installing the driver for the first time.

nefconc.exe --remove-device-node --hardware-id root\sudomaker\sudovda --class-guid "4D36E968-E325-11CE-BFC1-08002BE10318"

echo ================
echo Installing the new driver...

nefconc.exe --create-device-node --class-name Display --class-guid "4D36E968-E325-11CE-BFC1-08002BE10318" --hardware-id root\sudomaker\sudovda
if errorlevel 1 goto failed
nefconc.exe --install-driver --inf-path "SudoVDA.inf"
rem 3010: installed, a restart completes it
if "%ERRORLEVEL%"=="3010" (
    echo A restart of Windows completes the driver installation.
    goto done
)
if errorlevel 1 goto failed

:done
echo ================
echo Done!
popd
exit /b 0

:failed
echo ================
echo The SudoVDA driver installation failed (exit code %ERRORLEVEL%).
popd
exit /b 1
