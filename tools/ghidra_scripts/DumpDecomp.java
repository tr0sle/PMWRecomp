import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import java.io.*;

public class DumpDecomp extends GhidraScript {
    @Override
    public void run() throws Exception {
        String out = getScriptArgs()[0];
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        PrintWriter pw = new PrintWriter(new FileWriter(out));
        FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
        int n = 0;
        while (it.hasNext() && !monitor.isCancelled()) {
            Function f = it.next();
            if (f.isExternal()) continue;
            DecompileResults r = di.decompileFunction(f, 60, monitor);
            pw.println("//==== " + f.getEntryPoint() + " " + f.getName());
            if (r != null && r.getDecompiledFunction() != null) pw.println(r.getDecompiledFunction().getC());
            else pw.println("// decompile failed");
            n++;
        }
        pw.close();
        println("dumped " + n);
    }
}
