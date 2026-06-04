# Исследование механизмов CPU Parking в ядре Windows 11 (ARM64)

## Введение

**CPU Parking (Core Parking)** — это механизм энергосбережения в Windows, который позволяет переводить неиспользуемые ядра процессора в состояние глубокого сна (Idle), исключая их из планирования потоков. В отличие от обычного C-state, при парковке ядро вообще не получает задач от планировщика.

## Методология исследования

Исследование проводилось в режиме Kernel Debugging (WinDbg) на системе Windows 11 ARM64 (4 ядра).

### 1. Поиск структур в ядре

Основная информация о состоянии питания процессора хранится в блоке управления процессором (**KPRCB** — Kernel Processor Control Block).

Поле `PowerState` (тип `_PROCESSOR_POWER_STATE`) содержит флаги парковки:

```text
dt nt!_PROCESSOR_POWER_STATE
   ...
   +0x1ac Parked           : UChar
   +0x1ad SoftParked       : UChar
```

### 2. Живой анализ состояния ядер

С помощью WinDbg были извлечены адреса PRCB для всех 4 ядер через `nt!KiProcessorBlock`.

**Смещение для флага Parked:** `0x1400 (PowerState) + 0x1ac = 0x15ac`.

#### Результаты дампа памяти:
| Core | PRCB Address | Value at +0x15ac | Status |
|------|--------------|------------------|--------|
| 0 | `fffff80066e40980` | `00` | **Active** |
| 1 | `ffffe581c305f980` | `01` | **Parked** |
| 2 | `ffffe581c312b980` | `01` | **Parked** |
| 3 | `ffffe581c31a2980` | `01` | **Parked** |

**Вывод:** На момент отладки система припарковала 3 ядра из 4 (75% ядер простаивают).

### 3. Глобальные переменные и маски

Ядро оперирует битовыми масками для быстрого принятия решений о парковке:

*   `nt!PpmParkCoreMask`: Текущая маска припаркованных ядер.
*   `nt!PpmPerfCoreParkingMask`: Маска, рассчитанная на основе производительности.
*   `nt!PpmParkNodes`: Узел (NUMA-aware) со статистикой и масками.

На исследуемой системе `nt!PpmParkCoreMask` содержал значение, соответствующее состоянию ядер, подтверждая, что планировщик видит только Core 0 как доступное для выполнения задач.

### 4. Алгоритм принятия решения (PPM Policy)

Решение о парковке принимается в функции `nt!PpmParkCalculateCoreParkingMask`.
Она учитывает:
1.  **Utilization Thresholds** (пороги загрузки) из активной схемы питания.
2.  **Concurrency Thresholds** (пороги конкуренции).
3.  **Min/Max Cores** (минимальное и максимальное количество ядер).

Эти параметры хранятся в `nt!PpmPolicyConfigTable`.

```text
# Внутренние индексы настроек (enum _PPM_POLICY_SETTINGS):
PpmPolicySettingCPMinCores = 17
PpmPolicySettingCPMaxCores = 18
PpmPolicySettingCPDecreaseThreshold = 4
PpmPolicySettingCPIncreaseThreshold = 5
```

## Практические выводы для оптимизации

1.  **Прямое управление:** Для приложений "оптимизаторов" эффективнее всего использовать `PowerWriteACValueIndex` для изменения `PROCPARKMINCORES` и `PROCPARKMAXCORES`.
2.  **Проверка состояния:** Чтобы узнать, припарковано ли ядро сейчас, не нужно лезть в ядро через драйвер — достаточно системного счетчика производительности "Processor Information -> % Priority Time" или "Processor Performance -> Parked Status".
3.  **Soft Parking:** Поле `SoftParked` указывает на ядра, которые планировщик старается не использовать, но может задействовать, если все остальные ядра перегружены (в отличие от Hard Parking, где ядро полностью исключено).
4.  **ARM64 и Гибридные архитектуры:** На Apple Silicon (через Parallels) или Intel Alder/Raptor Lake парковка работает агрессивнее, чтобы удерживать потоки на эффективных (E-cores) или производительных (P-cores) ядрах в зависимости от QOS-класса потока.

## Полезные WinDbg команды

```text
# Посмотреть все PPM функции
x nt!PpmPark*

# Статус парковки для текущего CPU
dt nt!_PROCESSOR_POWER_STATE @$pcr+1400 Parked

# Маска припаркованных ядер
dq nt!PpmParkCoreMask L1
```

---
*Исследование подготовлено с помощью Gemini CLI & WinDbg Kernel Agent.*
