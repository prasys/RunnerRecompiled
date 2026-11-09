"""Applies selected Xenia Canary game patches for Mirror's Edge (45410850, default.xex
hash 25AA0820DF95AA47; xenia-canary/game-patches) to the static recompilation, i.e. to
the generated C++ of the affected guest instructions. Idempotent; keeps <file>.orig.
Each patch lists (function, original C++ line, replacement). Instruction words at the
addresses were checked against the decoded XEX (work/startup/ppc_at.py) before use.
Usage: python apply_xenia_patches.py <project root> [--revert]"""
import shutil, sys
from pathlib import Path

root = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path.cwd()
gen = root / 'work/xbox-game-private/generated-fourth-final'
MARK = ' // XENIA PATCH:'
PATCHES = [
    # Unlock FPS (illusion, boma): 0x8254f5fc lfs f1,1152(r3) -> nop; 0x8281492b li r11,2 -> li r11,1
    # Switchable at run time: me_unlock_fps() is false with ME_UNLOCK_FPS=0 (original code).
    ('ppc_recomp.73.cpp', 'sub_8254F5E0', '\ttemp.u32 = PPC_LOAD_U32(ctx.r3.u32 + 1152);\n\tctx.f1.f64 = double(temp.f32);\n',
     '\tif (!me_unlock_fps()) { temp.u32 = PPC_LOAD_U32(ctx.r3.u32 + 1152); ctx.f1.f64 = double(temp.f32); }' + MARK + ' Unlock FPS, 0x8254f5fc lfs f1,1152(r3) -> nop\n'),
    ('ppc_recomp.132.cpp', 'sub_82814838', 'loc_82814928:\n\t// li r11,2\n\tctx.r11.s64 = 2;\n',
     'loc_82814928:\n\t// li r11,2\n\tctx.r11.s64 = me_unlock_fps() ? 1 : 2;' + MARK + ' Unlock FPS, 0x8281492b li r11,2 -> li r11,1\n'),
    # Dynamic Lighting & Shadows Shading Fix (boma): invisible hands/feet from MSAA tiling.
    # Switchable at run time (shadow flicker A/B): me_shading_fix() is false with
    # ME_SHADING_FIX=0, and the original store then runs.
    ('ppc_recomp.39.cpp', 'sub_823AA2B0', '\tPPC_STORE_U32(ctx.r7.u32 + 19380, ctx.r9.u32);\n',
     '\tif (me_shading_fix()) ctx.r9.s64 = 0; else PPC_STORE_U32(ctx.r7.u32 + 19380, ctx.r9.u32);' + MARK + ' Shading Fix, 0x823aa2e0 stw r9,19380(r7) -> li r9,0\n'),
    ('ppc_recomp.39.cpp', 'sub_823AA310', '\tPPC_STORE_U32(ctx.r8.u32 + 19380, ctx.r9.u32);\n',
     '\tif (me_shading_fix()) ctx.r9.s64 = 0; else PPC_STORE_U32(ctx.r8.u32 + 19380, ctx.r9.u32);' + MARK + ' Shading Fix, 0x823aa350 stw r9,19380(r8) -> li r9,0\n'),
    # Off by default (original game code); switches in entry_probe.cpp (2026-11-02: FPS drops
    # most with lots of light and reflections - A/B of the post effects the patches remove).
    # Disable Lens Flares (boma): 0x8233ac08 lwz r3,23272(r31) -> li r10,0. The function is a
    # lazily created singleton getter run at class registration, so ME_DISABLE_LENS_FLARES=1
    # takes effect only from launch.
    ('ppc_recomp.28.cpp', 'sub_8233ABF0', '\tctx.r31.s64 = -2096168960;\n\t// lwz r3,23272(r31)\n\tctx.r3.u64 = PPC_LOAD_U32(ctx.r31.u32 + 23272);\n',
     '\tctx.r31.s64 = -2096168960;\n\t// lwz r3,23272(r31)\n\tif (me_no_lens_flares()) ctx.r10.s64 = 0; else ctx.r3.u64 = PPC_LOAD_U32(ctx.r31.u32 + 23272);' + MARK + ' Disable Lens Flares, 0x8233ac08 lwz r3,23272(r31) -> li r10,0\n'),
    # Disable Motion Blur (boma): 0x8239ad24 addi r18,r31,28 -> addi r23,r21,0. Live toggle.
    ('ppc_recomp.38.cpp', 'sub_8239AC88', '\tctx.r18.s64 = ctx.r31.s64 + 28;\n',
     '\tif (me_no_motion_blur()) ctx.r23.s64 = ctx.r21.s64; else ctx.r18.s64 = ctx.r31.s64 + 28;' + MARK + ' Disable Motion Blur, 0x8239ad24 addi r18,r31,28 -> addi r23,r21,0\n'),
    # Disable Bloom & Depth of Field (illusion): 0x8270f018 beq cr6 -> bne cr6. Live toggle.
    ('ppc_recomp.113.cpp', 'sub_8270F008', '\tctx.cr6.compare<uint32_t>(ctx.r11.u32, 0, ctx.xer);\n\t// beq cr6,0x8270f060\n\tif (ctx.cr6.eq) goto loc_8270F060;\n',
     '\tctx.cr6.compare<uint32_t>(ctx.r11.u32, 0, ctx.xer);\n\t// beq cr6,0x8270f060\n\tif (me_no_bloom_dof() ? !ctx.cr6.eq : ctx.cr6.eq) goto loc_8270F060;' + MARK + ' Disable Bloom & DoF, 0x8270f018 beq -> bne\n'),
]
# Declaration the switchable patches need, added once after the shared include.
DECL = 'extern "C" bool me_shading_fix();' + MARK + ' switch (entry_probe.cpp)\n'
DECL_UNLOCK = 'extern "C" bool me_unlock_fps();' + MARK + ' switch (entry_probe.cpp)\n'
revert = '--revert' in sys.argv
for name, func, old, new in PATCHES:
    path = gen / name
    text = path.read_text(encoding='utf-8')
    head = f'PPC_FUNC_IMPL(__imp__{func})'
    start = text.index(head); end = text.index('\n}\n', start)
    body = text[start:end]
    src, dst = (new, old) if revert else (old, new)
    if body.count(dst) == 1 and body.count(src) == 0:
        print(f'{name} {func}: already {"reverted" if revert else "applied"}'); continue
    assert body.count(src) == 1, f'{name} {func}: expected exactly one match ({body.count(src)})'
    orig = path.with_suffix(path.suffix + '.orig')
    if not orig.exists(): shutil.copy2(path, orig)
    text = text[:start] + body.replace(src, dst) + text[end:]
    include = '#include "ppc_recomp_shared.h"\n'
    switches = [('me_shading_fix()', DECL), ('me_unlock_fps()', DECL_UNLOCK)] + [
        (f'{n}()', f'extern "C" bool {n}();' + MARK + ' switch (entry_probe.cpp)\n') for n in ('me_no_lens_flares', 'me_no_motion_blur', 'me_no_bloom_dof')]
    for switch, decl in switches:
        if switch not in new: continue
        if revert: text = text.replace(include + decl, include)
        elif decl not in text: text = text.replace(include, include + decl, 1)
    path.write_text(text, encoding='utf-8')
    print(f'{name} {func}: {"reverted" if revert else "applied"}')

