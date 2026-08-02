# KdDebuggerDataBlock (`KDBG`) Kernel Research & Empirical Reconstruction

**Environment:**
- **OS:** Windows 11 Pro ARM64 (Build 26100.1)
- **Architecture:** ARM64 / AArch64 (EL1 Kernel Mode)
- **Debugger:** WinDbg Kernel Debugger via MCP Agent (`http://10.211.55.5:44445/mcp`)
- **Tooling:** `structscan v3.0` (Bayesian Multi-Feature Field Classifier & Direct Memory Engine)

---

## 1. Executive Summary

`nt!KdDebuggerDataBlock` (хранящий заголовок `DBGKD_DEBUG_DATA_HEADER64` и структуру `KDDEBUGGER_DATA64`) является ключевой централизованной структурой ядерного отладчика Windows. Она используется WinDbg, отладочными подсистемами, утилитами Memory Forensics (Volatility, Rekall) и компонентами безопасности (Kernel Patch Protection / PatchGuard) для локализации базовых глобальных объектов ядра без обращения к PDB-символам.

---

## 2. Эмпирический Анализ Памяти (WinDbg MCP + `structscan v3.0`)

С помощью расширения `structscan v3.0` был произведен прямой пакетный анализ виртуальной памяти блока `KdDebuggerDataBlock` по адресу `0xfffff802ac600f00`.

### Команда выполнения:
```text
0: kd> !structscan nt!KdDebuggerDataBlock 0x400
```

---

## 3. Реконструированная Таблица Смещений `KdDebuggerDataBlock` (ARM64)

| Смещение | Тип (v3.0) | Энтропия $H$ | Confidence | Символ / Значение | Назначение в ядре Windows |
|---|---|---|---|---|---|
| `+0x0000` | `LIST_ENTRY` | 2.75 | `[#####---]` | `Flink=nt!KdpDebuggerDataListHead` | Кольцевой список блоков данных отладчика |
| `+0x0008` | `Pointer` | 2.75 | `[########]` | `nt!KdpDebuggerDataListHead` | Указатель на голову списка KDBG |
| **`+0x0010`** | **`ASCII`** | **2.75** | **`[####----]`** | **`KDBG`** | **Сигнатура блока отладчика (4 байта)** |
| `+0x0018` | `Pointer` | 2.50 | `[########]` | `nt!_guard_eh_cont_table` | Таблица Control Flow Guard (CFG) |
| `+0x0020` | `Pointer` | 2.75 | `[########]` | `nt!DbgBreakPoint` | Точка останова ядра |
| `+0x0038` | `Pointer` | 2.75 | `[########]` | `nt!KiCallUserMode` | Точка перехода Ядро → Юзермод |
| **`+0x0048`** | **`Pointer`** | **2.75** | **`[########]`** | **`nt!PsLoadedModuleList`** | **Список всех загруженных драйверов в RAM** |
| **`+0x0050`** | **`Pointer`** | **2.75** | **`[########]`** | **`nt!PsActiveProcessHead`** | **Двухсвязный список всех активных EPROCESS** |
| **`+0x0058`** | **`Pointer`** | **2.75** | **`[########]`** | **`nt!PspCidTable`** | **Глобальная таблица хэндлов PID и TID** |
| `+0x0060` | `Pointer` | 2.75 | `[########]` | `nt!ExpSystemResourcesList` | Список системных ресурсов ERESOURCE |
| `+0x0078` | `Pointer` | 2.75 | `[########]` | `nt!KeTimeIncrement` | Шаг системного таймера (100-нс интервалы) |
| `+0x0080` | `Pointer` | 2.75 | `[########]` | `nt!KeBugCheckCallbackListHead` | Коллбэки BSOD (BugCheck) |
| `+0x0088` | `Pointer` | 2.75 | `[########]` | `nt!KiBugCheckData` | Данные падения системы (BugCheck parameters) |
| `+0x0090` | `Pointer` | 2.75 | `[########]` | `nt!IopErrorLogListHead` | Логи ошибок I/O подсистемы |
| **`+0x0098`** | **`Pointer`** | **2.50** | **`[########]`** | **`nt!ObpRootDirectoryObject`** | **Корневой каталог объектов (`\Device`, `\Driver`)** |
| **`+0x00a0`** | **`Pointer`** | **2.75** | **`[########]`** | **`nt!ObpTypeObjectType`** | **Директория типов объектов (`OBJECT_TYPE`)** |
| **`+0x00c0`** | **`Pointer`** | **2.75** | **`[########]`** | **`nt!MmPfnDatabase`** | **Массив PFN (Page Frame Number) фреймов RAM** |
| `+0x0138` | `Integer` | 0.54 | `[####----]` | `0x1000 (4096)` | Размер виртуальной страницы (Page Size) |
| `+0x0140` | `Pointer` | 2.75 | `[########]` | `nt!MmSizeOfPagedPoolInBytes` | Размер подкачиваемого пула |
| **`+0x01b8`** | **`Pointer`** | **2.75** | **`[########]`** | **`nt!PoolTrackTable`** | **Таблица отслеживания всех пулов и тегов** |
| `+0x01c8` | `Pointer` | 2.75 | `[########]` | `nt!MmHighestUserAddress` | Верхняя граница User-Space памяти |
| `+0x01d0` | `Pointer` | 2.75 | `[########]` | `nt!MmSystemRangeStart` | Нижняя граница Kernel-Space памяти |
| `+0x01d8` | `Pointer` | 2.75 | `[########]` | `nt!MmUserProbeAddress` | Адрес валидации пользовательских буферов |
| `+0x01e0` | `Pointer` | 2.75 | `[########]` | `nt!KdPrintDefaultCircularBuffer` | Кольцевой буфер DbgPrint отладки |
| `+0x0208` | `Pointer` | 2.75 | `[########]` | `nt!NtBuildLabEx` | Строка билда ядра Windows |
| **`+0x0218`** | **`Pointer`** | **2.50** | **`[########]`** | **`nt!KiProcessorBlock`** | **Массив структур KPRCB для каждого ядра CPU** |
| `+0x0220` | `Pointer` | 2.75 | `[########]` | `nt!MmUnloadedDrivers` | Список выгруженных драйверов в системе |
| `+0x0228` | `Pointer` | 2.75 | `[########]` | `nt!MmLastUnloadedDriver` | Последний выгруженный драйвер |
| **`+0x0238`** | **`Pointer`** | **2.75** | **`[########]`** | **`nt!MmSpecialPoolTag`** | **Тег специального пула Driver Verifier** |
| `+0x0240` | `Pointer` | 2.75 | `[########]` | `nt!KernelVerifier` | Флаги и состояние Driver Verifier |
| **`+0x0390`** | **`Pointer`** | **2.75** | **`[########]`** | **`nt!KePointerAuthMask`** | **Маска аппаратной аутентификации указателей ARM64 (PAC)** |

