# FluxTop — аппаратная совместимость сборщиков (Windows 10/11)

Проверено по реализации FluxTop 0.2.1. Код не использует ID платы ASUS, GPU AMD или название CPU.
Однако стандартный Windows API не означает, что драйвер любого устройства отдаёт все сведения.

| Метрика | Источник | Поддержка / важные границы |
|---|---|---|
| CPU процессов | Toolhelp32, GetProcessTimes, QPC/steady_clock | Intel/AMD на Windows x64 через Win32; требуется доступ к процессу. Для ARM64 нужна отдельная сборка. CPU % нормализован по всем логическим CPU; на CPU с несколькими processor groups проверить нормализацию отдельно |
| Общий CPU | GetSystemTimes | Windows API, но при >64 логических CPU охватывает лишь primary processor group. Нельзя выдавать такое число за всю машину без исправления |
| RAM всей системы | GlobalMemoryStatusEx | Не привязана к производителю; used=physical-available, это не сумма Working Set процессов |
| RAM процессов | GetProcessMemoryInfo | Working Set может включать общие страницы. Недоступность должна оставаться unknown |
| GPU загрузка | PDH GPU Engine | Зависит от WDDM, драйвера и доступных perf counters. Некоторым адаптерам недоступен показатель. Процессов с совпадающим PID не должно быть при join без baseline |
| GPU RAM процессов | D3DKMTQueryVideoMemoryInfo, LOCAL | Привязано к доступным WDDM возможностям, не к модели GPU. UMA, iGPU/dGPU и multi-GPU имеют иную семантику локального сегмента |
| Общая GPU RAM | PDH GPU Adapter Memory + D3DKMT dedicated capacity | Источники значения и ёмкости различны: суммарный процент в системах со смешанными iGPU/dGPU может вводить в заблуждение; нельзя считать VRAM capacity одного адаптера ёмкостью суммарной памяти всех адаптеров |
| GPU температура | D3DKMT AdapterPerfData или HWiNFO | Может отсутствовать у старых или виртуальных драйверов; не универсально гарантировано |
| CPU температура | HWiNFO Shared Memory -> LibreHardwareMonitor WMI -> OpenHardwareMonitor WMI -> CPU-именованная ACPI-зона | Прямой датчик Package/Die приоритетен; при его отсутствии подходит явно названный CPU/Socket датчик платы. Такой источник помечается MB, без имитации Package. Неназванные ACPI-зоны и недоступные датчики показываются как —°C. WMI работает в фоне. |
| Сеть Rx/Tx | Windows ETW TCP/IP | Не привязана к сетевой карте; требует достаточных прав, не измеряет абсолютный физический line-rate интерфейса, не всегда адекватна для VPN/туннелей |
| История | Windows SQLite | Есть snapshots/measurements. Температуры CPU/GPU и рейтинг AutoScore пока не сохраняются в history.db |

## Ограничения и последующие задачи

1. Windows не гарантирует API с реальной CPU Package temperature без специализированного драйвера/агента. Для полностью самостоятельного автономного FluxTop нужен аппаратный модуль (например, на базе LibreHardwareMonitorLib, MPL-2.0), работающий с Intel/AMD и датчиками платы. Явно идентифицированную CPU-зону платы можно показывать с пометкой MB; произвольные ACPI thermal zones нельзя выдавать за CPU Package.
2. Для >64 логических CPU нужен группо-независимый общесистемный счётчик (например, подходящий PDH Processor Information), а не GetSystemTimes одного processor group. До этого маркировать такую конфигурацию как не полностью поддерживаемую.
3. Для много-GPU конфигураций нужна корректная идентификация адаптера и разделение UMA/dedicated, вместо общего суммирования всех адаптеров в одну шкалу.
4. Нужна аппаратная матрица тестирования: Intel/AMD CPU, NVIDIA/AMD/Intel GPU, UMA, ноутбук с гибридной графикой, виртуальная Windows, много процессорных групп; проверять измерения внешними эталонами и адаптерные ограничения.
5. Не интерпретировать отсутствие датчика как 0°C или отсутствие GPU как 0%.

## Температура зоны CPU на материнской плате

Когда недоступен прямой датчик CPU Package/Die, FluxTop принимает датчик
материнской платы с явным именем «CPU», «CPU Socket», «CPU (PECI)» или
«CPU Temperature» из HWiNFO Shared Memory либо WMI Libre/OpenHardwareMonitor.
Как последний вариант проверяются ACPI thermal zones, только когда их InstanceName
явно содержит CPU/Processor. Зоны TZ00/TZ01 без привязки к CPU не подходят.
Верхняя строка помечает такую температуру `MB`; это показание зоны/платы,
не измерение температуры кристалла. При отсутствии источника сохраняется `—°C`.

Windows не гарантирует прямого доступа к Super I/O/EC. Для автономного чтения
регистров оборудования в будущем потребуется отдельный проверенный подписанный
драйвер, а не доступ через устаревший уязвимый WinRing0.

## Проверка источников CPU-температуры

В PowerShell:

```powershell
Get-CimInstance -Namespace root\LibreHardwareMonitor -ClassName Sensor -ErrorAction SilentlyContinue |
    Where-Object { $_.SensorType -eq 'Temperature' -and $_.Identifier -match 'cpu' } |
    Select-Object Identifier,Name,Value
Get-CimInstance -Namespace root\OpenHardwareMonitor -ClassName Sensor -ErrorAction SilentlyContinue |
    Where-Object { $_.SensorType -eq 'Temperature' -and $_.Identifier -match 'cpu' } |
    Select-Object Identifier,Name,Value
```

Если обе команды пустые и HWiNFO Shared Memory отключён, `—°C` — корректное сообщение о недоступном источнике, а не ошибка определения модели CPU.

### Документация

- https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getsystemtimes
- https://learn.microsoft.com/en-us/windows-hardware/design/device-experiences/design-guide
- https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/ns-d3dkmthk-_d3dkmt_adapter_perfdata
- https://github.com/LibreHardwareMonitor/LibreHardwareMonitor
- https://github.com/HardwareMonitor/OpenHardwareMonitor
## Особенности релиза 0.2.1

Группировка приложений в оверлее основана на нормализованном полном EXE-пути,
а при его отсутствии — на проверенном родстве процессов. Ни CPU, ни видеокарта,
ни материнская плата не распознаются по заранее заданному списку моделей.

Датчики CPU Package/Die имеют приоритет перед явно обозначенными CPU Socket /
CPU Thermal Zone датчиками платы. Резервная температура платы маркируется `MB`.
Любая неизвестная ACPI-зона **не считается** температурой CPU. Наличие датчика
на физической плате не гарантирует, что Windows открывает доступ к нему через
WMI/ACPI без специального драйвера. На системах без доступного датчика `—°C`
является ожидаемым результатом; аппаратный backend не включён в 0.2.1.

GPU RAM процессов: сначала D3DKMTQueryVideoMemoryInfo, затем PDH
`GPU Process Memory(*)\Dedicated Usage` для отсутствующего/нулевого
первичного результата. Разные физические GPU-адаптеры суммируются только
в пределах одного PID, с проверками корректности значений и идентичности
процесса; другие процессы того же EXE объединяются отдельно на уровне оверлея.