# Xenia patch 0x829fb3e7 (li r3,1 -> li r3,0): allow a modified Coalesced.ini
# only while the explicit A/B overlay is active. The original code remains the
# default branch, so ordinary game copies retain the original hash check.
path = gen / 'ppc_recomp.205.cpp'
text = path.read_text(encoding='utf-8')
head = 'PPC_FUNC_IMPL(__imp__sub_829FB3C0)'
start = text.index(head)
end = text.index('\n}\n', start)
body = text[start:end]
old = '\tctx.r3.s64 = 1;\nloc_829FB3E8:'
new = '\tctx.r3.s64 = me_diagnostic_coalesced_enabled() ? 0 : 1;' + MARK + ' Coalesced hash check, 0x829fb3e7\nloc_829FB3E8:'
decl = 'extern "C" bool me_diagnostic_coalesced_enabled();' + MARK + ' switch (entry_probe.cpp)\n'
src, dst = (new, old) if revert else (old, new)
if body.count(src) == 1 and body.count(dst) == 0:
    text = text[:start] + body.replace(src, dst) + text[end:]
    include = '#include "ppc_recomp_shared.h"\n'
    if revert:
        text = text.replace(include + decl, include)
    elif decl not in text:
        text = text.replace(include, include + decl, 1)
    path.write_text(text, encoding='utf-8')
    print(f'{path.name} sub_829FB3C0: {"reverted" if revert else "applied"}')
elif body.count(dst) == 1 and body.count(src) == 0:
    print(f'{path.name} sub_829FB3C0: already {"reverted" if revert else "applied"}')
else:
    raise AssertionError('Coalesced hash-check patch location changed')
