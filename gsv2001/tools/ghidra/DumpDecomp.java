// Post-analysis: every function's decompiled C, each headed by its address and size.
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.*;

public class DumpDecomp extends GhidraScript {
    @Override public void run() throws Exception {
        String out = getScriptArgs().length > 0 ? getScriptArgs()[0] : "decomp.c";
        DecompInterface d = new DecompInterface(); d.openProgram(currentProgram);
        int n = 0;
        try (PrintWriter w = new PrintWriter(new FileWriter(out))) {
            for (Function f : currentProgram.getListing().getFunctions(true)) {
                DecompileResults r = d.decompileFunction(f, 60, new ConsoleTaskMonitor());
                w.println("/* ==== " + f.getName() + " @ " + f.getEntryPoint() + "  size " + f.getBody().getNumAddresses() + " */");
                w.println(r != null && r.getDecompiledFunction() != null ? r.getDecompiledFunction().getC() : "/* decompile failed */");
                n++;
            }
        }
        println("DumpDecomp: " + n + " functions -> " + out);
    }
}
