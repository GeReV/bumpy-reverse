// Decompile every defined function and write one <name>@<seg_off>.c file
// under the directory given as arg[0]. @category Bumpy
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.listing.Function;
import java.io.File;
import java.io.FileWriter;

public class ExportDecomp extends GhidraScript {
    @Override
    public void run() throws Exception {
        String outDir = getScriptArgs().length > 0 ? getScriptArgs()[0] : "decomp";
        File dir = new File(outDir);
        dir.mkdirs();
        DecompInterface d = new DecompInterface();
        d.openProgram(currentProgram);
        int ok = 0, fail = 0;
        for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
            String tag = f.getEntryPoint().toString().replace(':', '_');
            File out = new File(dir, f.getName() + "@" + tag + ".c");
            DecompileResults r = d.decompileFunction(f, 180, monitor);
            try (FileWriter w = new FileWriter(out)) {
                if (r != null && r.getDecompiledFunction() != null) {
                    w.write(r.getDecompiledFunction().getC());
                    ok++;
                } else {
                    w.write("/* decompile failed: " + f.getName() + " */\n");
                    fail++;
                }
            }
        }
        println("ExportDecomp: ok=" + ok + " fail=" + fail + " -> " + dir.getAbsolutePath());
    }
}
