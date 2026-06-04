# Разработка RAM-диска для Windows (Kernel Mode)

Данный документ представляет собой техническое задание и руководство для системного программиста по созданию полноценного RAM-диска на уровне ядра Windows. 

## 1. Архитектура RAM-диска

Правильный RAM-диск реализуется как драйвер режима ядра (WDM или KMDF), который:
1. Выделяет память в Non-Paged пуле или через физические страницы (MDL).
2. Создает виртуальное блочное устройство (`FILE_DEVICE_DISK`).
3. Обрабатывает дисковые запросы (`IRP_MJ_READ`, `IRP_MJ_WRITE`).
4. Отвечает на запросы Storage Stack (IOCTLs).
5. Взаимодействует с Mount Manager для назначения буквы диска.

### Рекомендуемый подход (WDF/KMDF vs WDM)
Для новых проектов настоятельно рекомендуется использовать **KMDF (Kernel-Mode Driver Framework)** вместо чистого WDM, так как KMDF берет на себя большую часть рутины по PnP (Plug and Play) и управлению питанием.

## 2. Выделение памяти

Существует несколько подходов к хранению данных RAM-диска:

### Вариант А: Non-Paged Pool (Для прототипов и малых дисков)
```c
// Для Windows 10 2004+ (ExAllocatePoolWithTag устарел)
PVOID buf = ExAllocatePool2(POOL_FLAG_NON_PAGED, diskSizeBytes, 'RDSK');
```
*   **Плюсы:** Простота реализации.
*   **Минусы:** Плохо масштабируется для больших объемов (фрагментация пула).

### Вариант Б: Физические страницы + MDL (Рекомендуемый для продакшена)
```c
// Выделение физических страниц
PMDL pMdl = MmAllocatePagesForMdlEx(LowAddress, HighAddress, SkipBytes, TotalBytes, MmCached, MdlMappingNoExecute);
// Отображение в системное адресное пространство для работы драйвера
PVOID pSystemAddress = MmGetSystemAddressForMdlSafe(pMdl, NormalPagePriority | MdlMappingNoExecute);
```
*   **Плюсы:** Идеально для больших дисков (не требует непрерывного куска виртуальной памяти в пуле).

## 3. Обработка запросов (I/O)

Основная работа драйвера заключается в копировании данных между буфером RAM-диска и системными буферами IRP.

```c
NTSTATUS RamDiskReadWrite(PDEVICE_OBJECT DevObj, PIRP Irp) {
    PDEVICE_EXTENSION ext = DevObj->DeviceExtension;
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);

    LARGE_INTEGER offset = (stack->MajorFunction == IRP_MJ_READ)
        ? stack->Parameters.Read.ByteOffset
        : stack->Parameters.Write.ByteOffset;
    ULONG length = (stack->MajorFunction == IRP_MJ_READ)
        ? stack->Parameters.Read.Length
        : stack->Parameters.Write.Length;

    // Проверка выхода за границы диска
    if (offset.QuadPart + length > ext->DiskSize) {
        return CompleteIrp(Irp, STATUS_INVALID_PARAMETER, 0);
    }

    // Получение безопасного указателя на буфер пользователя
    PVOID sysAddr = MmGetSystemAddressForMdlSafe(
        Irp->MdlAddress, NormalPagePriority | MdlMappingNoExecute);
    if (!sysAddr) return CompleteIrp(Irp, STATUS_INSUFFICIENT_RESOURCES, 0);

    PUCHAR diskBuffer = (PUCHAR)ext->DiskBuffer + offset.QuadPart;
    
    if (stack->MajorFunction == IRP_MJ_READ) {
        RtlCopyMemory(sysAddr, diskBuffer, length);
    } else {
        RtlCopyMemory(diskBuffer, sysAddr, length);
    }

    return CompleteIrp(Irp, STATUS_SUCCESS, length);
}
```

## 4. Обязательные IOCTL (Device Control)

Чтобы диспетчер томов (Mount Manager) и утилиты типа `diskpart` или `format` распознали RAM-диск как полноценный накопитель, драйвер **обязан** обрабатывать следующие `IRP_MJ_DEVICE_CONTROL`:

| IOCTL | Описание / Что возвращать |
| :--- | :--- |
| `IOCTL_DISK_GET_DRIVE_GEOMETRY` | Структура `DISK_GEOMETRY`. Нужно задать `BytesPerSector` (обычно 512 или 4096), цилиндры, треки и сектора на трек так, чтобы их перемножение давало общий объем. |
| `IOCTL_DISK_GET_LENGTH_INFO` | Возвращает структуру `GET_LENGTH_INFORMATION` с полным размером диска в байтах. |
| `IOCTL_DISK_GET_PARTITION_INFO_EX` | Возвращает `PARTITION_INFORMATION_EX`. Обычно PartitionStyle = RAW, пока диск не размечен. |
| `IOCTL_DISK_IS_WRITABLE` | Просто возвращает `STATUS_SUCCESS` (если диск не Read-Only). |
| `IOCTL_STORAGE_GET_DEVICE_NUMBER` | Возвращает уникальный `STORAGE_DEVICE_NUMBER`. |
| `IOCTL_DISK_CREATE_DISK` | Вызывается при инициализации диска (создание MBR/GPT). |
| `IOCTL_DISK_SET_DRIVE_LAYOUT_EX` | Задает разметку (партиции). |

## 5. Инициализация и форматирование

После регистрации устройства (`IoCreateDevice`, создание симлинка `IoCreateSymbolicLink`):

1.  **Mount Manager:** Уведомить Mount Manager о новом устройстве (IOCTL `IOCTL_MOUNTDEV_LINK_CREATED` / `IOCTL_MOUNTMGR_CREATE_POINT`), чтобы система назначила букву диска (например, `Z:`).
2.  **Разметка (MBR/GPT):** Драйвер должен позволять `diskpart` выполнять `IOCTL_DISK_CREATE_DISK`.
3.  **Форматирование:** Обычными утилитами Windows (`format.com Z: /FS:NTFS /Q`). RAM-диск выглядит как блочное устройство, поэтому файловые системы (FAT32, NTFS, ReFS) работают поверх него прозрачно.

## 6. Требования (Чек-лист для ТЗ)

Для создания промышленного решения (Production-ready) драйвер должен поддерживать:
*   [ ] Plug and Play (PnP) — корректное создание и удаление диска "на лету".
*   [ ] Power Management — обработка гибернации/сна (опционально сброс содержимого на физический диск, если нужен persistent ramdisk).
*   [ ] Интеграция с Mount Manager (раздача букв дисков).
*   [ ] Поддержка GPT и MBR.
*   [ ] Поддержка NTFS и ReFS.
*   [ ] Выделение памяти через MDL (`MmAllocatePagesForMdlEx`) для обхода ограничений Non-Paged пула.
*   [ ] (Опционально) Persistent Storage — сохранение образа на физический носитель при выгрузке (`IRP_MJ_SHUTDOWN`) и загрузка при старте.
*   [ ] (Опционально) TRIM / UNMAP (если имеет смысл для внутренней реализации).

## 7. Полезные ресурсы для сиспрогера (WDK)

*   **Microsoft Storage Driver Architecture**
*   **WDF (Windows Driver Frameworks) / KMDF**
*   Исходники классического примера Microsoft (Ramdisk WDF Sample в составе Windows driver samples на GitHub).
