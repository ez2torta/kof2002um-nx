// Decompila UNA funcion y lista sus llamadores/llamados. Uso: -postScript DecompOne.java 8217A3B8 [out.txt]
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import java.io.*;

public class DecompOne extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        Address a = toAddr(Long.parseLong(args[0], 16));
        // Los helpers __savegprlr/__restgprlr (0x82725200-0x82725400) quedaron marcados noreturn
        // y cortan todas las funciones; se corrige solo en memoria (-readOnly no guarda).
        for (Function h : currentProgram.getFunctionManager().getFunctions(toAddr(0x82725200L), true)) {
            if (h.getEntryPoint().getOffset() > 0x82725400L) break;
            h.setNoReturn(false);
        }
        Function f = getFunctionContaining(a);
        StringBuilder sb = new StringBuilder();
        if (f == null) { println("no function at " + a); return; }
        if (!f.getEntryPoint().equals(a)) f = getFunctionAt(a);
        ghidra.app.cmd.function.CreateFunctionCmd.fixupFunctionBody(currentProgram, f, monitor);
        sb.append("// ").append(f.getName()).append(" @ ").append(f.getEntryPoint())
          .append(" size=").append(f.getBody().getNumAddresses()).append("\n");
        for (Function c : f.getCallingFunctions(monitor)) sb.append("// caller: ").append(c.getEntryPoint()).append("\n");
        for (Function c : f.getCalledFunctions(monitor)) sb.append("// calls: ").append(c.getEntryPoint()).append(" ").append(c.getName()).append("\n");
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        DecompileResults r = di.decompileFunction(f, 120, monitor);
        sb.append(r.decompileCompleted() ? r.getDecompiledFunction().getC() : ("// FAILED: " + r.getErrorMessage()));
        String out = args.length > 1 ? args[1] : null;
        if (out != null) try (PrintWriter w = new PrintWriter(out)) { w.print(sb); }
        else println(sb.toString());
    }
}
