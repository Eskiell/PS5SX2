@echo off
setlocal
rem Stores (or replaces) the Discord webhook in the PS5SX2 log relay, then lists the relay's secrets (names
rem only) so you can see it went in. Needs the Cloudflare login that deploy.cmd did.
cd /d "%~dp0"
echo.
echo Paste the Discord webhook URL (right-click or Ctrl+V; it may not show as you paste), then press Enter.
call npx --yes wrangler@4 secret put DISCORD_WEBHOOK_URL
if errorlevel 1 goto fail
echo.
call npx --yes wrangler@4 secret list
echo.
echo Done if DISCORD_WEBHOOK_URL is in the list above. Tell Claude.
pause
exit /b 0

:fail
echo.
echo That failed: the reason is in the lines above.
pause
exit /b 1
