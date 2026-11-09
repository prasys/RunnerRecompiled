"""The core of RunnerBuilder: from a GOG copy of Mirror's Edge to a folder ready for the tablet.

The windows of the program (pc/tools/builder-gui) only collect the two paths, run this file and show
its output, so a port to another system needs a new window and a new entry in HOSTS, not a new builder.
Lines that start with "==" name the current stage; the rest is the log. See pc/BUILDER.md.

  python -I -X utf8 -u pc/tools/builder.py --game "C:/GOG Games/Mirror's Edge" --out ready \
      --home C:/RunnerBuilder --accept-android-terms

Everything derived from the game stays in --home and --out: never publish or share them.
"""
import argparse
import hashlib
import os
import shutil
import sys
import subprocess
import urllib.request
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import redist  # noqa: E402
import selfbuild  # noqa: E402

REPO = os.path.normpath(os.path.join(HERE, "..", ".."))
GOOGLE = "https://dl.google.com/android/repository/"

# One entry per system the builder runs on. Linux and macOS are planned: see pc/BUILDER.md.
# Each tool: folder under --home, file that proves it is unpacked, URL, kind and value of the checksum
# (None: the publisher gives no fixed one here).
HOSTS = {
    "win32": [
        ("llvm-mingw", "bin/x86_64-w64-mingw32-clang.exe",
         "https://github.com/mstorsjo/llvm-mingw/releases/download/20261006/llvm-mingw-20261006-ucrt-x86_64.zip",
         "sha256", "317492c456aa27ee607a5919f1d2d38dcdc1112516a24d0bf4b00d078f52d17a"),
        ("android-ndk", "toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe",
         GOOGLE + "android-ndk-r30-windows.zip", "sha1", "9bf167a1985fa7d4a036186b78f702eab9179408"),
        ("android-sdk/build-tools/35.0.0", "apksigner.bat",
         GOOGLE + "build-tools_r35_windows.zip", "sha1", "af059bb67cf7786f45ee0db85e2d24985df1b4b6"),
        ("android-sdk/platforms/android-35", "android.jar",
         GOOGLE + "platform-35_r02.zip", "sha1", "0bb560a90a7a2cbd0dd8348224d518b638fe7949"),
        ("jdk", "bin/javac.exe",
         "https://github.com/adoptium/temurin21-binaries/releases/download/jdk-21.0.12.1%2B1/"
         "OpenJDK21U-jdk_x64_windows_hotspot_21.0.12.1_1.zip",
         "sha256", "f9d6e191ab098c0d416e7d588a24420a8621cd2f4720dab2459b8b7b2d2d8b4e"),
    ],
}
# Microsoft's current Visual C++ 2005 installer. It is only unpacked, never run.
VCREDIST = ("https://download.microsoft.com/download/8/B/4/8B42259F-5D70-43F4-AC2E-4B208FD8D66A/vcredist_x86.EXE",
            "8648c5fc29c44b9112fe52f9a33f80e7fc42d10f3b5b42b2121542a13e44adfd")
# NVIDIA's PhysX System Software with Engine/v2.8.0. Also only unpacked.
PHYSX = ("https://us.download.nvidia.com/Windows/9.23.1019/PhysX_9.23.1019_SystemSoftware.exe",
         "9b42b84e881769d681e09f62a1b51532616b2e6a2d5d99d0ccae6eb5fbbc208c")
GAME_DIRS = ("Binaries", "Engine", "TdGame")
NEED_FREE_GB = 25  # tools about 5, build about 2, the ZIP of the game 11.5, plus room to unpack

README = """\
Mirror's Edge для Android — подготовлено из вашей копии игры

В этой папке:
  RunnerRecompiled-pc-personal.apk  приложение
  MirrorsEdge-GOG.zip               файлы вашей игры

Что сделать:
1. Скопируйте APK и MirrorsEdge-GOG.zip на планшет, например в папку Download.
2. Установите APK. Android попросит разрешить установку из этого источника.
3. Откройте приложение и выберите MirrorsEdge-GOG.zip, когда оно попросит архив игры.
4. Если игра не запускается или картинка с артефактами, в настройках приложения откройте
   «Драйвер графики» и выберите ZIP-пакет драйвера для AdrenoTools (например, Turnip).

Нужно устройство Android с Vulkan и процессором ARMv8.4 или новее.

APK и архив содержат данные вашей лицензионной копии игры. Держите их у себя:
не публикуйте и не передавайте другим людям.
"""


LANG = "ru"
_table = {}


def set_language(code):
    """The tables are next to the window's sources; Russian text is the key and the fallback."""
    global LANG
    LANG = code
    path = os.path.join(HERE, "builder-gui", "i18n", f"{code}.txt")
    if not os.path.isfile(path):
        return
    with open(path, encoding="utf-8") as f:
        for line in f:
            key, _, value = line.rstrip("\n").partition("\t")
            _table[key.replace("\\\\", "\\")] = value.replace("\\n", "\n").replace("\\\\", "\\")


