"""Conformance cases: short snippets that must end with ret.

Float inputs are finite on purpose: NaN payloads and out-of-range conversions
differ between x86 and arm64 hardware and say nothing about the lifter.
"""

from __future__ import annotations

import struct

from tests.conformance.harness import Case, Xmm

D = "0x300000"


def fl(a: float, b: float = 0.0, c: float = 0.0, d: float = 0.0) -> Xmm:
    bits = struct.unpack("<4I", struct.pack("<4f", a, b, c, d))
    return (bits[0], bits[1], bits[2], bits[3])


def dbl(a: float, b: float = 0.0) -> Xmm:
    bits = struct.unpack("<4I", struct.pack("<2d", a, b))
    return (bits[0], bits[1], bits[2], bits[3])


F01 = {0: fl(1.5, -2.25, 3.0, 1e6), 1: fl(0.75, 8.5, -4.0, 2.5)}

HANDLED = [
    Case("add_sub", "add eax, ecx\nsub edx, eax\nadd ebx, 0x12345\nsub esi, edi\nret"),
    Case("adc_sbb", "add eax, ecx\nadc edx, ebx\nsub esi, edi\nsbb ebx, ebp\nret"),
    Case("imul", f"imul eax, ecx\nimul edx, ebx, 1234\nimul esi, dword ptr [{D}]\nimul edi\nret"),
    Case("mul", "mul ecx\nmov esi, edx\nmul ebx\nret"),
    Case("div", "div ecx\nret", regs={"edx": 0x1234, "ecx": 0xFFFF0001}),
    Case("idiv", "cdq\nidiv ecx\nret", regs={"ecx": 0xFFFFFF85}),
    Case("shifts", "shl eax, 5\nshr ecx, 3\nsar edx, 7\nshl ebx, cl\nsar esi, cl\nshr edi, cl\nret"),
    Case("rotates", "rol esi, 9\nror edi, 4\nrol edx, cl\nror ebx, cl\nret"),
    Case("movzx", f"movzx eax, cl\nmovzx edx, bx\nmovzx esi, word ptr [{D}+4]\nmovzx edi, byte ptr [{D}+8]\nret"),
    Case("movsx", f"movsx eax, cl\nmovsx edx, bx\nmovsx esi, word ptr [{D}+4]\nmovsx edi, byte ptr [{D}+8]\nret"),
    Case("lea", "lea eax, [ecx+edx*4+8]\nlea esi, [ebx+ebx*2]\nlea edi, [ebp-0x10]\nret"),
    Case("cmovcc", "cmp eax, ecx\ncmovl edx, ebx\ncmova esi, edi\ncmovne ebx, ebp\ncmove ebx, ecx\ncmovge edi, edx\nret"),
    Case("setcc", "cmp eax, ecx\nsetl bl\nsetae dl\nsete al\nsetg cl\nret"),
    Case("bswap", "bswap eax\nbswap ecx\nret"),
    Case("xchg", "xchg eax, ecx\nxchg edx, ebx\nret"),
    Case("push_pop", f"push eax\npush dword ptr [{D}]\npush 0x1234\npop ebx\npop ecx\npop edx\nret"),
    Case("load_store", f"mov eax, dword ptr [{D}+0x10]\nmov dword ptr [{D}+0x24], ecx\n"
                       f"mov word ptr [{D}+0x30], dx\nmov byte ptr [{D}+0x40], bl\n"
                       f"mov esi, dword ptr [{D}+0x50]\nret"),
    Case("rmw_mem", f"mov ebx, {D}+0x100\nmov eax, dword ptr [ebx+8]\nadd dword ptr [ebx+12], eax\n"
                    "sub dword ptr [ebx+16], ecx\nxor dword ptr [ebx+20], edx\nret"),
    Case("logic_unary", "and eax, ecx\nor edx, 0xff00\nxor ebx, esi\nneg edi\nnot esi\ninc ecx\ndec edx\nret"),
    Case("movss_addss", f"mov dword ptr [{D}+0x20], 0x40490fdb\nmovss xmm0, dword ptr [{D}+0x20]\n"
                        f"addss xmm0, xmm1\nmovss dword ptr [{D}+0x40], xmm0\nret", xmm=F01),
    Case("mulss", "mulss xmm0, xmm1\nsubss xmm1, xmm0\nret", xmm=F01),
    Case("cvtsi2ss", "cvtsi2ss xmm0, eax\ncvtsi2ss xmm1, dword ptr [0x300000]\nret"),
    Case("cvttss2si", "cvttss2si eax, xmm0\ncvttss2si edx, xmm1\nret",
         xmm={0: fl(1234.5678), 1: fl(-77.9)}),
    Case("movaps_addps", f"movaps xmm2, xmm0\naddps xmm2, xmm1\nmovaps xmmword ptr [{D}+0x40], xmm2\n"
                         f"movaps xmm3, xmmword ptr [{D}+0x40]\nmulps xmm3, xmm0\nret", xmm=F01),
    Case("shufps", "movaps xmm2, xmm0\nshufps xmm2, xmm1, 0x1b\nshufps xmm0, xmm0, 0xe4\nshufps xmm1, xmm2, 0x55\nret",
         xmm=F01),
    Case("ucomiss_sbb", "ucomiss xmm2, xmm1\nsbb eax, eax\nand eax, 1\nret",
         xmm={1: fl(2.0), 2: fl(1.0)}),
    Case("ucomiss_sbb_ge", "ucomiss xmm2, xmm1\nsbb eax, eax\nand eax, 1\nret",
         xmm={1: fl(1.0), 2: fl(2.0)}),
    Case("comisd_setb", "comisd xmm0, xmm1\nsetb bl\nsetae dl\nret",
         xmm={0: dbl(1.0), 1: dbl(2.0)}),
    # ebp-relative locals in a frame-pointer function are plain accesses.
    Case("frame_locals", "push ebp\nmov ebp, esp\nsub esp, 8\nmov dword ptr [ebp-4], eax\n"
                         "mov dword ptr [ebp-8], ecx\nmov edx, dword ptr [ebp-4]\nadd edx, dword ptr [ebp-8]\n"
                         "mov esp, ebp\npop ebp\nret"),
]

