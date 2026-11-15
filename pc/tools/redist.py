"""Takes single DLLs out of the official Visual C++ and PhysX installers without running them.

Nothing is installed: an installer is a cabinet inside an .exe, an .msi or both, so this walks
through those layers and picks the wanted files by SHA-256. Windows only for now (expand.exe, and
the tar.exe that comes with Windows, which reads 7-Zip archives).
"""
import hashlib
import os
import shutil
import struct
import subprocess
import tempfile

CFB_MAGIC = bytes.fromhex("D0CF11E0A1B11AE1")


def cfb_streams(data):
    """The large streams of an OLE compound file (.msi), each rebuilt from its sector chain."""
    sector = 1 << struct.unpack_from("<H", data, 30)[0]
    at = lambda n: (n + 1) * sector  # noqa: E731
    fat_count, directory_start = struct.unpack_from("<I", data, 44)[0], struct.unpack_from("<I", data, 48)[0]
    difat = list(struct.unpack_from("<109I", data, 76))
    next_difat, extra = struct.unpack_from("<II", data, 68)
    for _ in range(extra):
        chunk = struct.unpack_from(f"<{sector // 4}I", data, at(next_difat))
        difat += chunk[:-1]
        next_difat = chunk[-1]
    fat = []
    for n in difat[:fat_count]:
        fat += struct.unpack_from(f"<{sector // 4}I", data, at(n))

    def chain(first):
        parts, n = [], first
        while n < 0xFFFFFFFA:
            parts.append(data[at(n):at(n) + sector])
            n = fat[n]
        return b"".join(parts)

    cutoff = struct.unpack_from("<I", data, 56)[0]
    directory = chain(directory_start)
    for off in range(0, len(directory) - 127, 128):
        kind = directory[off + 66]
        first, size = struct.unpack_from("<II", directory, off + 116)
        if kind == 2 and size >= cutoff:
            yield chain(first)[:size]


def cabinets(data):
    """Cabinets glued into an executable or stored in the streams of an .msi."""
    if data[:8] == CFB_MAGIC:
        return [s for s in cfb_streams(data) if s[:4] == b"MSCF"]
    found, pos = [], 0
    while True:
        pos = data.find(b"MSCF\0\0\0\0", pos)
        if pos < 0:
            return found
        size = struct.unpack_from("<I", data, pos + 8)[0]
        if data[pos + 25] == 1 and 0 < size <= len(data) - pos:
            found.append(data[pos:pos + size])
            pos += size
        else:
            pos += 4


def walk(data, tmp, on_file, depth=0):
    """Unpacks every cabinet in `data`, and whatever those hold, calling on_file for each file."""
    for number, cab in enumerate(cabinets(data)):
        folder = os.path.join(tmp, f"{depth}-{number}")
        os.makedirs(folder)
        path = folder + ".cab"
        with open(path, "wb") as f:
            f.write(cab)
        subprocess.run(["expand.exe", "-F:*", path, folder], check=True, stdout=subprocess.DEVNULL)
        for name in sorted(os.listdir(folder)):
            with open(os.path.join(folder, name), "rb") as f:
                content = f.read()
            on_file(content)
            if depth < 3 and (content[:2] == b"MZ" or content[:8] == CFB_MAGIC or content[:4] == b"MSCF"):
                nested = folder + f"-{name}"
                os.makedirs(nested)
                walk(content, nested, on_file, depth + 1)


def extract_7z(installer, wanted, target):
    """The same as extract() for a 7-Zip self-extracting installer."""
    with open(installer, "rb") as f:
        data = f.read()
    start = data.find(b"7z\xbc\xaf\x27\x1c")
    if start < 0:
        return sorted(wanted.values())
    left = dict(wanted)
    tmp = tempfile.mkdtemp(prefix="redist-")
    try:
        archive = os.path.join(tmp, "a.7z")
        with open(archive, "wb") as f:
            f.write(data[start:])
        del data
        subprocess.run(["tar.exe", "-xf", archive, "-C", tmp], check=True, stdout=subprocess.DEVNULL)
        for base, _, names in os.walk(tmp):
            for name in names:
                path = os.path.join(base, name)
                with open(path, "rb") as f:
                    content = f.read()
                digest = hashlib.sha256(content).hexdigest()
                if digest in left:
                    os.makedirs(target, exist_ok=True)
                    with open(os.path.join(target, left.pop(digest)), "wb") as out:
                        out.write(content)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return sorted(left.values())


def extract(installer, wanted, target):
    """Writes the files of `wanted` ({sha256: name}) to `target`; returns the names not found."""
    with open(installer, "rb") as f:
        data = f.read()
    left = dict(wanted)

    def on_file(content):
        digest = hashlib.sha256(content).hexdigest()
        if digest in left:
            os.makedirs(target, exist_ok=True)
            with open(os.path.join(target, left.pop(digest)), "wb") as out:
                out.write(content)

    tmp = tempfile.mkdtemp(prefix="redist-")
    try:
        walk(data, tmp, on_file)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return sorted(left.values())