def t(text, **values):
    text = _table.get(text, text)
    return text.format(**values) if values else text


def stage(text):
    print(f"== {text}", flush=True)


def stop(text):
    print("\n" + t("ОШИБКА: {text}", text=text), flush=True)
    sys.exit(1)


def check_game(game):
    exe = os.path.join(game, "Binaries", "MirrorsEdge.exe")
    if not os.path.isfile(exe):
        stop(t("В этой папке нет Binaries\\MirrorsEdge.exe. Укажите папку, куда установлена игра из GOG."))
    for name in GAME_DIRS:
        if not os.path.isdir(os.path.join(game, name)):
            stop(t("В папке игры не хватает каталога {name}. Переустановите игру через GOG.", name=name))
    if selfbuild.sha256(exe) != selfbuild.GAME_EXE:
        stop(t("Этот MirrorsEdge.exe не из поддерживаемой версии GOG. Копии из Steam, EA и с дисков не подходят."))


def ensure_physx(home):
    """The folder with the PhysX 2.8.0 DLLs, or None when they are in the system."""
    _, missing = selfbuild.locate_libraries()
    if all(name in selfbuild.CRT for name in missing):
        return None
    target = os.path.join(home, "redist", "physx")
    _, missing = selfbuild.locate_libraries(physx_dir=target)
    if all(name in selfbuild.CRT for name in missing):
        return target
    stage(t("Скачивание PhysX"))
    os.makedirs(os.path.join(home, "redist"), exist_ok=True)
    installer = os.path.join(home, "redist", "physx.exe")
    download(PHYSX[0], installer, "sha256", PHYSX[1])
    left = redist.extract_7z(installer, {p[1]: p[0] for p in selfbuild.PHYSX}, target)
    os.remove(installer)
    if left:
        stop(t("В установщике PhysX нет нужных файлов: ") + ", ".join(left))
    return target


def ensure_crt(home):
    """The folder with the Visual C++ 2005 DLLs, or None when they are in the system."""
    _, missing = selfbuild.locate_libraries()
    if not any(name in selfbuild.CRT for name in missing):
        return None
    target = os.path.join(home, "redist", "crt")
    _, missing = selfbuild.locate_libraries(crt_dir=target)
    if not any(name in selfbuild.CRT for name in missing):
        return target
    stage(t("Скачивание библиотек Visual C++ 2005"))
    os.makedirs(os.path.join(home, "redist"), exist_ok=True)
    installer = os.path.join(home, "redist", "vcredist_x86.exe")
    download(VCREDIST[0], installer, "sha256", VCREDIST[1])
    left = redist.extract(installer, {want[1]: name for name, want in selfbuild.CRT.items()}, target)
    os.remove(installer)
    if left:
        stop(t("В установщике Visual C++ нет нужных файлов: ") + ", ".join(left))
    return target


def download(url, target, kind, want):
    name = os.path.basename(target)
    h = hashlib.new(kind) if kind else None
    request = urllib.request.Request(url, headers={"User-Agent": "RunnerBuilder"})
    with urllib.request.urlopen(request, timeout=60) as r, open(target, "wb") as f:
        total = int(r.headers.get("Content-Length") or 0)
        done = shown = 0
        for chunk in iter(lambda: r.read(1 << 20), b""):
            f.write(chunk)
            done += len(chunk)
            if h:
                h.update(chunk)
            percent = done * 100 // total if total else 0
            if percent >= shown + 10:
                shown = percent - percent % 10
                print(f"  {name}: {shown}%", flush=True)
    if h and h.hexdigest() != want:
        os.remove(target)
        stop(t("Файл {name} скачался с другой контрольной суммой. Запустите сборку снова.", name=name))


def unpack(archive, folder):
    """Unpacks into `folder` without the archive's single top-level directory."""
    tmp = folder + ".tmp"
    shutil.rmtree(tmp, ignore_errors=True)
    with zipfile.ZipFile(archive) as z:
        z.extractall(tmp)
    entries = os.listdir(tmp)
    root = os.path.join(tmp, entries[0]) if len(entries) == 1 and os.path.isdir(os.path.join(tmp, entries[0])) else tmp
    os.makedirs(os.path.dirname(folder), exist_ok=True)
    os.replace(root, folder)
    shutil.rmtree(tmp, ignore_errors=True)


