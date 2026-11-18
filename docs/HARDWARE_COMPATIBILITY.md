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
| CPU температура | HWiNFO Shared Memory -> LibreHardwareMonitor WMI -> OpenHardwareMonitor WMI | Данные должны происходить из настоящего CPU Package/Die, а не ACPI-зоны. При отсутствии аппаратного поставщика отображается —°C. WMI fallback опрашивается в собственном фоне, без новых зависимостей на .NET |
| Сеть Rx/Tx | Windows ETW TCP/IP | Не привязана к сетевой карте; требует достаточных прав, не измеряет абсолютный физический line-rate интерфейса, не всегда адекватна для VPN/туннелей |
| История | Windows SQLite | Есть snapshots/measurements. Температуры CPU/GPU и рейтинг AutoScore пока не сохраняются в history.db |

## Ограничения и последующие задачи

1. Windows не гарантирует API с реальной CPU Package temperature без специализированного драйвера/агента. Для полностью самостоятельного автономного FluxTop нужен аппаратный модуль (например, разработка на LibreHardwareMonitorLib, MPL-2.0), который правильно обслуживает Intel/AMD/датчики ноутбуков. Он требует оценки стоимости памяти, прав и распространения драйвера. Не подменять датчик температурой ACPI зоны.
2. Для >64 логических CPU нужен группо-независимый общесистемный счётчик (например, подходящий PDH Processor Information), а не GetSystemTimes одного processor group. До этого маркировать такую конфигурацию как не полностью поддерживаемую.
3. Для много-GPU конфигураций нужна корректная идентификация адаптера и разделение UMA/dedicated, вместо общего суммирования всех адаптеров в одну шкалу.
4. Нужна аппаратная матрица тестирования: Intel/AMD CPU, NVIDIA/AMD/Intel GPU, UMA, ноутбук с гибридной графикой, виртуальная Windows, много процессорных групп; проверять измерения внешними эталонами и адаптерные ограничения.
5. Не интерпретировать отсутствие датчика как 0°C или отсутствие GPU как 0%.

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