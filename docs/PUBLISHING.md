# Сборка и публикация Windows-релизов

## Артефакты и устройство установщика

Пользователь скачивает один файл `FluxTop.exe` для Windows x64. Он содержит Win32 launcher/updater, монитор `fluxtop.exe` (ресурс 201), деинсталлятор (205), модули IntelMSR (206) и AMDFamily17 (208), а также неизменённый официальный установщик PawnIO (207). Сборка MSVC Runtime статически слинкована. Git, CMake, Python и PowerShell на пользовательском компьютере не требуются.

После установки используются `%LOCALAPPDATA%\FluxTop\FluxTop.exe`, `Uninstall-FluxTop.exe` и `versions/<SHA-256>/fluxtop.exe`. Удаление через Windows Installed Apps выполняет нативный EXE. История, настройки и журналы сохраняются. Структура данных описана в [HISTORY.md](HISTORY.md).

Имя релизного asset **`FluxTop.exe` нельзя менять**: его ожидает встроенный updater. `SHA256SUMS.txt` и `LICENSE.txt` публикуются рядом. Updater проверяет HTTPS, размер, digest SHA-256, PE-заголовок и release tag перед активацией. Digest GitHub не заменяет цифровую подпись издателя.

## Подготовка новой версии

1. Выбрать номер, которого ещё нет среди опубликованных тегов. Обновить `project(FluxTop VERSION ...)` в `CMakeLists.txt`, описание в `docs/RELEASE_NOTES.md`, пользовательскую и техническую документацию.
2. Выполнить локальную Release-сборку и CTest. Без повышенных прав `network` может быть помечен `Skipped` — это не успешная проверка сетевого ETW на администраторском токене.
3. Создать коммит и отправить его в `main`. Проверить успешное завершение Windows CI **до создания тега**.
4. Создать новый аннотированный тег на проверенном коммите и отправить его. Существующие теги остаются на прежних коммитах.
5. После локальной приёмки и проверки чистых Windows 10/11 запустить вручную workflow `.github/workflows/release.yml` для проверенного тега и подтвердить `acceptance_verified`. Workflow выполнит сборку и CTest и создаст черновик GitHub Release. Отправка тега сама по себе не запускает публикацию.
6. Скачать `FluxTop.exe` и `SHA256SUMS.txt` из черновика, сверить хеш и проверить установку именно этого файла, обновление предыдущей версии и сохранение INI/SQLite. Опубликовать черновик после проверки и повторно сверить скачанный публичный EXE.

**Не создавать GitHub Release вручную одновременно с workflow:** возможна гонка публикации. Версия CMake и числовая часть тега должны совпадать. Для preview допускается `vX.Y.Z-preview.N`.

## Локальная проверка

Запускайте из корня checkout в Developer PowerShell с MSVC x64:

```powershell
cmake -P tools/Prepare-PawnIO.cmake
cmake --preset windows -DFLUXTOP_RELEASE_TAG:STRING=v0.2.6
cmake --build --preset release
ctest --preset release --output-on-failure
cmake '-DROOT=.' '-DTAG=v0.2.6' '-DOUTPUT=build/release-notes.md' -P tools/Get-ReleaseNotes.cmake
cmake '-DROOT=.' '-DBUILD_DIRECTORY=build' '-DOUTPUT_DIR=out/release' '-DCONFIG=Release' -P tools/Package-Release.cmake
```

**Почему тег указан явно:** `FLUXTOP_RELEASE_TAG` — переменная CMake Cache. После сборки предыдущей версии в том же каталоге значение может остаться `v0.2.5`, даже когда `project(VERSION)` уже изменён. Перед локальной упаковкой задавайте `-DFLUXTOP_RELEASE_TAG:STRING=v0.2.6`; workflow релиза получает тег из GitHub и задаёт его самостоятельно.

Результат: `out/release/FluxTop.exe`, `SHA256SUMS.txt`, `LICENSE.txt` и `build/release-notes.md`. Скрипты упаковки и заметок — нативные CMake, без PowerShell. Для Ninja используйте отдельный каталог сборки и укажите его в `-DBUILD_DIRECTORY`.

`Prepare-PawnIO.cmake` подготавливает входной файл сборки. Он берёт официальный PawnIO 2.2.0 по фиксированному URL, сверяет SHA-256 и использует проверенный файл повторно при последующих сборках. Неверный локальный файл отклоняется. Настройка и сборка пакета могут выполняться без сети после подготовки входного файла. Сам установщик FluxTop получает PawnIO из встроенного ресурса. [Официальный PawnIO Setup 2.2.0](https://github.com/namazso/PawnIO.Setup/releases/tag/2.2.0)

Для локальной приёмки из повышенного Developer PowerShell:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\Apply-V56.ps1
```

Сценарий создаёт новую Release x64 сборку, запускает CTest с запретом пропуска автоматических тестов, проверяет архитектуру и контрольные суммы файлов пакета и запускает сценарии установки именно на копии из `distribution`. Установка выполняется в изолированном каталоге. `candidate.json` фиксирует исходную ревизию, локальные изменения, версию и состояние подписей. Тестовый тег `preview.56` предназначен для локальной сборки.

Перед публичным релизом подтвердите результаты на чистых Windows 10 и 11 x64 со стандартной защитой. На каждой системе проверьте отсутствие PawnIO, его штатную установку, повторное обнаружение и отказ от установки. Проверьте отказ UAC, запуск через «Пуск» и автозапуск после входа, обновление с сохранением INI/SQLite и удаление с сохранением общего PawnIO. [Интерактивные сценарии](STABILITY.md)

Сборка проверяет SHA-256 встроенных `IntelMSR.bin` и `AMDFamily17.bin` по зафиксированным контрольным суммам официального выпуска PawnIO.Modules 0.2.11. Перед публикацией приложите к пакету уведомление LGPL 2.1 и исходники соответствующей версии модулей. [Руководство автора](https://github.com/namazso/PawnIO.Modules/wiki/Using-PawnIO-Modules) · [Выпуск 0.2.11](https://github.com/namazso/PawnIO.Modules/releases/tag/0.2.11) · [Исходники и лицензия 0.2.11](https://github.com/namazso/PawnIO.Modules/tree/0.2.11)

## Проверка тега и публикация

Только после приёмки, коммита и успешного CI. Ниже `vX.Y.Z` обозначает выбранный номер нового выпуска:

```powershell
git status --short
git log -1 --oneline
git tag -a vX.Y.Z -m "FluxTop vX.Y.Z"
git push origin vX.Y.Z
```

Страница тега в GitHub показывает README и документацию из коммита тега. Обновление `main` применяется к следующим выпускам, созданным на новых коммитах.

## Границы автоматической проверки

CTest покрывает математику, SQLite/иконки, JSONL, Win32-установщик, нативное удаление на изолированных тестовых каталогах, Git fast-forward, блокировку и fallback исходного launcher-а. CI не подтверждает ручное взаимодействие с UAC, реальный автозапуск после входа в Windows, права на защищённые процессы, достижение целевого бюджета RAM и работу в эксклюзивном fullscreen.

Релиз остаётся неподписанным, пока не настроена подпись издателя. Не отключайте встроенную защиту Windows ради тестирования.
