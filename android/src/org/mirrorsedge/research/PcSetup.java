package org.mirrorsedge.research;

import android.content.Context;
import android.content.Intent;
import android.database.Cursor;
import android.graphics.Color;
import android.net.Uri;
import android.provider.DocumentsContract;
import android.view.View;
import android.view.WindowManager;
import android.widget.*;
import static org.mirrorsedge.research.LauncherUi.*;
import java.io.*;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.file.Files;
import java.security.MessageDigest;
import java.util.*;

/**
 * First-run setup of the PC version: language, the user's own GOG copy, frame limit and resolution.
 * The game copy comes from one ZIP of the installed game or from its folder (the GOG installer only
 * runs on Windows); nothing of it is in the APK. Files already there with the right size are
 * skipped, so an interrupted install resumes. Private builds carry
 * the four runtime DLLs the game copy does not have (assets/pcdeps); the game's own DLLs are
 * rebased here, each result checked against the build the code was lifted from.
 */
final class PcSetup {
    static final int PICK_GAME = 30, PICK_ZIP = 31;
    private static final int WELCOME = 0, DATA = 1, OPTIONS = 2, STEPS = 3;
    /** MirrorsEdge.exe of the supported copy (GOG); the recompiled code matches only it. */
    private static final String EXE_SHA256 = "58de3df21f40e9953e00be92f7749097832bad02e5b657d8485cdb61f360a3dd";
    /** Name, image base of the lifted copy, SHA-256 of the rebased file. */
    private static final String[][] REBASE = {
        {"PhysXLoader.dll", "20000000", "f2ae5f04911a781c4e5b4fd800a0819d9d3c34690ff1be3536a809039fbfbc49"},
        {"NxCooking.dll", "23000000", "ca57712c0234e1d51acd81aea05cf7ef4ca333e76c7b191319e0fe3c8b881603"},
        {"PhysXExtensions.dll", "24000000", "01942e506440658b628bdbf2e85bfbde47fcd2ac4725323543e8234d6ed82909"},
        {"NxCharacter.dll", "25000000", "c5f7f6ceccae3d050452ab6d5a87059bcf81e4bd584486723ea1ae7b97a904f1"},
        {"ogg.dll", "26000000", "fcc4536f8ae0894eef07eb68f2c22225bd4cf5926f4698f25b0f3ec2deee155f"},
        {"vorbis.dll", "27000000", "6736a4b8f79c135b1bc01b0e9ca5da09d853e2ab3e667cf1f9d305b6e8e5d1a9"},
        {"vorbisfile.dll", "28000000", "34aab92c2c9c8c152b68a631ae06c345e7963778710ff687ec4a7cf05608eb98"},
        {"vorbisenc.dll", "29000000", "e7d1a36e7289c3b26cd1d05c8be780f5832356bcfa0c2878fa8232c367a7f353"},
        {"libresample.dll", "2A000000", "f352bab07ccacafbdebc27ef8f64057081f13c826a8acb3d44ec3b6be02d5a2e"},
        {"binkw32.dll", "2B000000", "8a7683e0d9675b347cd60a075c9092fdd6ae8701235e1ea7efa537256e53c3a8"},
    };
    /** assets/pcdeps name, place under files/pc. */
    private static final String[][] BUNDLED = {
        {"msvcr80.dll", "deps/msvcr80.dll"}, {"msvcp80.dll", "deps/msvcp80.dll"},
        {"PhysXCore.dll", "deps/rebased/PhysXCore.dll"}, {"PhysXCooking.dll", "deps/rebased/PhysXCooking.dll"},
    };
    private static final String[] GAME_DIRS = {"Binaries", "Engine", "TdGame"};

