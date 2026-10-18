# Upstream

Source: https://github.com/sp00nznet/xboxrecomp (MIT, see LICENSE and NOTICE)
Commit: eb0aa1d5fcb1ca34cdb6470fcfa471d87f431d73

Copied unchanged except as listed below:
- tools/recomp/{lifter.py,translator.py,disasm.py,config.py,__init__.py} -> tools/xrecomp/
- templates/runtime/recomp_types.h -> tools/xrecomp/include/

## Local changes

config.py
- Added `configure_from_macho(analysis, image_path)`, `MACHO_CODE_SECTIONS`
  (`__text`, `__stub_helper`), `MACHO_ENTRY_POINT` (0x2720). Sections are named
  `SEG.sect`; zero-fill sections get raw_size 0.

lifter.py
- `Lifter.reg_switch_targets` ({jmp_addr: [targets]}): `jmp reg` at a known
  address emits one `if (g_reg == 0xT) goto loc_T;` per in-function target,
  then `RECOMP_ITAIL(g_reg)`. Placed before the upstream intra-function
  `jmp reg` branch.
- PIC pair: `call $+5` followed by a one-byte `pop r32` lifts the call to
  `g_<r> = 0x<next>u;` and the pop to a comment (`_pic_pop_reg`, `_is_pic_pop`,
  `_image_bytes`). Both halves are decided from the image bytes.
- `_lift_muldiv` comment-only fallback (`/* m: unhandled */`) is now counted
  in `self.unimplemented`.

translator.py
- `BatchTranslator(..., image_format="xbe", reg_switch_json_path=None,
  import_stubs=None)`. With `image_format="macho"`: no XBE banner
  (`xbe_title`), no SEH and no setjmp/longjmp detection.
- Loads reg_switches.json into `Lifter.reg_switch_targets`.
- `_control_flow_census` adds those switch arms as computed edges, so they
  become block leaders and labels.
- `translate_batch_split`: records a failure reason per function
  (`stats["failures"]`), excludes import stubs from the unresolved-call stubs,
  and includes `imports.h` from the generated header.
- recomp_types.h is copied from `tools/xrecomp/include/`.

M1-c2 (SSE2 integer and overflow flag)
- lifter.py `_lift_sse`: movdqa/movdqu (as movaps), movq (xmm forms), pshufd,
  pinsrw/pextrw, padd/psub b/w/d/q, pcmpeq/pcmpgt b/w/d, punpckl/h bw/wd/dq/qdq,
  pmuludq, psll/psrl w/d/q, psraw/d, pslldq/psrldq, pand/pandn/por/pxor,
  packuswb/packsswb/packssdw, cvtdq2ps. Tables `_SSE2_INT_BINARY`, `_SSE2_SHIFT`.
- OF: `_make_condition` derives jo/jno (hence seto/setno/cmovo/cmovno) from the
  cmp/add/sub snapshot, clears it for and/or/xor, and reads `_fb` for mul and
  one-operand imul, whose lowering now publishes OF=CF there. mul now stays a
  tracked setter (it was in `_FLAGS_UNDEFINED`); only jo/jno are answered.
  Two-/three-operand imul and any other setter leave seto/cmovo unhandled.
- pushfd builds EFLAGS (CF PF ZF SF OF, DF, IF) from the tracked setter; when
  no setter is known, pushfd/popfd push/pop the word (stack stays balanced)
  and stay counted as unhandled; popfd restores DF only.
- translator.py: `_fa/_fb` are also declared when a function has mul or imul.
- recomp_types.h: RecompXmm gains b/sb/w/sw views; XMM_P* helpers,
  RECOMP_OF_SUB/RECOMP_OF_ADD.
- run.py: `--only @FILE` reads the address list from a file.

New files: run.py (CLI), UPSTREAM.md. Inputs come from tools/hobbit/emit_inputs.py.

## Known gaps (not handled in this slice)
- Fences: sfence/lfence/mfence lift to `(void)0`; MEM macros are volatile only,
  no acquire/release ordering.
- Atomics: only `lock xadd` / `lock cmpxchg` on 32-bit memory become atomics;
  other locked ops and memory `xchg` are plain.
- x87: 64-bit double, not 80-bit extended precision.
- Memory base: upstream `g_xbox_mem_offset` kept, names unchanged.
- gs: TLS (Mach-O i386) is not mapped; the fs: fake-TIB path is upstream's.

- translator.py `_function_needs_cf`: `lahf` counts as a carry reader (it probes `jb`), so `_cf` is declared. Fixes 4 compile errors in M2.
