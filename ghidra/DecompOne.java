// Decompile functions and show what the decompiler assumes about their callees.
// args: outFile addr [addr...] [EXP=op:addr[:val]/op:addr[:val]...]
// EXP mutates the (read-only) program step by step and reports, after each
// step, whether the first function's decompile still contains the bad text.
//   ops: cc (set calling convention), purge, nolock (signature source DEFAULT),
//        varargs0, bad:<text to look for>
// @category DaytonaUSA
import ghidra.app.decompiler.*;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.*;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.util.*;

public class DecompOne extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        PrintWriter out = new PrintWriter(new FileWriter(a[0]));
        DecompInterface d = new DecompInterface();
        d.setOptions(new DecompileOptions());
        d.openProgram(currentProgram);
        List<String> addrs = new ArrayList<>();
        String exp = null;
        for (int i = 1; i < a.length; i++) {
            if (a[i].equals("EXP") && i + 1 < a.length) exp = a[++i]; else if (a[i].startsWith("EXP=")) exp = a[i].substring(4); else addrs.add(a[i]);
        }
        if (exp != null && !addrs.isEmpty()) {
            Function f = getFunctionAt(toAddr(Long.parseLong(addrs.get(0), 16)));
            String bad = "BROKEN";
            for (String t : exp.split("/")) {
                String[] kv = t.split(":");
                if (kv[0].equals("bad")) { bad = kv[1]; continue; }
                Function g = getFunctionAt(toAddr(Long.parseLong(kv[1], 16)));
                if (kv[0].equals("cc")) g.setCallingConvention(kv[2]);
                if (kv[0].equals("purge")) g.setStackPurgeSize(Integer.parseInt(kv[2]));
                if (kv[0].equals("nolock")) g.setSignatureSource(SourceType.DEFAULT);
                if (kv[0].equals("varargs0")) g.setVarArgs(false);
                if (kv[0].equals("noparams")) { g.replaceParameters(new ArrayList<Variable>(), Function.FunctionUpdateType.DYNAMIC_STORAGE_ALL_PARAMS, true, SourceType.DEFAULT); g.setReturnType(ghidra.program.model.data.Undefined.getUndefinedDataType(1), SourceType.DEFAULT); }
                d.closeProgram();
                d.openProgram(currentProgram);
                DecompileResults r = d.decompileFunction(f, 60, monitor);
                String c = r.getDecompiledFunction() != null ? r.getDecompiledFunction().getC() : "";
                out.println("--- after " + t + ": " + (c.contains(bad) ? "still bad" : "OK"));
            }
        }
        for (String s : addrs) {
            Function ff = getFunctionAt(toAddr(Long.parseLong(s, 16)));
            if (ff != null) {
                InstructionIterator ii = currentProgram.getListing().getInstructions(ff.getBody(), true);
                while (ii.hasNext()) {
                    Instruction ins = ii.next();
                    if (ins.getFlowOverride() != ghidra.program.model.listing.FlowOverride.NONE || ins.isFallThroughOverridden()
                        || getFunctionAt(ins.getAddress()) != null && !ins.getAddress().equals(ff.getEntryPoint()))
                        out.println("   insn " + ins.getAddress() + " " + ins + " override=" + ins.getFlowOverride() + " fallthroughOverridden=" + ins.isFallThroughOverridden());
                    if (ins.getMnemonicString().equals("CALL")) out.println("   call " + ins.getAddress() + " " + ins + " depthchange=" + ghidra.app.cmd.function.CallDepthChangeInfo.getStackDepthChange(currentProgram, ins.getAddress()));
                }
            }
            Function f = getFunctionAt(toAddr(Long.parseLong(s, 16)));
            if (f == null) { out.println("no function at " + s); continue; }
            out.println("== " + f.getName() + " cc=" + f.getCallingConventionName() + " purge=" + f.getStackPurgeSize()
                + " sig=" + f.getSignature().getPrototypeString() + " src=" + f.getSignatureSource());
            for (Variable v : f.getLocalVariables())
                out.println("   local " + v.getName() + " " + v.getDataType().getName() + " @" + v.getVariableStorage());
            for (Function c : f.getCalledFunctions(monitor))
                out.println("   calls " + c.getName() + " cc=" + c.getCallingConventionName() + " purge=" + c.getStackPurgeSize()
                    + " varargs=" + c.hasVarArgs() + " src=" + c.getSignatureSource() + " sig=" + c.getSignature().getPrototypeString());
            DecompileResults r = d.decompileFunction(f, 60, monitor);
            out.println(r.getDecompiledFunction() != null ? r.getDecompiledFunction().getC() : r.getErrorMessage());
        }
        out.close();
    }
}
