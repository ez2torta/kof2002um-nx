// Lista instrucciones (o bytes crudos) de un rango. Uso: -postScript ListRange.java 8270EB00 8270EF48 out.txt
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import java.io.*;

public class ListRange extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        long s = Long.parseLong(a[0], 16), e = Long.parseLong(a[1], 16);
        StringBuilder sb = new StringBuilder();
        sb.append("block: ").append(currentProgram.getMemory().getBlock(toAddr(s))).append("\n");
        Listing l = currentProgram.getListing();
        for (long p = s; p < e; p += 4) {
            Address ad = toAddr(p);
            Function f = getFunctionAt(ad);
            if (f != null) sb.append("---- function ").append(f.getName()).append("\n");
            Instruction ins = l.getInstructionAt(ad);
            int w = getInt(ad);
            String txt = ins != null ? ins.toString() : "(no insn)";
            sb.append(String.format("%08X  %08X  %s%n", p, w, txt));
        }
        try (PrintWriter pw = new PrintWriter(a[2])) { pw.print(sb); }
    }
}
