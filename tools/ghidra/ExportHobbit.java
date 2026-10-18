// Exports function metadata and decompiled C for selected ranges.
// Usage: -postScript ExportHobbit.java <outdir>
// Writes <outdir>/recon/ghidra_functions.json and <outdir>/docs/research/pseudocode/*.c
// @category Hobbit
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;

import com.google.gson.Gson;
import com.google.gson.GsonBuilder;

import java.io.File;
import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

public class ExportHobbit extends GhidraScript {

    private static final long[][] RANGES = {
        {0x3db000L, 0x3e2000L}, {0x3b0000L, 0x3b3000L}, {0x3ee000L, 0x3f1000L}
    };
    private static final String[] NAMES = {
        "frontend_3db000.c", "audio_3b0000.c", "cg_3ee000.c"
    };

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 1) {
            throw new IllegalArgumentException("usage: ExportHobbit.java <outdir>");
        }
        File outDir = new File(args[0]);
        File pseudoDir = new File(outDir, "docs/research/pseudocode");
        File reconDir = new File(outDir, "recon");
        pseudoDir.mkdirs();
        reconDir.mkdirs();

        List<Map<String, Object>> rows = new ArrayList<>();
        List<Function> all = new ArrayList<>();
        FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
        while (it.hasNext()) {
            Function f = it.next();
            all.add(f);
            Map<String, Object> row = new LinkedHashMap<>();
            row.put("addr", f.getEntryPoint().getOffset());
            row.put("name", f.getName());
            row.put("signature", f.getPrototypeString(false, false));
            row.put("size", f.getBody().getNumAddresses());
            row.put("is_thunk", f.isThunk());
            rows.add(row);
        }
        Gson gson = new GsonBuilder().disableHtmlEscaping().create();
        try (PrintWriter w = new PrintWriter(new File(reconDir, "ghidra_functions.json"), StandardCharsets.UTF_8)) {
            w.println(gson.toJson(rows));
        }
        println("functions: " + rows.size());

        DecompInterface ifc = new DecompInterface();
        ifc.openProgram(currentProgram);
        try {
            for (int r = 0; r < RANGES.length; r++) {
                long lo = RANGES[r][0], hi = RANGES[r][1];
                int n = 0, failed = 0;
                try (PrintWriter w = new PrintWriter(new File(pseudoDir, NAMES[r]), StandardCharsets.UTF_8)) {
                    w.println(String.format("// Decompiled by Ghidra: range 0x%x-0x%x", lo, hi));
                    w.println();
                    for (Function f : all) {
                        long a = f.getEntryPoint().getOffset();
                        if (a < lo || a >= hi) {
                            continue;
                        }
                        n++;
                        w.println(String.format("// ==== 0x%x %s ====", a, f.getName()));
                        DecompileResults res = ifc.decompileFunction(f, 30, monitor);
                        if (res != null && res.decompileCompleted() && res.getDecompiledFunction() != null) {
                            w.println(res.getDecompiledFunction().getC());
                        } else {
                            failed++;
                            String msg = res == null ? "null result" : res.getErrorMessage();
                            w.println("// DECOMPILE FAILED: " + msg);
                            w.println();
                        }
                    }
                }
                println(String.format("%s: %d functions, %d failed", NAMES[r], n, failed));
            }
        } finally {
            ifc.dispose();
        }
    }
}
