// After analysis: listing.txt (every instruction with the function it belongs to), funcs.txt (functions with
// their callers) and decomp.c (decompiled C), written to the directory given as the first argument.
// Addresses from 0xD000 up are the rewritten helper calls (ms8051.py), not original code.
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.*;
import ghidra.program.model.symbol.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.*;

public class Dump8051 extends GhidraScript {
    @Override public void run() throws Exception {
        String dir = getScriptArgs().length > 0 ? getScriptArgs()[0] : ".";
        Listing L = currentProgram.getListing();
        try (PrintWriter w = new PrintWriter(new FileWriter(dir + "/listing.txt"))) {
            AddressSpace cs = currentProgram.getAddressFactory().getDefaultAddressSpace();
            InstructionIterator it = L.getInstructions(cs.getAddress(0), true);
            while (it.hasNext()) {
                Instruction i = it.next();
                if (!i.getAddress().getAddressSpace().equals(cs)) continue;
                Function f = getFunctionAt(i.getAddress());
                if (f != null) w.println("; ---- " + f.getName());
                StringBuilder b = new StringBuilder();
                for (byte x : i.getBytes()) b.append(String.format("%02x", x & 0xff));
                Function in = getFunctionContaining(i.getAddress());
                w.println(String.format("%04x  %-8s %-28s ; %s", i.getAddress().getOffset(), b, i.toString(), in != null ? in.getName() : "-"));
            }
        }
        try (PrintWriter w = new PrintWriter(new FileWriter(dir + "/funcs.txt"))) {
            for (Function f : L.getFunctions(true)) {
                StringBuilder c = new StringBuilder();
                int n = 0;
                for (Reference r : getReferencesTo(f.getEntryPoint())) {
                    if (!r.getReferenceType().isCall() && !r.getReferenceType().isJump()) continue;
                    Function cf = getFunctionContaining(r.getFromAddress());
                    if (n++ < 12) c.append(cf != null ? cf.getName() : r.getFromAddress().toString()).append(' ');
                }
                w.println(String.format("%s %s size=%d callers=%d %s", f.getEntryPoint(), f.getName(), f.getBody().getNumAddresses(), n, c));
            }
        }
        DecompInterface d = new DecompInterface(); d.openProgram(currentProgram);
        int n = 0;
        try (PrintWriter w = new PrintWriter(new FileWriter(dir + "/decomp.c"))) {
            for (Function f : L.getFunctions(true)) {
                DecompileResults r = d.decompileFunction(f, 60, new ConsoleTaskMonitor());
                w.println("/* ==== " + f.getName() + " @ " + f.getEntryPoint() + "  size " + f.getBody().getNumAddresses() + " */");
                w.println(r != null && r.getDecompiledFunction() != null ? r.getDecompiledFunction().getC() : "/* decompile failed */");
                n++;
            }
        }
        println("Dump8051: " + n + " functions -> " + dir);
    }
}
