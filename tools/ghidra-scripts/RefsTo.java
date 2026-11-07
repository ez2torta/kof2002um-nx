// Lista todas las referencias (codigo y datos) a cada direccion. Uso: -postScript RefsTo.java out.txt 8290EB58 ...
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import java.io.*;

public class RefsTo extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        StringBuilder sb = new StringBuilder();
        for (int i = 1; i < args.length; i++) {
            Address a = toAddr(Long.parseLong(args[i], 16));
            sb.append("== ").append(args[i]).append("\n");
            for (Reference r : getReferencesTo(a)) {
                Function f = getFunctionContaining(r.getFromAddress());
                sb.append("  ").append(r.getFromAddress()).append(" ").append(r.getReferenceType())
                  .append(f != null ? " in " + f.getEntryPoint() : " (data)").append("\n");
            }
        }
        try (PrintWriter w = new PrintWriter(args[0])) { w.print(sb); }
    }
}
