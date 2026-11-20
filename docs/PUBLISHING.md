# Сборка и публикация Windows-релизов

## Артефакты и устройство установщика

Пользователь скачивает один файл `FluxTop.exe` для Windows 10/11 x64. Он содержит нативный Win32 launcher/updater, встроенный монитор `fluxtop.exe` (ресурс 201) и нативный деинсталлятор `Uninstall-FluxTop.exe` (ресурс 205). Сборка MSVC Runtime статически слинкована: Git, CMake, Python и PowerShell на пользовательском компьютере не требуются.

После установки используются `%LOCALAPPDATA%\FluxTop\FluxTop.exe`, `Uninstall-FluxTop.exe` и `versions/<SHA-256>/fluxtop.exe`. Удаление через Windows Installed Apps выполняет нативный EXE. История, настройки и журналы сохраняются. Структура данных описана в [HISTORY.md](HISTORY.md).

Имя релизного asset **`FluxTop.exe` нельзя менять**: его ожидает встроенный updater. `SHA256SUMS.txt` и `LICENSE.txt` публикуются рядом. Updater проверяет HTTPS, размер, digest SHA-256, PE-заголовок и release tag перед активацией. Digest GitHub не заменяет цифровую подпись издателя.

## Подготовка v0.2.6

1. Обновить `project(FluxTop VERSION 0.2.6 ...)` в `CMakeLists.txt`, описание в `docs/RELEASE_NOTES.md`, пользовательскую и техническую документацию.
2. Выполнить локальную Release-сборку и CTest. Без повышенных прав `network` может быть помечен `Skipped` — это не успешная проверка сетевого ETW на администраторском токене.
3. Создать коммит и отправить его в `main`. Проверить успешное завершение Windows CI **до создания тега**.
4. Создать **новый аннотированный тег `v0.2.6`** на проверенном коммите и отправить `git push origin v0.2.6`. Не перемещать и не перезаписывать существующий `v0.2.5`.
5. Workflow `.github/workflows/release.yml` соберёт EXE, выполнит CTest, сформирует release notes, `SHA256SUMS.txt` и создаст GitHub Release. Проверить, что в Assets присутствуют `FluxTop.exe`, `SHA256SUMS.txt` и `LICENSE.txt`.
6. Ручной сценарий: обновление установленной предыдущей версии после согласия; установка, вход в Windows, отмена UAC, удаление через Windows и сохранение `fluxtop.db`/настроек.

**Не создавать GitHub Release вручную одновременно с workflow:** возможна гонка публикации. Версия CMake и числовая часть тега должны совпадать. Для preview допускается `vX.Y.Z-preview.N`.

## Локальная проверка

Запускайте из корня checkout в Developer PowerShell с MSVC x64:

```powershell
cmake --preset windows -DFLUXTOP_RELEASE_TAG:STRING=v0.2.6
cmake --build --preset release
ctest --preset release --output-on-failure
cmake -DROOT=. -DTAG=v0.2.6 -DOUTPUT=build/release-notes.md -P tools/Get-ReleaseNotes.cmake
cmake -DROOT=. -DBUILD_DIRECTORY=build -DOUTPUT_DIR=out/release -DCONFIG=Release -P tools/Package-Release.cmake
```

**Почему тег указан явно:** `FLUXTOP_RELEASE_TAG` — переменная CMake Cache. После сборки предыдущей версии в том же каталоге значение может остаться `v0.2.5`, даже когда `project(VERSION)` уже изменён. Перед локальной упаковкой задавайте `-DFLUXTOP_RELEASE_TAG:STRING=v0.2.6`; workflow релиза получает тег из GitHub и задаёт его самостоятельно.

Результат: `out/release/FluxTop.exe`, `SHA256SUMS.txt`, `LICENSE.txt` и `build/release-notes.md`. Скрипты упаковки и заметок — нативные CMake, без PowerShell. Для Ninja используйте отдельный каталог сборки и укажите его в `-DBUILD_DIRECTORY`.

## Проверка тега и публикация

Только после коммита и успешного CI:

```powershell
git status --short
git log -1 --oneline
git tag -a v0.2.6 -m "FluxTop v0.2.6"
git push origin v0.2.6
```

Страница тега в GitHub показывает **README и документацию из коммита тега**. Обновление только `main` не меняет историческую страницу `v0.2.5`. Поэтому README версии `v0.2.6` попадёт на страницу нового тега после его создания.

## Границы автоматической проверки

CTest покрывает математику, SQLite/иконки, JSONL, Win32-установщик, нативное удаление на изолированных тестовых каталогах, Git fast-forward, блокировку и fallback исходного launcher-а. CI не подтверждает ручное взаимодействие с UAC, реальный автозапуск после входа в Windows, права на защищённые процессы, достижение целевого бюджета RAM и работу в эксклюзивном fullscreen.

Релиз остаётся неподписанным, пока не настроена подпись издателя. Не отключайте встроенную защиту Windows ради тестирования.