    /** True when everything the runtime opens at the start is in place. */
    static boolean filesReady(File pc) {
        if (!new File(pc, "MirrorsEdge/Binaries/MirrorsEdge.exe").isFile() || !new File(pc, "MirrorsEdge/Engine").isDirectory()
            || !new File(pc, "MirrorsEdge/TdGame/CookedPC").isDirectory()) return false;
        for (String[] b : BUNDLED) if (!new File(pc, b[1]).isFile()) return false;
        for (String[] r : REBASE) if (!new File(pc, "deps/rebased/" + r[0]).isFile()) return false;
        return true;
    }

    /** The result of a section-table walk is the same as pc/tools/rebase_pe.py; REBASE checks it. */
    static byte[] rebase(byte[] file, int newBase) {
        byte[] out = file.clone();
        ByteBuffer b = ByteBuffer.wrap(out).order(ByteOrder.LITTLE_ENDIAN);
        int pe = b.getInt(0x3C), sections = b.getShort(pe + 6) & 0xFFFF, optSize = b.getShort(pe + 20) & 0xFFFF;
        int opt = pe + 24, table = opt + optSize, delta = newBase - b.getInt(opt + 28);
        int rva = b.getInt(opt + 96 + 5 * 8), size = b.getInt(opt + 96 + 5 * 8 + 4);
        int p = offset(b, table, sections, rva, 8), end = p + size;
        while (p + 8 <= end) {
            int page = b.getInt(p), block = b.getInt(p + 4);
            if (block < 8) break;
            for (int e = p + 8; e < p + block; e += 2) {
                int entry = b.getShort(e) & 0xFFFF;
                if (entry >> 12 != 3) continue;
                int site = offset(b, table, sections, page + (entry & 0xFFF), 4);
                if (site >= 0) b.putInt(site, b.getInt(site) + delta);
            }
            p += block;
        }
        b.putInt(opt + 28, newBase);
        return out;
    }

    /** File offset of an RVA; -1 when those bytes are not stored in the file. */
    private static int offset(ByteBuffer b, int table, int sections, int rva, int bytes) {
        for (int i = 0; i < sections; ++i) {
            int s = table + 40 * i, vsize = b.getInt(s + 8), va = b.getInt(s + 12), raw = b.getInt(s + 16), off = b.getInt(s + 20);
            if (rva >= va && rva < va + Math.max(vsize, raw)) return rva - va + bytes > raw ? -1 : off + rva - va;
        }
        return -1;
    }

    private static String hex(byte[] d) {
        StringBuilder s = new StringBuilder();
        for (byte x : d) s.append(String.format(Locale.ROOT, "%02x", x & 0xFF));
        return s.toString();
    }

    private final PcLauncherActivity a;
    private final FrameLayout overlay;
    private int step;
    private volatile boolean busy;
    private TextView status, progress;
    private LauncherUi.Bar bar;
    private Btn pick, pickFolder, next;
    private String lastStatus = "", lastProgress = "";
    private float lastFraction;
    private long bytesDone, bytesTotal, lastReport;

    PcSetup(PcLauncherActivity a) {
        this.a = a;
        overlay = new FrameLayout(a);
        overlay.setBackgroundColor(BG);
        overlay.setClickable(true);
        overlay.setFitsSystemWindows(true);
        overlay.setVisibility(View.GONE);
    }

    View view() { return overlay; }
    boolean visible() { return overlay.getVisibility() == View.VISIBLE; }
    boolean isBusy() { return busy; }

    void show(int at) {
        step = at;
        overlay.setVisibility(View.VISIBLE);
        render();
    }

    void showData() { show(DATA); }

    /** Back: the previous step; false on the first one or while copying is not going to stop. */
    boolean back() {
        if (busy) return true;
        if (step == WELCOME) return false;
        step--;
        render();
        return true;
    }

    private void go(int to) {
        if (to >= STEPS) {
            a.prefs().edit().putBoolean("setupDone", true).apply();
            overlay.setVisibility(View.GONE);
            a.refresh();
            return;
        }
        step = to;
        render();
    }

    private TextView body(String s) { return text(a, s, 15, MUTED, false); }

