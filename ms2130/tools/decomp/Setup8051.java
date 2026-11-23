// Before Ghidra's analysis of the patched 8051 image (ms8051.py prepare): seed everything the flow trace found.
// Arguments: entries.txt (call targets; "addr -" = disassemble only), insn.txt (every instruction start),
// inline.txt (calls still followed by 4 bytes of data: "call data next").
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.mem.*;
import ghidra.program.model.symbol.SourceType;
import java.io.*;
import java.nio.file.*;
import java.util.List;

public class Setup8051 extends GhidraScript {
    private void entry(long a, String name, boolean fn) throws Exception {
        Address h = toAddr(a);
        disassemble(h);
        if (!fn) return;
        if (getFunctionAt(h) == null) createFunction(h, name);
        else if (name != null) getFunctionAt(h).setName(name, SourceType.USER_DEFINED);
    }
    @Override public void run() throws Exception {
        String[] args = getScriptArgs();
        // The Keil helpers pop their return address, which Ghidra reads as "never returns"; and the rewritten
        // helper calls jump out to the stub area and back, which it would otherwise split off as shared tails.
        setAnalysisOption(currentProgram, "Non-Returning Functions - Discovered", "false");
        setAnalysisOption(currentProgram, "Shared Return Calls", "false");
        if (args.length > 2 && new File(args[2]).exists()) {
            List<String> ls = Files.readAllLines(Paths.get(args[2]));
            for (String l : ls) {           // all the data first: disassembly must stop at each of them
                String[] p = l.trim().split("\\s+"); if (p.length < 3) continue;
                Address d = toAddr(Long.parseLong(p[1], 16));
                for (int i = 0; i < 4; i++) createByte(d.add(i));
            }
            for (String l : ls) {
                String[] p = l.trim().split("\\s+"); if (p.length < 3) continue;
                Address c = toAddr(Long.parseLong(p[0], 16)), n = toAddr(Long.parseLong(p[2], 16));
                disassemble(c);
                getInstructionAt(c).setFallThrough(n);
                disassemble(n);
            }
        }
        Memory mem = currentProgram.getMemory();
        String[] vn = {"reset", "int0", "timer0", "int1", "timer1"};
        long[] va = {0x00, 0x03, 0x0b, 0x13, 0x1b};
        for (int i = 0; i < va.length; i++) {
            if ((mem.getByte(toAddr(va[i])) & 0xff) != 0x02) continue;
            long t = ((mem.getByte(toAddr(va[i] + 1)) & 0xff) << 8) | (mem.getByte(toAddr(va[i] + 2)) & 0xff);
            entry(va[i], null, true);
            entry(t, "handler_" + vn[i], true);
        }
        if (args.length > 0 && new File(args[0]).exists()) {
            for (String l : Files.readAllLines(Paths.get(args[0]))) {
                String[] p = l.trim().split("\\s+"); if (p[0].isEmpty()) continue;
                entry(Long.parseLong(p[0], 16), null, !(p.length > 1 && p[1].equals("-")));
            }
        }
        if (args.length > 1 && new File(args[1]).exists()) {
            for (String l : Files.readAllLines(Paths.get(args[1]))) {
                l = l.trim(); if (l.isEmpty()) continue;
                Address a = toAddr(Long.parseLong(l, 16));
                if (getInstructionContaining(a) == null && (getDataContaining(a) == null || !getDataContaining(a).isDefined())) disassemble(a);
            }
        }
        println("Setup8051 done");
    }
}
