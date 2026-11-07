// Imprime el string C en cada direccion. Uso: -postScript DumpStr.java out.txt 82BD2FDC 82BD2FF0 ...
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import java.io.*;

public class DumpStr extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        StringBuilder sb = new StringBuilder();
        for (int i = 1; i < args.length; i++) {
            Address a = toAddr(Long.parseLong(args[i], 16));
            StringBuilder s = new StringBuilder();
            for (int k = 0; k < 256; k++) {
                byte b = getByte(a.add(k));
                if (b == 0) break;
                s.append(b >= 32 && b < 127 ? (char) b : '.');
            }
            sb.append(args[i]).append(": \"").append(s).append("\"\n");
        }
        try (PrintWriter w = new PrintWriter(args[0])) { w.print(sb); }
    }
}