    private LinearLayout.LayoutParams gap(int topDp) {
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(-2, -2);
        p.topMargin = dp(a, topDp);
        return p;
    }

    private void render() {
        overlay.removeAllViews();
        status = progress = null;
        bar = null;
        pick = pickFolder = next = null;
        overlay.addView(new LauncherUi.Backdrop(a, step == WELCOME ? LauncherUi.Backdrop.CITY : LauncherUi.Backdrop.HORIZON, 0), new FrameLayout.LayoutParams(-1, -1));
        LinearLayout card = new LinearLayout(a);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setBackgroundColor(Color.WHITE);
        card.setPadding(dp(a, 32), dp(a, 28), dp(a, 32), dp(a, 24));
        ScrollView scroll = new ScrollView(a);
        scroll.addView(card);
        int width = a.getResources().getDisplayMetrics().widthPixels;
        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(Math.min(dp(a, 620), width - dp(a, 32)), -2);
        lp.leftMargin = dp(a, 16) + Math.max(0, (width - dp(a, 620)) / 8);
        lp.topMargin = dp(a, 24);
        lp.bottomMargin = dp(a, 24);
        overlay.addView(scroll, lp);
        card.addView(wordmark(a, 15));
        switch (step) {
            case WELCOME:
                card.addView(kicker(a, "MIRROR'S EDGE · ПК-ВЕРСИЯ"), gap(22));
                card.addView(text(a, "Настроим за пару минут", 26, INK, true), gap(6));
                card.addView(body("Приложение запускает ПК-версию игры на Android. Файлы игры берутся из вашей копии GOG, в приложении их нет."), gap(8));
                card.addView(row(a, text(a, "Язык игры", 15, INK, false), a.languageBox()), gap(16));
                next = cta(a, "Далее", v -> go(DATA));
                break;
            case DATA:
                card.addView(kicker(a, "ФАЙЛЫ ИГРЫ"), gap(22));
                card.addView(text(a, "Подключите свою копию", 26, INK, true), gap(6));
                card.addView(body("Нужен один файл: ZIP-архив папки с вашей игрой из GOG (внутри Binaries, Engine и TdGame). "
                    + "Приложение само его распакует. Нужно около 12 ГБ свободного места."), gap(8));
                pick = cta(a, "Выбрать архив игры (.zip)…", v -> {
                    Intent open = new Intent(Intent.ACTION_OPEN_DOCUMENT);
                    open.addCategory(Intent.CATEGORY_OPENABLE);
                    open.setType("*/*");
                    a.startActivityForResult(open, PICK_ZIP);
                });
                card.addView(pick, gap(16));
                pickFolder = ghost(a, "Или выбрать распакованную папку…", v -> a.startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), PICK_GAME));
                card.addView(pickFolder, gap(10));
                status = text(a, "", 15, INK, false);
                progress = text(a, "", 13, MUTED, false);
                bar = new LauncherUi.Bar(a);
                card.addView(status, gap(14));
                card.addView(bar, new LinearLayout.LayoutParams(-1, -2));
                card.addView(progress, gap(6));
                next = cta(a, "Далее", v -> go(OPTIONS));
                updateData();
                break;
            default:
                card.addView(kicker(a, "НАСТРОЙКИ"), gap(22));
                card.addView(text(a, "Почти готово", 26, INK, true), gap(6));
                card.addView(body("Это можно изменить позже в разделе «Настройки»."), gap(8));
                String[] fps = new String[PcLauncherActivity.FPS.length];
                for (int i = 0; i < fps.length; ++i) fps[i] = String.valueOf(PcLauncherActivity.FPS[i]);
                int current = 0;
                for (int i = 0; i < fps.length; ++i) if (PcLauncherActivity.FPS[i] == a.prefs().getInt("fps", 62)) current = i;
                card.addView(row(a, text(a, "Лимит кадров", 15, INK, false),
                    segments(a, fps, current, i -> a.prefs().edit().putInt("fps", PcLauncherActivity.FPS[i]).apply())), gap(12));
                card.addView(row(a, text(a, "Начальное разрешение", 15, INK, false),
                    segments(a, PcLauncherActivity.SCALES, a.prefs().getInt("scale", 0), i -> a.prefs().edit().putInt("scale", i).apply())));
                next = cta(a, "Готово", v -> go(STEPS));
                break;
        }
        card.addView(next, gap(20));
        if (step == DATA) next.setVisibility(PcSetup.filesReady(a.pcDir()) ? View.VISIBLE : View.GONE);
    }

    private void updateData() {
        if (status == null) return;
        boolean ready = filesReady(a.pcDir());
        status.setText(busy ? lastStatus : ready ? "Файлы игры на месте." : lastStatus);
        progress.setText(lastProgress);
        bar.set(lastFraction);
        bar.setVisibility(busy || lastFraction > 0 ? View.VISIBLE : View.GONE);
        pick.setEnabled(!busy);
        pick.setAlpha(busy ? 0.4f : 1f);
        pickFolder.setEnabled(!busy);
        pickFolder.setAlpha(busy ? 0.4f : 1f);
        if (next != null) next.setVisibility(!busy && ready ? View.VISIBLE : View.GONE);
    }

    private void say(String s, String p, float fraction) {
        lastStatus = s;
        lastProgress = p;
        lastFraction = fraction;
        a.runOnUiThread(this::updateData);
    }

    void picked(Uri uri, boolean zip) {
        busy = true;
        a.getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        say("Проверяю копию игры…", "", 0);
        new Thread(() -> install(uri, zip), "pc-install").start();
    }

    /** Relative path under the game folder: from the first Binaries/Engine/TdGame component; null if none. */
    private static String gamePath(String entry) {
        String[] parts = entry.replace('\\', '/').split("/");
        for (int i = 0; i < parts.length; ++i)
            for (String d : GAME_DIRS)
                if (parts[i].equalsIgnoreCase(d)) {
                    StringBuilder s = new StringBuilder(d);
                    for (int k = i + 1; k < parts.length; ++k) {
                        if (parts[k].equals("..")) return null;
                        s.append('/').append(parts[k]);
                    }
                    return s.toString();
                }
        return null;
    }

    private static String sha256(File f) throws Exception {
        MessageDigest sha = MessageDigest.getInstance("SHA-256");
        try (InputStream in = new FileInputStream(f)) {
            byte[] buf = new byte[1 << 20];
            for (int n; (n = in.read(buf)) > 0; ) sha.update(buf, 0, n);
        }
        return hex(sha.digest());
    }

    /** One pass over the archive; the check of the executable stops a wrong copy as soon as it is out. */
    private void unzip(Uri uri, File pc) throws Exception {
        long size = -1;
        try (Cursor c = a.getContentResolver().query(uri, new String[] {android.provider.OpenableColumns.SIZE}, null, null, null)) {
            if (c != null && c.moveToFirst() && !c.isNull(0)) size = c.getLong(0);
        }
        if (size > 0 && pc.getUsableSpace() < size + (1L << 30))
            throw new IOException(String.format(Locale.US, "мало места: нужно не меньше %.1f ГБ", (size + (1L << 30)) / 1e9));
        bytesDone = 0;
        bytesTotal = Math.max(size, 0);
        File exe = new File(pc, "MirrorsEdge/Binaries/MirrorsEdge.exe");
        boolean checked = false;
        byte[] buf = new byte[1 << 20];
        InputStream raw = a.getContentResolver().openInputStream(uri);
        if (raw == null) throw new IOException("не удалось открыть архив");
        try (java.util.zip.ZipInputStream zip = new java.util.zip.ZipInputStream(new BufferedInputStream(new FilterInputStream(raw) {
            @Override public int read(byte[] b, int off, int len) throws IOException {
                int n = super.read(b, off, len);
                if (n > 0) { bytesDone += n; report("zip"); }
                return n;
            }
        }, 1 << 20))) {
            for (java.util.zip.ZipEntry e; (e = zip.getNextEntry()) != null; ) {
                String rel = e.isDirectory() ? null : gamePath(e.getName());
                if (rel == null) continue;
                File target = new File(pc, "MirrorsEdge/" + rel);
                if (e.getSize() >= 0 && target.isFile() && target.length() == e.getSize()) {
                    if (target.equals(exe) && !checked) { checkExe(exe); checked = true; }
                    continue;
                }
                mkdirs(target.getParentFile());
                try (OutputStream out = new FileOutputStream(target)) {
                    for (int n; (n = zip.read(buf)) > 0; ) out.write(buf, 0, n);
                }
                if (target.equals(exe)) { checkExe(exe); checked = true; }
            }
        }
        if (!checked) throw new IOException("в архиве нет Binaries/MirrorsEdge.exe");
    }

    private static void checkExe(File exe) throws Exception {
        if (!EXE_SHA256.equals(sha256(exe))) {
            exe.delete();
            throw new IOException("эта версия игры не подходит: нужна копия GOG той же сборки");
        }
    }

    private static final class Entry {
        final String id, name; final boolean dir; final long size;
        Entry(String id, String name, boolean dir, long size) { this.id = id; this.name = name; this.dir = dir; this.size = size; }
    }

    private List<Entry> children(Uri tree, String parent) {
        List<Entry> out = new ArrayList<>();
        Uri uri = DocumentsContract.buildChildDocumentsUriUsingTree(tree, parent);
        String[] columns = {DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME,
            DocumentsContract.Document.COLUMN_MIME_TYPE, DocumentsContract.Document.COLUMN_SIZE};
        try (Cursor c = a.getContentResolver().query(uri, columns, null, null, null)) {
            while (c != null && c.moveToNext())
                out.add(new Entry(c.getString(0), c.getString(1), DocumentsContract.Document.MIME_TYPE_DIR.equals(c.getString(2)), c.isNull(3) ? -1 : c.getLong(3)));
        }
        return out;
    }

    private void collect(Uri tree, Entry dir, File target, List<Entry> from, List<File> to) {
        for (Entry e : children(tree, dir.id)) {
            if (e.dir) collect(tree, e, new File(target, e.name), from, to);
            else { from.add(e); to.add(new File(target, e.name)); }
        }
    }

    private static Entry named(List<Entry> list, String name) {
        for (Entry e : list) if (e.name.equalsIgnoreCase(name)) return e;
        return null;
    }

    private static void mkdirs(File dir) throws IOException {
        if (dir != null && !dir.isDirectory() && !dir.mkdirs() && !dir.isDirectory()) throw new IOException("не удалось создать " + dir);
    }

    private void report(String file) {
        long now = System.currentTimeMillis();
        if (now - lastReport < 300) return;
        lastReport = now;
        say("Копирую файлы игры… Не закрывайте приложение.", String.format(Locale.US, "%.2f / %.2f ГБ", bytesDone / 1e9, bytesTotal / 1e9),
            bytesTotal > 0 ? (float) bytesDone / bytesTotal : 0);
    }

    private void install(Uri tree, boolean zip) {
        String error = null;
        try {
            File pc = a.pcDir();
            mkdirs(pc);
            for (String[] b : BUNDLED) {
                try (InputStream in = a.getAssets().open("pcdeps/" + b[0])) { /* present */ }
                catch (IOException e) { throw new IOException("в этой сборке приложения нет библиотек ПК-версии"); }
            }
            byte[] buf = new byte[1 << 20];
            if (zip) unzip(tree, pc);
            else copyFolder(tree, pc, buf);
            finish(pc, buf);
        } catch (Exception e) {
            error = e.getMessage() == null ? e.toString() : e.getMessage();
        }
        final String failed = error;
        a.runOnUiThread(() -> {
            busy = false;
            a.getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
            lastStatus = failed == null ? "Файлы игры на месте." : "Не получилось: " + failed + ".";
            lastProgress = failed == null ? "" : "Можно выбрать файл ещё раз, готовые файлы пропускаются.";
            updateData();
            a.refresh();
        });
    }

    private void copyFolder(Uri tree, File pc, byte[] buf) throws Exception {
            String root = DocumentsContract.getTreeDocumentId(tree);
            List<Entry> top = children(tree, root);
            List<Entry> from = new ArrayList<>();
            List<File> to = new ArrayList<>();
            for (String d : GAME_DIRS) {
                Entry e = named(top, d);
                if (e == null || !e.dir) throw new IOException("в выбранной папке нет " + d + " (нужна папка, где лежат Binaries, Engine и TdGame)");
                collect(tree, e, new File(pc, "MirrorsEdge/" + d), from, to);
            }
            int exe = -1;
            for (int i = 0; i < from.size(); ++i) if (to.get(i).equals(new File(pc, "MirrorsEdge/Binaries/MirrorsEdge.exe"))) exe = i;
            if (exe < 0) throw new IOException("в Binaries нет MirrorsEdge.exe");
            MessageDigest sha = MessageDigest.getInstance("SHA-256");
            try (InputStream in = open(tree, from.get(exe))) {
                for (int n; (n = in.read(buf)) > 0; ) sha.update(buf, 0, n);
            }
            if (!EXE_SHA256.equals(hex(sha.digest()))) throw new IOException("эта версия игры не подходит: нужна копия GOG той же сборки");
            long missing = 0;
            bytesDone = bytesTotal = 0;
            for (int i = 0; i < from.size(); ++i) {
                long size = Math.max(0, from.get(i).size);
                bytesTotal += size;
                if (!(to.get(i).isFile() && to.get(i).length() == size)) missing += size;
            }
            if (pc.getUsableSpace() < missing + (1L << 30))
                throw new IOException(String.format(Locale.US, "мало места: нужно ещё %.1f ГБ", (missing + (1L << 30)) / 1e9));
            for (int i = 0; i < from.size(); ++i) {
                Entry e = from.get(i); File target = to.get(i);
                if (target.isFile() && e.size >= 0 && target.length() == e.size) { bytesDone += e.size; continue; }
                mkdirs(target.getParentFile());
                try (InputStream in = open(tree, e); OutputStream out = new FileOutputStream(target)) {
                    for (int n; (n = in.read(buf)) > 0; ) { out.write(buf, 0, n); bytesDone += n; report(e.name); }
                }
            }
    }

    private void finish(File pc, byte[] buf) throws Exception {
            say("Подготавливаю библиотеки…", "", 1);
            mkdirs(new File(pc, "deps/rebased"));
            mkdirs(new File(pc, "user"));
            for (String[] b : BUNDLED) {
                try (InputStream in = a.getAssets().open("pcdeps/" + b[0]); OutputStream out = new FileOutputStream(new File(pc, b[1]))) {
                    for (int n; (n = in.read(buf)) > 0; ) out.write(buf, 0, n);
                }
            }
            for (String[] r : REBASE) {
                byte[] copy = rebase(Files.readAllBytes(new File(pc, "MirrorsEdge/Binaries/" + r[0]).toPath()), (int) Long.parseLong(r[1], 16));
                if (!r[2].equals(hex(MessageDigest.getInstance("SHA-256").digest(copy)))) throw new IOException(r[0] + " не совпадает с поддерживаемой сборкой");
                Files.write(new File(pc, "deps/rebased/" + r[0]).toPath(), copy);
            }
    }

    private InputStream open(Uri tree, Entry e) throws IOException {
        InputStream in = a.getContentResolver().openInputStream(DocumentsContract.buildDocumentUriUsingTree(tree, e.id));
        if (in == null) throw new IOException("не удалось открыть " + e.name);
        return in;
    }
}
