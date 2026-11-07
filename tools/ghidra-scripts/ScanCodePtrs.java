// Busca palabras de 32 bits en bloques de datos (no ejecutables) que apunten a .text
// justo despues de un terminador (blr / bctr / b) o a un "mflr r12": candidatos a
// funciones que solo se alcanzan por puntero (vtables, structs de callbacks).
// Uso: -postScript ScanCodePtrs.java out.txt
// Salida por linea: TARGET  PREV_WORD  FIRST_WORD  REF_ADDR  BLOCK
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.mem.*;
import java.io.*;
import java.util.*;

public class ScanCodePtrs extends GhidraScript {
    @Override
    public void run() throws Exception {
        String out = getScriptArgs()[0];
        Memory mem = currentProgram.getMemory();
        MemoryBlock text = null;
        for (MemoryBlock b : mem.getBlocks()) if (b.getName().equals(".text")) text = b;
        long ts = text.getStart().getOffset(), te = text.getEnd().getOffset();
        TreeMap<Long, String> hits = new TreeMap<>();
        for (MemoryBlock b : mem.getBlocks()) {
            if (b.isExecute() || !b.isInitialized()) continue;
            long s = (b.getStart().getOffset() + 3) & ~3L, e = b.getEnd().getOffset();
            byte[] buf = new byte[(int) (e - s + 1)];
            b.getBytes(toAddr(s), buf);
            for (int i = 0; i + 4 <= buf.length; i += 4) {
                long w = ((buf[i] & 0xFFL) << 24) | ((buf[i + 1] & 0xFFL) << 16) | ((buf[i + 2] & 0xFFL) << 8) | (buf[i + 3] & 0xFFL);
                if (w < ts || w > te || (w & 3) != 0 || hits.containsKey(w)) continue;
                int first = mem.getInt(toAddr(w));
                int prev = w - 4 >= ts ? mem.getInt(toAddr(w - 4)) : 0;
                boolean term = prev == 0x4E800020 || prev == 0x4E800420 || ((prev >>> 26) == 18 && (prev & 1) == 0)
                        || prev == 0 ;
                boolean prologue = first == 0x7D8802A6;
                if (!term && !prologue) continue;
                if (first == 0) continue;
                hits.put(w, String.format("%08X  %08X  %08X  %08X  %s", w, prev, first, s + i, b.getName()));
            }
        }
        try (PrintWriter pw = new PrintWriter(out)) { for (String v : hits.values()) pw.println(v); }
        println("ScanCodePtrs: " + hits.size() + " candidatos");
    }
}