_OVERFLOW = "mov eax, 0x7fffffff\nadd eax, 1\n"

BUG_CASES = [
    Case("ebp_clobber", "add ebp, ecx\nxor ebp, edx\nret",
         xfail="lifter bug: leaf-function ebp is a local seeded from g_seh_ebp and never written back to g_ebp"),
]

SSE2_CASES = [
    Case("pshufd", "pshufd xmm0, xmm1, 0x1b\nret"),
    Case("movdqa", f"movdqa xmmword ptr [{D}+0x40], xmm0\nmovdqa xmm2, xmmword ptr [{D}+0x40]\nret"),
    Case("movdqu", f"movdqu xmmword ptr [{D}+0x41], xmm0\nmovdqu xmm2, xmmword ptr [{D}+0x41]\nret"),
    Case("paddd", "paddd xmm0, xmm1\nret"),
    Case("punpckldq", "punpckldq xmm0, xmm1\nret"),
    Case("pmuludq", "pmuludq xmm0, xmm1\nret"),
    Case("pcmpeqd", "pcmpeqd xmm0, xmm0\npcmpeqd xmm1, xmm2\nret"),
    Case("pslld", "pslld xmm0, 5\nret"),
    Case("psrad", "psrad xmm0, 5\nret"),
    Case("pinsrw", "pinsrw xmm0, eax, 3\nret"),
    Case("pextrw", "pextrw eax, xmm0, 3\nret"),
    Case("cvtdq2ps", "cvtdq2ps xmm0, xmm1\nret"),
    Case("pshuflw", "pshuflw xmm4, xmm4, 8\npshuflw xmm1, xmm2, 0x1b\nret"),
    Case("pshuflw_mem", f"pshuflw xmm1, xmmword ptr [{D}+0x40], 0x93\nret"),
    Case("movlpd", f"movlpd qword ptr [{D}+0x44], xmm4\nmovlpd xmm2, qword ptr [{D}+0x80]\nret"),
    Case("cvtps2pd_addpd_cvtpd2ps",
         "cvtps2pd xmm1, xmm0\naddpd xmm1, xmm7\ncvtpd2ps xmm1, xmm1\nmovhlps xmm0, xmm0\n"
         "cvtps2pd xmm0, xmm0\naddpd xmm0, xmm7\ncvtpd2ps xmm0, xmm0\nmovlhps xmm1, xmm0\nret",
         xmm={0: fl(1.5, -2.25, 3.0, 1e6), 7: dbl(0.125, -40.5)}),
    # Memory is seeded with finite values: random bytes can be NaN or denormal.
    Case("cvtps2pd_mem",
         f"mov dword ptr [{D}+0x20], 0x40490fdb\nmov dword ptr [{D}+0x24], 0xc0a00000\n"
         f"cvtps2pd xmm3, qword ptr [{D}+0x20]\nret"),
    Case("addpd_mem",
         f"mov dword ptr [{D}+0x40], 0\nmov dword ptr [{D}+0x44], 0x3ff80000\n"
         f"mov dword ptr [{D}+0x48], 0\nmov dword ptr [{D}+0x4c], 0xc0040000\n"
         f"addpd xmm7, xmmword ptr [{D}+0x40]\nret",
         xmm={7: dbl(2.5, 100.25)}),
    Case("seto", f"{_OVERFLOW}seto bl\nret", regs={"ebx": 0}),
    Case("cmovo", f"{_OVERFLOW}cmovo ecx, edx\nret", regs={"ecx": 1, "edx": 2}),
]

