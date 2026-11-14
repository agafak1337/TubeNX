# Тулчейн TubeNX (RB-04)

Фиксируем версии, чтобы сборка не зависела от «у меня стоит». Этот файл —
источник правды; CI использует тот же образ.

## Базовый образ (CI)

```
devkitpro/devkita64:latest
```

## Пакеты

Портлибы Switch (устанавливаются поверх образа):

```
dkp-pacman -S --noconfirm --needed \
  switch-mpv switch-curl switch-jansson switch-ffmpeg switch-sdl2
```

## Проверенные версии (локальная машина автора)

| Компонент | Версия |
|---|---|
| devkitA64 GCC | `aarch64-none-elf-gcc (devkitA64) 15.2.0` |
| libnx | из `$(DEVKITPRO)/libnx` |
| Хост-компилятор (тесты) | `g++` (C++17) |

## Проверка

```
export DEVKITPRO=/path/to/devkitpro
make doctor     # печатает версии и наличие портлиб
make -j$(nproc)
```

Сборка офлайн: все зависимости вендорены в devkitPro/портлибах, сеть не нужна.
