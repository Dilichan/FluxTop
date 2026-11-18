# Технологический стек

Решение принято для Windows-only приложения с минимальным фоновым расходом.

| Область | Решение | Причина и этап |
| --- | --- | --- |
| Язык | C++20 | RAII, прямой доступ к Windows API, контроль выделений; этап 1 |
| Платформа | Win32, Windows SDK, Unicode | Без отдельной UI-среды; этап 1 |
| Сборка | CMake 3.24+, MSVC, x64 | Воспроизводимая локальная сборка и Windows CI |
| CPU и RAM | Tool Help + GetProcessTimes + PSAPI | Документированные API, простой проверяемый прототип; этап 1 |
| GPU / memory | PDH GPU Engine / Adapter Memory; D3DKMTQueryVideoMemoryInfo для процессов | Текущая local usage вместо проблемного process Dedicated Usage; недоступное остаётся неизвестным |
| Рендеринг | Direct2D + DirectWrite | Нативный текст и таблица; этап 2 |
| Сцены | SetWinEventHook, EVENT_SYSTEM_FOREGROUND | Событийная смена активного приложения; этап 3 |
| Настройки | Win32-окно, создаваемое по запросу | Ресурсы окна освобождаются при закрытии; этап 3 |
| Сеть и точный дисковый I/O | ETW, отдельные отключаемые сборщики | TCP/UDP IPv4/IPv6 Rx/Tx реализованы, нужны права; дисковый I/O ещё планируется |
| Хранение | SQLite, транзакции и ограниченный RAM-кеш | История без прикладного лимита размера/срока хранения; месяц, год, всё время; этап 5 |
| Тестирование | CTest + собственные небольшие C++ тесты | Без стороннего тестового фреймворка на старте |
| CI | GitHub Actions, Windows | Сборка, unit-тесты и короткий smoke test |
| Обновление | PowerShell launcher + Git upstream + CMake/CTest | Проверка при запуске, fast-forward, отдельные версии EXE и fallback |
| Права монитора | UAC RunAs + Win32 token API (Advapi32) | Административный запуск через launcher; проверка токена и назначенной SeDebugPrivilege |
| Кеш EXE-иконок | WIC PNG 32×32 + системная winsqlite3 | Без квоты на диск и удаления старых путей, ограниченный RAM-кеш, фоновое извлечение; история метрик — отдельный этап |

## Компромиссы первого сборщика

Открытие процесса на каждом снимке проще проверить, но может быть дороже
пакетного получения данных. Первое измерение строит baseline.
Если замеры покажут превышение бюджета, отдельным решением заменяем backend
на пакетный Native API с проверками ABI и резервным документированным путём.
Не связываем модель данных с выбранным способом опроса.

GPU вычисляется по самому занятому двигателю, а не суммой процентов всех
двигателей. Выделенная графическая память на интегрированном GPU может быть
зарезервированной системной памятью, поэтому надпись «VRAM» требует пояснения.
Счётчики отключаются целиком, когда пользователь не запрашивает GPU.

## Зависимости и лицензирование

Первый этап использует стандартную библиотеку C++, Windows API и runtime MSVC.
Текущая Release-сборка использует динамический CRT: для запуска на чистой машине
нужен Visual C++ Redistributable x64, совместимый с компилятором сборки.
Для кеша иконок используется системная SQLite Windows: SDK-заголовок
`winsqlite/winsqlite3.h`, import library `winsqlite3.lib` и DLL операционной
системы. Отдельный SQL-пакет не загружается; версия обновляется вместе с Windows.
История метрик уже записывается отдельно в history.db, без автоудаления.
Графики и статистические запросы — следующий этап; [HISTORY.md](HISTORY.md).
Сторонние исходники не копируются автоматически. При добавлении зависимости
фиксируются версия, происхождение, лицензия и необходимые notices.

Electron, WebView2/Tauri и WPF не входят в выбранный стек.
Rust — возможная альтернатива для отдельного будущего решения,
но смешивать два языка в первом прототипе не требуется.

## Первичные технические источники

- [GetProcessTimes](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getprocesstimes)
- [GetProcessMemoryInfo](https://learn.microsoft.com/en-us/windows/win32/api/psapi/nf-psapi-getprocessmemoryinfo)
- [PdhAddEnglishCounterW](https://learn.microsoft.com/en-us/windows/win32/api/pdh/nf-pdh-pdhaddenglishcounterw)
- [PdhGetFormattedCounterArrayW](https://learn.microsoft.com/en-us/windows/win32/api/pdh/nf-pdh-pdhgetformattedcounterarrayw)
- [Microsoft: GPU engines и memory в Task Manager](https://devblogs.microsoft.com/directx/gpus-in-the-task-manager/)

Другие мониторы из исходной концепции служат ориентирами возможностей.
Утверждения об их лицензиях и пригодности конкретного кода нужно проверять
перед заимствованием; такой код в FluxTop пока отсутствует.
