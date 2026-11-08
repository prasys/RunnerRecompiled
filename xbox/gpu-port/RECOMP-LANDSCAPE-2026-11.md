# Как другие рекомпиляции Xbox 360 получают скорость (обзор 2026-11-02)

Цель: понять, что мы упускаем на пути к ровным 30 / 60 FPS на Y700 (Adreno 830).

## Главное

Все проекты, где игра идёт быстро, **не эмулируют видеокарту Xenos** (PM4 + EDRAM + resolve + передачи владения RT), а рисуют «родным» рендерером на уровне вызовов D3D игры. Мы же, как и ReXGlue по умолчанию, используем Xenos-бэкенд из Xenia: служебная часть эмуляции у нас ≈11 мс из ≈34 мс GPU на кадр (edram_dump, resolve_copy, host_depth_store, texture_load, копии, разрывы проходов «RT как текстура»).

| Проект | База | Рендер | Результат |
|---|---|---|---|
| UnleashedRecomp (Sonic Unleashed) | XenonRecomp | слой трансляции D3D → plume (D3D12/Vulkan), шейдеры XenosRecomp → HLSL/SPIR-V; «эмуляция GPU не нужна, это сильно ускорило» | ПК, есть любительские Android-форки |
| LibertyRecompAndroid (GTA IV) | **ReXGlue**, Android | `gta4_native`: перехват D3D9-подобных вызовов игры, повтор в Vulkan (NativeCommand → worker → recorder) | **30 FPS 720p на Snapdragon 865 (Adreno 650)**, тяжёлые сцены 25–29, упор в CPU |
| graw360-recomp (GRAW) | ReXGlue | родной Vulkan-бэкенд: RT на каждую гостевую поверхность (dynamic rendering), resolve compute-шейдерами, шейдеры Xenos через SPIR-V-транслятор SDK | CPU записи 2000 draw: 2,5 мс против 5 мс у D3D12-эмуляции |
| Xbox360-Native-Ports (Split/Second, Midnight Club LA, Ridge Racer…) | ReXGlue | «native Vulkan renderer» на каждую игру | ПК |
| gears-of-war-3-recomp (UE3) | ReXGlue | пока Xenos-рендер (D3D12) | ПК |

Автор ReXGlue (интервью): Xenos-бэкенд — временная «служба рендеринга», план — заменить родным рендерингом; каждая игра использует GPU по-своему, порты разбираются сами.

## Что сделал LibertyRecompAndroid (тот же SDK, тоже Android) — применимо к нам

- Родной рендерер вместо Xenos (главное).
- `TU_DEBUG=sysmem` по умолчанию: на Turnip тайловый GMEM теряет ~15 % на многих мелких проходах и resolve (у нас на Qualcomm такого переключателя нет; Turnip на A830 у нас был вдвое медленнее).
- Следующие шаги у них: «перенести resolve внутрь существующих проходов», объединять draw.
- CPU: `-march=armv8.2-a` (инлайн LSE-атомики вместо outline-вызовов), `-fno-emulated-tls` (родной TLS), -O3 + LTO + PGO, горячие функции рядом.
- Ожидания: vblank гостя от vsync дисплея (AChoreographer); `WaitMultiple` спит, а не крутится; `gta4_frame_wait_sleep` — сон вместо busy-wait игры на готовность кадра; ограниченные ожидания fence.
- Потоки звука/XMA — `nice -16` (треск звука).
- Адаптивное качество: при < 30 FPS уменьшать дальность прорисовки, вдвое реже обновлять отражения (`gta4_fps_guard`).

## Сверка с нами

- Флаги сборки: код игры `-O2`, рантайм `-O1`, **без `-march`** → armv8-a с outline-атомиками; TLS в .so идёт через `__tls_get_addr` (в профиле рендер-потока 13–20 % в linker64 из `me_guest_poll_window`). Дёшево: `-march=armv8.2-a` (Oryon в 8 Elite поддерживает), `-mtls-dialect=desc` или убрать thread_local из горячего пути, -O2/-O3 для рантайма.
- Рендер-поток крутится в GetData (запросы окклюзии) и в `sub_8278B508/sub_8280D270` — у них это закрыто сном вместо вращения (нагрев).
- Отражения у Mirror's Edge дорогие (пользователь: FPS падает при свете и отражениях) — аналог `fps_guard` (реже обновлять отражения) возможен как опция.

## Варианты стратегии

1. **Продолжать резать эмуляцию Xenos** (прямой resolve для остальных форм, глубина по необходимости, объединение проходов): по ≈1–3 мс за шаг, к 30 FPS в тяжёлых местах — реально; к 60 — нет (служебная часть + сериализация остаются).
2. **Гибрид по образцу GRAW**: оставить транслятор шейдеров Xenos → SPIR-V, но заменить кэш RT/EDRAM на RT «по гостевым поверхностям» (D3D-уровень: SetRenderTarget/Resolve игры). Убирает дампы, передачи владения, host_depth_store, большую часть разрывов. Требует перехвата D3D-функций XDK в игре (кластер 0x827F2xxx–0x8282Axxx, 186 вызываемых снаружи функций — см. API-RENDER-ESTIMATE.md). Недели работы.
3. **Полный родной рендерер** (как GTA IV): максимум скорости, самый большой объём (наша оценка 17–28 недель; GTA IV показывает, что на ReXGlue и на Android это выполнимо).

Источники: hedge-dev/UnleashedRecomp и XenosRecomp (GitHub), vaduur/LibertyRecompAndroid (README, docs/android/PORTING.md), Juansolo457/graw360-recomp PR #44, CrownParkComputing/Xbox360-Native-Ports, Flippage/gears-of-war-3-recomp, интервью readonlymemo.com с автором ReXGlue.