---

## 4. Глубокое бурение подструктур (Deep Drill-down Analysis)

### 🅰️ Анализ списка загруженных драйверов (`!structscan list nt!PsLoadedModuleList 0x400`)
Кросс-референс 65 драйверов в памяти реконструировал схему ядра `KLDR_DATA_TABLE_ENTRY`:

| Смещение | Тип | Consistency | Значения / Назначение |
|---|---|---|---|
| `+0x0000` | `LIST_ENTRY` | 100% | Ссылка на следующий/предыдущий драйвер (`InLoadOrderLinks`) |
| **`+0x0030`** | `Pointer` | 98% | **`DllBase` (Базовый адрес загрузки драйвера в RAM)** |
| **`+0x0038`** | `Pointer` | 98% | **`EntryPoint` (Точка входа драйвера `DriverEntry`)** |
| **`+0x0048`** | `UNICODE_STRING` | 98% | **`FullDllName` (Полный путь к `.sys` файлу на диске)** |
| **`+0x0058`** | `UNICODE_STRING` | 98% | **`BaseDllName` (Имя драйвера: `ntoskrnl.exe`, `hal.dll`)** |
| `+0x0068` | `Flags` | 98% | Флаги состояния модуля (`LDRP_ENTRY_PROCESSED`) |
| `+0x0100` | `LIST_ENTRY` | 98% | Вложенная цепочка инициализации модулей |

---

## 5. Архитектурные Особенности ARM64 и Безопасность

### 🛡️ 1. ARM64 Pointer Authentication Code (PAC) (`+0x0390`):
На архитектуре ARM64 ядро использует аппаратные инструкции `PACIA`/`AUTIA` для подписи указателей в верхних битах виртуального адреса (биты 55–63). Поле `nt!KePointerAuthMask` хранит битмаску для очистки PAC-подписи при дереферировании указателей ядра.

### 🔐 2. Защита PatchGuard (KPP) и Зашифрованный KDBG:
В современных релизах Windows 64-bit / ARM64 структура `KdDebuggerDataBlock` в неактивном состоянии отладки может быть зашифрована алгоритмом с динамическим ключом (`KdCopyDataBlock`). Когда отладчик подключается по протоколу KD, ядро расшифровывает блок и делает его доступным для `structscan`.

### 🔍 3. Применение в Forensics и Kernel Exploitation:
* **Поиск скрытых процессов (DKOM):** Если вредоносное ПО удаляет объект из `PsActiveProcessHead` (`+0x0050`), его можно обнаружить, сравнив список с таблицей дескрипторов `PspCidTable` (`+0x0058`) или массивом потоков `KiProcessorBlock` (`+0x0218`).
* **Аудит выгруженных драйверов:** Поле `MmUnloadedDrivers` (`+0x0220`) позволяет обнаружить следы ранее загружавшихся нераспознанных или уязвимых драйверов (BYOVD-атаки).
