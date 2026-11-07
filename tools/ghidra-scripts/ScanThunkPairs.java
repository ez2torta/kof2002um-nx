// Busca thunks de 2 instrucciones "<insn>; b X" pegados detrás de un terminador
// (blr / bctr / b) en .text. Saca "ADDR WORD0 TARGET" por línea.
// Uso: -postScript ScanThunkPairs.java out.txt
import ghidra.app.script.GhidraScript;
import ghidra.program.model.mem.*;
import java.io.*;

public class ScanThunkPairs extends GhidraScript {
    static boolean isTerm(int w) {
        if (w == 0x4E800020 || w == 0x4E800420) return true;          // blr, bctr
        return ((w >>> 26) == 18) && ((w & 3) == 0);                    // b (no link, rel)
    }
    static boolean isB(int w) { return ((w >>> 26) == 18) && ((w & 3) == 0); }

    @Override
    public void run() throws Exception {
        String out = getScriptArgs()[0];
        MemoryBlock b = currentProgram.getMemory().getBlock(".text");
        long s = b.getStart().getOffset(), e = b.getEnd().getOffset();
        StringBuilder sb = new StringBuilder();
        int prev = getInt(toAddr(s));
        for (long p = s + 4; p + 8 <= e; p += 4) {
            int w0 = getInt(toAddr(p)), w1 = getInt(toAddr(p + 4));
            if (isTerm(prev) && w0 != 0 && !isTerm(w0) && isB(w1)) {
                int off = (w1 & 0x03FFFFFC);
                if ((off & 0x02000000) != 0) off |= 0xFC000000;
                long tgt = (p + 4 + off) & 0xFFFFFFFFL;
                sb.append(String.format("%08X %08X %08X%n", p, w0, tgt));
            }
            prev = w0;
        }
        try (PrintWriter pw = new PrintWriter(out)) { pw.print(sb); }
    }
}
