// List call-site prototype overrides inside the given functions (and count all).
// @category DaytonaUSA
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.pcode.*;
import java.io.*;

public class ListOverrides extends GhidraScript {
    static String sigText(ghidra.program.model.listing.FunctionSignature fs) {
        StringBuilder b = new StringBuilder(fs.getReturnType().getName() + " (");
        for (ghidra.program.model.data.ParameterDefinition p : fs.getArguments())
            b.append(p.getDataType().getName()).append("[").append(p.getLength()).append("] ");
        if (fs.hasVarArgs()) b.append("...");
        return b.append(") cc=").append(fs.getCallingConventionName()).toString();
    }
    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        PrintWriter out = new PrintWriter(new FileWriter(a[0]));
        int total = 0;
        SymbolIterator it = currentProgram.getSymbolTable().getAllSymbols(false);
        java.util.Map<String,Integer> byCallee = new java.util.TreeMap<>();
        while (it.hasNext()) {
            Symbol s = it.next();
            if (!s.getName().startsWith("prt")) continue;
            DataTypeSymbol ds = HighFunctionDBUtil.readOverride(s);
            if (ds == null) continue;
            total++;
            String sig = ds.getDataType().toString();
            Function f = getFunctionContaining(s.getAddress());
            String fn = f == null ? "?" : f.getName();
            Instruction ins = getInstructionAt(s.getAddress());
            String callee = ins != null ? ins.toString() : "?";
            byCallee.merge(callee.replaceAll(".*CALL ", ""), 1, Integer::sum);
            for (int i = 1; i < a.length; i++)
                if (fn.toLowerCase().contains(a[i].toLowerCase()))
                    out.println(fn + " " + s.getAddress() + " " + callee + " :: " + sigText((ghidra.program.model.listing.FunctionSignature) ds.getDataType()));
        }
        out.println("total overrides: " + total);
        for (java.util.Map.Entry<String,Integer> e : byCallee.entrySet()) out.println("  " + e.getKey() + " " + e.getValue());
        out.close();
    }
}