def ensure_tools(home, accepted):
    tools = os.path.join(home, "tools")
    absent = [t for t in HOSTS[sys.platform] if not os.path.isfile(os.path.join(tools, t[0], t[1]))]
    if absent and not accepted:
        stop(t("Для скачивания Android SDK и NDK нужно принять условия Google (--accept-android-terms)."))
    for number, (folder, _, url, kind, want) in enumerate(absent, 1):
        stage(t("Скачивание инструментов {number} из {total}", number=number, total=len(absent)))
        archive = os.path.join(tools, url.rsplit("/", 1)[1].replace("%2B", "+"))
        os.makedirs(tools, exist_ok=True)
        download(url, archive, kind, want)
        print(t("  распаковка"), flush=True)
        unpack(archive, os.path.join(tools, *folder.split("/")))
        os.remove(archive)
    return tools


def zip_game(game, target):
    tmp = target + ".part"
    files = [(os.path.join(base, f), os.path.relpath(os.path.join(base, f), game))
             for name in GAME_DIRS for base, _, names in os.walk(os.path.join(game, name)) for f in names]
    shown = 0
    # Stored, not deflated: the packages of the game are already compressed.
    with zipfile.ZipFile(tmp, "w", zipfile.ZIP_STORED, allowZip64=True) as z:
        for number, (path, rel) in enumerate(files, 1):
            z.write(path, rel.replace(os.sep, "/"))
            percent = number * 100 // len(files)
            if percent >= shown + 10:
                shown = percent - percent % 10
                print(t("  архив игры: {shown}%", shown=shown), flush=True)
    os.replace(tmp, target)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--game", required=True, help="папка установленной игры из GOG")
    ap.add_argument("--out", required=True, help="куда положить готовые файлы")
    ap.add_argument("--home", required=True, help="папка инструментов, ключа и промежуточной сборки")
    ap.add_argument("--accept-android-terms", action="store_true", help="условия Android SDK приняты")
    ap.add_argument("--keep-build", action="store_true", help="не удалять промежуточную сборку")
    ap.add_argument("--lang", default="ru", help="язык сообщений: ru, en, de, fr, es, pt, pl, zh")
    args = ap.parse_args()
    set_language(args.lang)
    if sys.platform not in HOSTS:
        stop(t("Эта версия программы работает только в Windows. Версии для Linux и macOS запланированы."))
    game, out, home = (os.path.abspath(p) for p in (args.game, args.out, args.home))
    if not (home.isascii() and HERE.isascii()):  # aapt2 cannot open such paths
        stop(t("Рабочая папка должна быть без русских букв в пути: {home}", home=home))

    stage(t("Проверка файлов игры"))
    check_game(game)
    os.makedirs(out, exist_ok=True)
    os.makedirs(home, exist_ok=True)
    free = shutil.disk_usage(home).free / 2 ** 30
    if free < NEED_FREE_GB:
        stop(t("На диске свободно {free} ГБ, а нужно {need} ГБ.", free=f"{free:.0f}", need=NEED_FREE_GB))

    tools = ensure_tools(home, args.accept_android_terms)
    crt, physx = ensure_crt(home), ensure_physx(home)

    stage(t("Сборка приложения из вашей копии игры (от 5 минут до получаса)"))
    prebuilt = os.path.join(REPO, "prebuilt")
    workspace = os.path.join(home, "build")
    subprocess.run([sys.executable, "-I", "-X", "utf8", "-u", os.path.join(HERE, "selfbuild.py"), "--game", game,
                    *(["--crt", crt] if crt else []), *(["--physx", physx] if physx else []),
                    "--dxvk", os.path.join(prebuilt, "libdxvk_d3d9.so"),
                    "--adrenotools", os.path.join(prebuilt, "adrenotools"),
                    "--toolchain", os.path.join(tools, "llvm-mingw"), "--ndk", os.path.join(tools, "android-ndk"),
                    "--sdk", os.path.join(tools, "android-sdk"), "--jdk", os.path.join(tools, "jdk"),
                    "--workspace", workspace, "--keystore", os.path.join(home, "personal-key.p12")], check=True)

    stage(t("Подготовка папки для планшета"))
    shutil.copyfile(os.path.join(workspace, "RunnerRecompiled-pc-personal.apk"),
                    os.path.join(out, "RunnerRecompiled-pc-personal.apk"))
    zip_game(game, os.path.join(out, "MirrorsEdge-GOG.zip"))
    for name in os.listdir(prebuilt):
        if name.startswith("turnip-") and name.endswith(".zip"):
            shutil.copyfile(os.path.join(prebuilt, name), os.path.join(out, name))
    with open(os.path.join(out, t("КАК УСТАНОВИТЬ.txt")), "w", encoding="utf-8-sig", newline="\r\n") as f:
        f.write(README if LANG == "ru" else t("README"))
    if not args.keep_build:
        shutil.rmtree(workspace, ignore_errors=True)
    stage(t("Готово"))
    print(f"DONE {out}", flush=True)


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as e:
        stop(t("Шаг сборки завершился с кодом {code}. Подробности в журнале выше.", code=e.returncode))
    except OSError as e:
        stop(str(e))
