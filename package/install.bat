@echo off
chcp 65001 >nul
rem SA:COOP: installs the ASI loader when the game has none. Run it from the game folder.
cd /d "%~dp0"
rem the game's own files, not its exe: some builds name it otherwise (gta-sa.exe), issue #54
if not exist models\gta3.img (
    echo Здесь нет файлов игры. Распакуйте архив SA-COOP прямо в папку игры и запустите install.bat оттуда.
    pause
    exit /b 1
)
if not exist coop.asi (
    echo Не хватает coop.asi. Распакуйте архив SA-COOP в папку игры целиком.
    pause
    exit /b 1
)
rem The game's own vorbisFile.dll has its name inside in lower case ("vorbisfile.dll"), an ASI loader in its place has not.
rem Ultimate ASI Loader (Widescreen Fix brings it as vorbisFile.dll) needs no vorbisHooked.dll: install.bat took it for the
rem game's library and renamed it, and under that name it closed the game ("This library isn't supported", 28.10).
if exist vorbisHooked.dll (
    findstr /m /c:"vorbisfile.dll" vorbisHooked.dll >nul
    if errorlevel 1 (
        findstr /m /c:"vorbisHooked" vorbisFile.dll >nul
        if not errorlevel 1 goto restore
    )
    echo Загрузчик ASI уже установлен. Ничего менять не нужно.
    echo Можно запускать игру.
    pause
    exit /b 0
)
if not exist vorbisFile.dll (
    echo В папке игры нет vorbisFile.dll. Похоже, это не GTA San Andreas 1.0.
    pause
    exit /b 1
)
findstr /m /c:"vorbisfile.dll" vorbisFile.dll >nul
if errorlevel 1 (
    echo В игре уже стоит загрузчик ASI: vorbisFile.dll не оригинальный, например из Widescreen Fix.
    echo Он сам загрузит SA:COOP, ничего менять не нужно. Можно запускать игру.
    pause
    exit /b 0
)
ren vorbisFile.dll vorbisHooked.dll
copy /y "coop\loader\vorbisFile.dll" vorbisFile.dll >nul
if errorlevel 1 (
    ren vorbisHooked.dll vorbisFile.dll
    echo Не получилось скопировать загрузчик. Если игра лежит в Program Files, запустите install.bat от имени администратора.
    pause
    exit /b 1
)
echo Загрузчик ASI установлен. Оригинальный vorbisFile.dll сохранён как vorbisHooked.dll.
echo Можно запускать игру.
pause
exit /b 0

:restore
rem vorbisHooked.dll is another loader the old install.bat renamed, and vorbisFile.dll is ours: that one comes back
del vorbisFile.dll
if exist vorbisFile.dll (
    echo Не получилось убрать загрузчик SA-COOP. Закройте игру и запустите install.bat ещё раз. Если игра лежит
    echo в Program Files, запустите install.bat от имени администратора.
    pause
    exit /b 1
)
ren vorbisHooked.dll vorbisFile.dll
if not exist vorbisFile.dll (
    echo Не получилось переименовать vorbisHooked.dll в vorbisFile.dll. Переименуйте его вручную.
    pause
    exit /b 1
)
echo Ваш загрузчик ASI ^(например, из Widescreen Fix^) возвращён на место: прежний install.bat переименовал его
echo в vorbisHooked.dll, и игра закрывалась с ошибкой «This library isn't supported». Он сам загрузит SA:COOP.
echo Можно запускать игру.
pause