MULDIV_CASES = [
    Case("imul2_wrap", "imul eax, ebx\nret", regs={"eax": 0x7FFFFFFF, "ebx": 2}),
    Case("imul3_overflow", "imul eax, ebx, 0x10000\nret", regs={"ebx": 0x12345678}),
    Case("shl_cl33", "shl eax, cl\nret", regs={"ecx": 33}),
    Case("shl_cl32", "shl eax, cl\nret", regs={"ecx": 32}),
    Case("shr_cl32", "shr eax, cl\nret", regs={"ecx": 32}),
    Case("shl8_cl9", "shl al, cl\nret", regs={"ecx": 9}),
    Case("shl8_cl17", "shl al, cl\nret", regs={"ecx": 17}),
    Case("shl8_cl31", "shl al, cl\nret", regs={"ecx": 31}),
    Case("shr16_cl17", "shr ax, cl\nret", regs={"ecx": 17}),
    Case("shr16_cl31", "shr ax, cl\nret", regs={"ecx": 31}),
    Case("shl16_cl16", "shl ax, cl\nret", regs={"ecx": 16}),
    Case("shl16_cl16_cf", "shl ax, cl\nsetc dl\nret", regs={"ecx": 16}),
    Case("shl8_cl8_cf", "shl al, cl\nsetc dl\nret", regs={"ecx": 8}),
    Case("shr16_cl16_cf", "shr ax, cl\nsetc dl\nret", regs={"ecx": 16}),
    Case("shr8_cl3_cf", "shr al, cl\nsetc dl\nret", regs={"ecx": 3}),
    Case("shl8_cl3_cf", "shl al, cl\nsetc dl\nret", regs={"ecx": 3}),
    Case("xchg_mem32", f"xchg dword ptr [{D}+8], eax\nret"),
    Case("xchg_mem8", f"xchg byte ptr [{D}+3], cl\nret"),
    Case("xchg_mem16", f"xchg dx, word ptr [{D}+6]\nmfence\nret"),
    Case("mul8", "mul bl\nret", regs={"eax": 0xA1B2C3F5, "ebx": 0x99887766}),
    Case("div8", "div bl\nret", regs={"eax": 0xAABB0123, "ebx": 0x99887707}),
    Case("div16", "div bx\nret", regs={"eax": 0xBBBB0002, "edx": 0xAAAA0001, "ebx": 0xCCCC0100}),
    Case("mul16", "mul bx\nret", regs={"eax": 0xA1B2C3F5, "ebx": 0x9988F766}),
    Case("imul8", "imul bl\nret", regs={"eax": 0xA1B2C3F5, "ebx": 0x99887786}),
    Case("imul16", "imul bx\nret", regs={"eax": 0xA1B2C3F5, "ebx": 0x9988F766}),
    Case("idiv8_negative", "idiv bl\nret", regs={"eax": 0xAABBFF00, "ebx": 0x99887707}),
    Case("idiv16_negative", "idiv cx\nret",
         regs={"eax": 0x5678FF00, "edx": 0x1234FFFF, "ecx": 0x99990007}),
    # bt's CF must come from the operand at the bt, not after a later write.
    Case("bt_clobber_set", "mov ecx, 0x19\nbt ecx, eax\nmov ecx, 0\njae skip\nmov edx, 7\nskip:\nret",
         regs={"eax": 4, "edx": 0}),
    Case("bt_clobber_clear", "mov ecx, 0x19\nbt ecx, eax\nmov ecx, 0xFFFF\njae skip\nmov edx, 7\nskip:\nret",
         regs={"eax": 1, "edx": 0}),
    Case("bt_mem_string_jb", f"mov dword ptr [{D}+4], 0x10\nbt dword ptr [{D}], eax\n"
                             f"mov dword ptr [{D}+4], 0\njb hit\nmov edx, 7\nhit:\nret",
         regs={"eax": 36, "edx": 0}),
    Case("bts_setc", "mov ecx, 0x10\nbts ecx, eax\nsetc dl\nret", regs={"eax": 4, "edx": 0}),
    # Flag setters other than bt: the operand is overwritten before the jcc. The
    # overwrite is chosen so a lift that re-reads the operand takes the other branch.
    Case("inc_clobber_je", "mov ecx, -1\ninc ecx\nmov ecx, 5\nje skip\nmov edx, 7\nskip:\nret", regs={"edx": 0}),
    Case("dec_clobber_jne", "mov ecx, 1\ndec ecx\nmov ecx, 5\njne skip\nmov edx, 7\nskip:\nret", regs={"edx": 0}),
    Case("dec_clobber_js", "mov ecx, 0\ndec ecx\nmov ecx, 5\njs skip\nmov edx, 7\nskip:\nret", regs={"edx": 0}),
    Case("inc_clobber_jo", "mov ecx, 0x7fffffff\ninc ecx\nmov ecx, 0\njo skip\nmov edx, 7\nskip:\nret", regs={"edx": 0}),
    Case("dec_mem_clobber_je", f"mov dword ptr [{D}+8], 1\ndec dword ptr [{D}+8]\nmov dword ptr [{D}+8], 5\n"
                               "je skip\nmov edx, 7\nskip:\nret", regs={"edx": 0}),
    Case("imul_clobber_jo", "mov ecx, 0x10000\nimul ecx, ecx\nmov ecx, 0\njo skip\nmov edx, 7\nskip:\nret", regs={"edx": 0},
         xfail="lifter bug: 2/3-operand imul has no jo/jno condition (lifter.py:761-777 handles one-operand only), so the branch reads the unassigned _flags"),
    Case("imul_clobber_jno", "mov ecx, 3\nimul ecx, ecx\nmov ecx, 0x7fffffff\njno skip\nmov edx, 7\nskip:\nret",
         regs={"edx": 0},
         xfail="lifter bug: 2/3-operand imul has no jo/jno condition (lifter.py:761-777 handles one-operand only), so the branch reads the unassigned _flags"),
    Case("test_clobber_je", "mov ecx, 0\ntest ecx, ecx\nmov ecx, 5\nje skip\nmov edx, 7\nskip:\nret", regs={"edx": 0}),
    Case("cmp_clobber_jb", "mov ecx, 1\ncmp ecx, 2\nmov ecx, 9\njb skip\nmov edx, 7\nskip:\nret", regs={"edx": 0}),
    Case("ucomiss_clobber_jb", "ucomiss xmm0, xmm1\nmovaps xmm0, xmm3\njb skip\nmov edx, 7\nskip:\nret",
         regs={"edx": 0}, xmm={0: fl(1.0), 1: fl(2.0), 3: fl(5.0)}),
    Case("ucomiss_clobber_ja", "ucomiss xmm0, xmm1\nxorps xmm0, xmm0\nja skip\nmov edx, 7\nskip:\nret",
         regs={"edx": 0}, xmm={0: fl(3.0), 1: fl(2.0)}),
    Case("rol_clobber_jb", "mov ecx, 0x80000000\nrol ecx, 1\nmov ecx, 0\njb skip\nmov edx, 7\nskip:\nret", regs={"edx": 0},
         xfail="lifter bug: rol/ror have no jb/jae condition (lifter.py:1008-1017 returns None), so the branch reads the unassigned _flags"),
    Case("ror_clobber_jae", "mov ecx, 2\nror ecx, 1\nmov ecx, 0\njae skip\nmov edx, 7\nskip:\nret", regs={"edx": 0},
         xfail="lifter bug: rol/ror have no jb/jae condition (lifter.py:1008-1017 returns None), so the branch reads the unassigned _flags"),
    Case("cmpxchg_clobber_je", f"mov dword ptr [{D}+8], 5\nmov eax, 5\nmov ebx, 9\nlock cmpxchg dword ptr [{D}+8], ebx\n"
                               f"mov eax, 1\nmov dword ptr [{D}+8], 2\nje skip\nmov edx, 7\nskip:\nret", regs={"edx": 0}),
    Case("cmpxchg_clobber_jne", f"mov dword ptr [{D}+8], 5\nmov eax, 6\nmov ebx, 9\nlock cmpxchg dword ptr [{D}+8], ebx\n"
                                f"mov eax, 5\nmov dword ptr [{D}+8], 5\njne skip\nmov edx, 7\nskip:\nret", regs={"edx": 0}),
    # SSE2 moves between a compare and its jcc must not drop the compare.
    Case("cmp_movdqa_je", f"cmp eax, 0\nmovdqa xmm5, xmmword ptr [{D}+0x40]\nje skip\nmov edx, 7\nskip:\nret",
         regs={"eax": 0, "edx": 0}),
    # Padding after a jmp falls through into a setcc block it never reaches.
    Case("jne_padding_setl", "cmp eax, ecx\njne target\nmov edx, 9\njmp done\nnop dword ptr [eax + eax]\n"
                             "target:\nsetl dl\ndone:\nret", regs={"eax": 1, "ecx": 2, "edx": 0}),
    Case("cmp_movdqu_pmuludq_jb", f"cmp eax, 1\nmovdqu xmm2, xmmword ptr [{D}+0x41]\npmuludq xmm0, xmm1\n"
                                  "jb skip\nmov edx, 7\nskip:\nret", regs={"eax": 0, "edx": 0}),
    # Random xmm2 bytes give a mix of set and clear mask bits.
    Case("maskmovdqu", "maskmovdqu xmm0, xmm2\nret", regs={"edi": 0x300043}),
]

CASES = HANDLED + BUG_CASES + SSE2_CASES + MULDIV_CASES
SEEDS = (1, 2)
