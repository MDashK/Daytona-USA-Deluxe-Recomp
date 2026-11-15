// Export the whole program as re-compilable C for the Daytona USA Deluxe port.
//
// Pass 0: create functions at code addresses that are only referenced from data
//         (callback / vtable entries Ghidra's analysis left as plain labels).
// Pass 1..N: decompile every function and commit the recovered prototype
//         (params + return) to the database, so every call site agrees with
//         its callee's signature in the final pass.
// Final pass: decompile again and write one record per function containing
//         the prototype, the global symbols it references (address + C
//         declaration), its stack locals (stack offset + C declaration) and
//         the C body.  Also writes types.h (all program data types).
//
// Usage (headless):
//   analyzeHeadless <projDir> <projName> -process "DAYTONA USA Deluxe.exe" -noanalysis
//       -scriptPath ghidra -postScript RecompExport.java <outDir> [commitPasses]
//
// @category DaytonaUSA

import ghidra.app.decompiler.*;
import ghidra.app.script.GhidraScript;
import ghidra.app.util.cparser.C.*;
import ghidra.program.model.address.*;
import ghidra.program.model.data.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.*;
import ghidra.program.model.pcode.*;
import ghidra.program.model.reloc.*;
import ghidra.program.model.symbol.*;

import java.io.*;
import java.util.*;

public class RecompExport extends GhidraScript {

    private DecompInterface decomp;

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        String outDir = args.length > 0 ? args[0] : "recomp_export";
        int passes = args.length > 1 ? Integer.parseInt(args[1]) : 2;
        String fixups = args.length > 2 ? args[2] : null;
        new File(outDir).mkdirs();

        createMissingFunctions();
        clearUndefinedData();
        typeGlobalsByAccess();
        renameOddFunctions();
        fixX87Helpers();
        if (fixups != null && new File(fixups).exists()) applyFixups(fixups, true);

        decomp = new DecompInterface();
        DecompileOptions opts = new DecompileOptions();
        decomp.setOptions(opts);
        decomp.toggleCCode(true);
        decomp.toggleSyntaxTree(true);
        decomp.setSimplificationStyle("decompile");
        if (!decomp.openProgram(currentProgram)) {
            printerr("decompiler init failed");
            return;
        }

        for (int p = 0; p < passes; p++) {
            int n = 0, fail = 0;
            for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
                if (monitor.isCancelled()) return;
                if (f.isExternal() || f.isThunk()) continue;
                DecompileResults r = decomp.decompileFunction(f, 180, monitor);
                if (r == null || !r.decompileCompleted() || r.getHighFunction() == null) { fail++; continue; }
                try {
                    HighFunctionDBUtil.commitParamsToDatabase(r.getHighFunction(), true,
                        HighFunctionDBUtil.ReturnCommitOption.COMMIT, SourceType.ANALYSIS);
                    n++;
                } catch (Exception e) {
                    fail++;
                }
            }
            println("commit pass " + p + ": " + n + " committed, " + fail + " failed");
            // the decompiler caches prototypes; restart it so the next pass sees the commits
            decomp.dispose();
            decomp = new DecompInterface();
            decomp.setOptions(opts);
            decomp.toggleCCode(true);
            decomp.toggleSyntaxTree(true);
            decomp.openProgram(currentProgram);
        }

        fixCallingConventions();
        removeCallSiteOverrides();
        try {
            ghidra.program.model.util.PropertyMapManager pm = currentProgram.getUsrPropertyManager();
            if (pm.getIntPropertyMap("StackDepthChange") != null) {
                pm.removePropertyMap("StackDepthChange");
                println("removed StackDepthChange overrides");
            }
        } catch (Exception e) { println("StackDepthChange: " + e); }
        if (fixups != null && new File(fixups).exists()) {
            applyFixups(fixups, false);
            decomp.dispose();
            decomp = new DecompInterface();
            decomp.setOptions(opts);
            decomp.toggleCCode(true);
            decomp.toggleSyntaxTree(true);
            decomp.openProgram(currentProgram);
        }

        PrintWriter out = new PrintWriter(new BufferedWriter(new OutputStreamWriter(
            new FileOutputStream(new File(outDir, "functions.txt")), "UTF-8")));
        int ok = 0, fail = 0;
        for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
            if (monitor.isCancelled()) break;
            if (f.isExternal()) continue;
            Address ep = f.getEntryPoint();
            out.println("@@FUNC " + hex(ep) + " " + f.getName());
            out.println("@@SIZE " + f.getBody().getNumAddresses() + " " + hex(f.getBody().getMaxAddress()));
            out.println("@@CC " + f.getCallingConventionName());
            if (f.isThunk()) {
                Function t = f.getThunkedFunction(true);
                if (t != null) out.println("@@SIG " + t.getSignature().getPrototypeString());
                out.println("@@THUNK " + (t != null ? t.getName() + (t.isExternal() ? " EXTERNAL" : " " + hex(t.getEntryPoint())) : "?"));
                out.println("@@END");
                continue;
            }
            out.println("@@SIG " + f.getSignature().getPrototypeString());
            if (f.hasNoReturn()) out.println("@@NORETURN");
            DecompileResults r = decomp.decompileFunction(f, 180, monitor);
            if (r == null || !r.decompileCompleted() || r.getDecompiledFunction() == null) {
                out.println("@@FAIL " + (r != null ? r.getErrorMessage().replace('\n', ' ') : "null"));
                out.println("@@END");
                fail++;
                continue;
            }
            HighFunction hf = r.getHighFunction();
            if (hf != null) {
                Iterator<HighSymbol> gi = hf.getGlobalSymbolMap().getSymbols();
                while (gi.hasNext()) {
                    HighSymbol s = gi.next();
                    Address a = null;
                    try { a = s.getStorage().getMinAddress(); } catch (Exception e) { }
                    DataType dt = s.getDataType();
                    HighVariable hv = s.getHighVariable();
                    if (hv != null && hv.getDataType() != null && hv.getSize() >= s.getSize()) dt = hv.getDataType();
                    out.println("@@GLOBAL " + (a != null ? hex(a) : "?") + " " + s.getSize() + " " +
                        s.getName() + " :: " + decl(dt, s.getName()));
                }
                Iterator<HighSymbol> li = hf.getLocalSymbolMap().getSymbols();
                while (li.hasNext()) {
                    HighSymbol s = li.next();
                    VariableStorage st = s.getStorage();
                    String where;
                    if (st == null) where = "?";
                    else if (st.isStackStorage()) where = "stack:" + st.getStackOffset();
                    else if (st.isRegisterStorage()) where = "reg:" + st.getRegister().getName();
                    else if (st.isUniqueStorage()) where = "unique";
                    else where = st.toString().replace(' ', '_');
                    out.println("@@LOCAL " + where + " " + s.getSize() + " " + (s.isParameter() ? "P" : "L") + " " +
                        s.getName() + " :: " + decl(s.getDataType(), s.getName()));
                }
            }
            out.println("@@CODE");
            out.println(r.getDecompiledFunction().getC());
            out.println("@@END");
            ok++;
            if (ok % 250 == 0) println("exported " + ok);
        }
        for (Function f : currentProgram.getFunctionManager().getExternalFunctions()) {
            out.println("@@EXTERN " + f.getName() + " :: " + f.getSignature().getPrototypeString());
        }
        out.close();
        println("exported " + ok + " functions, " + fail + " failed");

        // all data types, as a C header
        try (PrintWriter tw = new PrintWriter(new File(outDir, "types.h"), "UTF-8")) {
            DataTypeWriter dtw = new DataTypeWriter(currentProgram.getDataTypeManager(), tw, false);
            List<DataType> all = new ArrayList<>();
            currentProgram.getDataTypeManager().getAllDataTypes(all);
            dtw.write(all, monitor);
        }

        // labels/symbols at every address (for resolving &LAB_/PTR_ names)
        try (PrintWriter sw = new PrintWriter(new File(outDir, "symbols.txt"), "UTF-8")) {
            SymbolIterator si = currentProgram.getSymbolTable().getAllSymbols(true);
            while (si.hasNext()) {
                Symbol s = si.next();
                if (s.isExternal()) continue;
                sw.println(hex(s.getAddress()) + " " + s.getSymbolType() + " " + s.getName(true).replace(' ', '_'));
            }
        }
        decomp.dispose();
    }

    /** Functions referenced from data (relocations pointing into .text). */
    private void createMissingFunctions() throws Exception {
        Memory mem = currentProgram.getMemory();
        MemoryBlock text = mem.getBlock(".text");
        if (text == null) return;
        FunctionManager fm = currentProgram.getFunctionManager();
        Listing listing = currentProgram.getListing();
        int created = 0, inside = 0;
        Iterator<Relocation> it = currentProgram.getRelocationTable().getRelocations();
        Set<Address> targets = new TreeSet<>();
        while (it.hasNext()) {
            Relocation rel = it.next();
            Address loc = rel.getAddress();
            if (text.contains(loc)) continue; // only data -> code pointers
            int v;
            try { v = mem.getInt(loc); } catch (Exception e) { continue; }
            Address tgt = toAddr(v & 0xffffffffL);
            if (!text.contains(tgt)) continue;
            targets.add(tgt);
        }
        for (Address tgt : targets) {
            if (fm.getFunctionAt(tgt) != null) continue;
            if (fm.getFunctionContaining(tgt) != null) { inside++; continue; }
            if (listing.getInstructionAt(tgt) == null) disassemble(tgt);
            if (createFunction(tgt, null) != null) created++;
        }
        println("data->code pointers: " + targets.size() + ", created " + created + " functions, " + inside + " inside existing");
        try (PrintWriter w = new PrintWriter(new File(getScriptArgs()[0], "code_pointers.txt"))) {
            for (Address tgt : targets) {
                Function f = fm.getFunctionContaining(tgt);
                w.println(hex(tgt) + " " + (f == null ? "NONE" : (f.getEntryPoint().equals(tgt) ? "ENTRY " : "INSIDE ") + f.getName()));
            }
        }
    }

    /**
     * Apply hand-written corrections (recomp/fixups.txt), one per line:
     *   remove ADDR                      not code (data inside .text)
     *   func ADDR                        create a function here
     *   ret ADDR TYPE[@REG]              set the return type
     *   varargs ADDR NKEEP               keep the first NKEEP params, then "..."
     *   noreturn ADDR                    mark as non-returning
     *   sig ADDR RET[@REG] ; TYPE NAME[@REG] ; ...   full signature, optional register storage
     */
    private void applyFixups(String path, boolean structural) throws Exception {
        FunctionManager fm = currentProgram.getFunctionManager();
        int n = 0;
        for (String line0 : java.nio.file.Files.readAllLines(new File(path).toPath())) {
            String line = line0;
            int hash = line.indexOf('#');
            if (hash >= 0) line = line.substring(0, hash);
            line = line.trim();
            if (line.isEmpty()) continue;
            String[] w = line.split("\\s+", 3);
            boolean isStructural = w[0].equals("func") || w[0].equals("remove") || w[0].equals("patch");
            if (isStructural != structural) continue;
            Address a = toAddr(Long.parseLong(w[1], 16));
            try {
                if (w[0].equals("patch")) {
                    /* rewrite instruction bytes in the analysis copy only
                     * (same-length, same-semantics rewrites that Ghidra can follow) */
                    String hx = w[2].trim();
                    byte[] b = new byte[hx.length() / 2];
                    for (int i = 0; i < b.length; i++) b[i] = (byte) Integer.parseInt(hx.substring(i * 2, i * 2 + 2), 16);
                    byte[] cur = getBytes(a, b.length);
                    if (!java.util.Arrays.equals(cur, b)) {
                        Function outer = currentProgram.getFunctionManager().getFunctionContaining(a);
                        clearListing(a, a.add(b.length - 1));
                        setBytes(a, b);
                        disassemble(a);
                        if (outer != null) {
                            Address e = outer.getEntryPoint();
                            currentProgram.getFunctionManager().removeFunction(e);
                            createFunction(e, null);
                        }
                    }
                    n++;
                    continue;
                }
                if (w[0].equals("func")) {
                    if (fm.getFunctionAt(a) == null) {
                        Function outer = fm.getFunctionContaining(a);
                        Address outerEntry = outer != null ? outer.getEntryPoint() : null;
                        if (outer != null) fm.removeFunction(outerEntry);
                        if (getInstructionAt(a) == null) disassemble(a);
                        Function nf = createFunction(a, null);
                        if (outerEntry != null) createFunction(outerEntry, null);
                        if (nf == null) println("fixup: could not create function at " + w[1]);
                    }
                    n++;
                    continue;
                }
                Function f = fm.getFunctionAt(a);
                if (f == null) { println("fixup: no function at " + w[1]); continue; }
                switch (w[0]) {
                case "remove": {
                    AddressSetView body = f.getBody();
                    fm.removeFunction(a);
                    clearListing(body);
                    break;
                }
                case "ret": {
                    String[] rt = w[2].trim().split("@");
                    DataType rtype = parseType(rt[0]);
                    if (rt.length > 1) {
                        List<Parameter> ps = new ArrayList<>(Arrays.asList(f.getParameters()));
                        ReturnParameterImpl ret = new ReturnParameterImpl(rtype,
                            new VariableStorage(currentProgram, currentProgram.getRegister(rt[1].trim())), currentProgram);
                        f.updateFunction(f.getCallingConventionName(), ret, ps,
                            Function.FunctionUpdateType.CUSTOM_STORAGE, true, SourceType.USER_DEFINED);
                    } else {
                        f.setReturnType(rtype, SourceType.USER_DEFINED);
                    }
                    break;
                }
                case "noreturn":
                    f.setNoReturn(true);
                    break;
                case "varargs": {
                    int keep = Integer.parseInt(w[2].trim());
                    while (f.getParameterCount() > keep) f.removeParameter(f.getParameterCount() - 1);
                    f.setVarArgs(true);
                    break;
                }
                case "sig": {
                    String[] parts = w[2].split(";");
                    String[] rt = parts[0].trim().split("@");
                    DataType rtype = parseType(rt[0].trim());
                    boolean custom = rt.length > 1;
                    List<String[]> defs = new ArrayList<>();
                    for (int i = 1; i < parts.length; i++) {
                        String pdef = parts[i].trim();
                        if (pdef.isEmpty()) continue;
                        String[] at = pdef.split("@");
                        if (at.length > 1) custom = true;
                        defs.add(at);
                    }
                    List<Parameter> ps = new ArrayList<>();
                    int off = 4;
                    for (String[] at : defs) {
                        String decl = at[0].trim();
                        int sp = decl.lastIndexOf(' ');
                        DataType pt = parseType(decl.substring(0, sp).trim());
                        String pname = decl.substring(sp + 1).trim();
                        if (at.length > 1) {
                            ps.add(new ParameterImpl(pname, pt,
                                new VariableStorage(currentProgram, currentProgram.getRegister(at[1].trim())), currentProgram));
                        } else if (custom) {
                            ps.add(new ParameterImpl(pname, pt,
                                new VariableStorage(currentProgram, off, pt.getLength()), currentProgram));
                            off += Math.max(4, pt.getLength());
                        } else {
                            ps.add(new ParameterImpl(pname, pt, currentProgram));
                        }
                    }
                    ReturnParameterImpl ret;
                    if (rtype instanceof VoidDataType)
                        ret = new ReturnParameterImpl(rtype, VariableStorage.VOID_STORAGE, currentProgram);
                    else if (rt.length > 1)
                        ret = new ReturnParameterImpl(rtype, new VariableStorage(currentProgram,
                            currentProgram.getRegister(rt[1].trim())), currentProgram);
                    else if (custom)
                        ret = new ReturnParameterImpl(rtype, new VariableStorage(currentProgram,
                            currentProgram.getRegister("EAX")), currentProgram);
                    else
                        ret = new ReturnParameterImpl(rtype, currentProgram);
                    f.updateFunction("__cdecl", ret, ps,
                        custom ? Function.FunctionUpdateType.CUSTOM_STORAGE : Function.FunctionUpdateType.DYNAMIC_STORAGE_ALL_PARAMS,
                        true, SourceType.USER_DEFINED);
                    break;
                }
                default:
                    println("fixup: unknown directive " + w[0]);
                    continue;
                }
                n++;
            } catch (Exception e) {
                println("fixup failed: " + line + ": " + e);
            }
        }
        println("applied " + n + " fixups");
    }

    private DataType parseType(String t) {
        t = t.trim();
        if (t.endsWith("*")) return new PointerDataType(parseType(t.substring(0, t.length() - 1)));
        switch (t) {
        case "void": return VoidDataType.dataType;
        case "char": return CharDataType.dataType;
        case "uchar": case "byte": return ByteDataType.dataType;
        case "short": return ShortDataType.dataType;
        case "ushort": case "word": return UnsignedShortDataType.dataType;
        case "int": return IntegerDataType.dataType;
        case "uint": case "dword": return UnsignedIntegerDataType.dataType;
        case "bool": return BooleanDataType.dataType;
        case "float": return FloatDataType.dataType;
        case "double": return DoubleDataType.dataType;
        case "float10": return new Float10DataType();
        case "longlong": return LongLongDataType.dataType;
        case "undefined1": return Undefined1DataType.dataType;
        case "undefined2": return Undefined2DataType.dataType;
        case "undefined4": return Undefined4DataType.dataType;
        case "undefined8": return Undefined8DataType.dataType;
        }
        List<DataType> found = new ArrayList<>();
        currentProgram.getDataTypeManager().findDataTypes(t, found);
        if (!found.isEmpty()) return found.get(0);
        throw new IllegalArgumentException("unknown type " + t);
    }

    /** Give every directly addressed global an undefinedN of the widest size
     *  any instruction accesses it with, so the decompiler names it DAT_x with
     *  a consistent size (narrower accesses become DAT_x._n_m_ pieces). */
    private void typeGlobalsByAccess() throws Exception {
        Listing listing = currentProgram.getListing();
        Memory mem = currentProgram.getMemory();
        MemoryBlock text = mem.getBlock(".text");
        TreeMap<Address, Integer> sizes = new TreeMap<>();
        InstructionIterator ii = listing.getInstructions(true);
        while (ii.hasNext()) {
            Instruction ins = ii.next();
            for (PcodeOp op : ins.getPcode()) {
                List<Varnode> vs = new ArrayList<>(Arrays.asList(op.getInputs()));
                if (op.getOutput() != null) vs.add(op.getOutput());
                for (Varnode v : vs) {
                    if (!v.isAddress()) continue;
                    Address a = v.getAddress();
                    if (!a.getAddressSpace().isMemorySpace()) continue;
                    MemoryBlock b = mem.getBlock(a);
                    if (b == null || b == text) continue;
                    String bn = b.getName();
                    if (bn.equals(".idata") || bn.equals(".rsrc")) continue;
                    sizes.merge(a, v.getSize(), Math::max);
                }
            }
        }
        int n = 0, skipped = 0;
        for (Map.Entry<Address, Integer> e : sizes.entrySet()) {
            Address a = e.getKey();
            int sz = e.getValue();
            DataType dt;
            switch (sz) {
            case 1: dt = Undefined1DataType.dataType; break;
            case 2: dt = Undefined2DataType.dataType; break;
            case 4: dt = Undefined4DataType.dataType; break;
            case 8: dt = Undefined8DataType.dataType; break;
            case 10: dt = new Float10DataType(); break;
            default: continue;
            }
            if (listing.getDefinedDataContaining(a) != null) { skipped++; continue; }
            try {
                Address end = a.add(dt.getLength() - 1);
                boolean free = true;
                for (Address x = a; x.compareTo(end) <= 0; x = x.add(1)) {
                    if (listing.getDefinedDataContaining(x) != null || listing.getInstructionContaining(x) != null) { free = false; break; }
                }
                if (!free) { skipped++; continue; }
                listing.createData(a, dt);
                n++;
            } catch (Exception ex) { skipped++; }
        }
        println("typed " + n + " globals by access size (" + skipped + " skipped)");
    }

    /** The calling convention must match the function's real RET: MSVC C
     *  code is cdecl (plain "ret"), and a __stdcall label makes the
     *  decompiler assume the callee pops its arguments, which corrupts the
     *  stack tracking of every caller. */
    private void fixCallingConventions() throws Exception {
        int toC = 0, toStd = 0, purgeFixed = 0;
        Listing listing = currentProgram.getListing();
        for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
            if (f.isExternal() || f.isThunk()) continue;
            int purge = -1;
            boolean mixed = false;
            InstructionIterator it = listing.getInstructions(f.getBody(), true);
            while (it.hasNext()) {
                Instruction ins = it.next();
                if (!ins.getMnemonicString().equalsIgnoreCase("RET")) continue;
                int p = 0;
                if (ins.getNumOperands() > 0) {
                    Object[] o = ins.getOpObjects(0);
                    if (o.length > 0 && o[0] instanceof ghidra.program.model.scalar.Scalar)
                        p = (int) ((ghidra.program.model.scalar.Scalar) o[0]).getUnsignedValue();
                }
                if (purge >= 0 && purge != p) mixed = true;
                purge = p;
            }
            if (purge < 0 || mixed) continue;
            if (f.getStackPurgeSize() != purge) {
                try { f.setStackPurgeSize(purge); purgeFixed++; } catch (Exception e) { }
            }
            String cc = f.getCallingConventionName();
            try {
                if (purge == 0 && f.getParameterCount() > 0 && !"__cdecl".equals(cc) && !f.hasCustomVariableStorage()) {
                    f.setCallingConvention("__cdecl");
                    toC++;
                } else if (purge > 0 && "__cdecl".equals(cc) && !f.hasCustomVariableStorage()) {
                    f.setCallingConvention("__stdcall");
                    toStd++;
                }
            } catch (Exception e) { }
        }
        println("calling conventions: " + toC + " -> __cdecl, " + toStd + " -> __stdcall, " + purgeFixed + " stack purges fixed");
    }

    /** Ghidra's printf format-string analysis puts a prototype override on
     *  every sprintf call site, with an "unknown" calling convention.  The
     *  decompiler then mis-tracks the stack after those calls: later reads of
     *  locals come from the wrong slot (HUD gear digit, records screen crash).
     *  The plain varargs prototype of _sprintf is enough. */
    private void removeCallSiteOverrides() throws Exception {
        SymbolTable st = currentProgram.getSymbolTable();
        List<Symbol> kill = new ArrayList<>();
        SymbolIterator it = st.getAllSymbols(false);
        while (it.hasNext()) {
            Symbol s = it.next();
            if (s.getName().startsWith("prt") && HighFunctionDBUtil.readOverride(s) != null) kill.add(s);
        }
        for (Symbol s : kill) st.removeSymbolSpecial(s);
        println("removed " + kill.size() + " call-site prototype overrides");
    }

    /** Game functions must have unique C identifiers (no "switchD_x::caseD_4"). */
    private void renameOddFunctions() throws Exception {
        Map<String, Integer> count = new HashMap<>();
        for (Function f : currentProgram.getFunctionManager().getFunctions(true))
            count.merge(f.getName(), 1, Integer::sum);
        int n = 0;
        for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
            if (f.isExternal() || f.isThunk()) continue;
            long a = f.getEntryPoint().getOffset();
            if (a >= 0x4a7e30L) continue;
            String nm = f.getName(true);
            if (!nm.matches("[A-Za-z_][A-Za-z0-9_]*") || count.get(f.getName()) > 1) {
                f.setParentNamespace(currentProgram.getGlobalNamespace());
                f.setName(String.format("FUN_%08x", a), SourceType.ANALYSIS);
                n++;
            }
        }
        println("renamed " + n + " functions");
    }

    /** Analysis leaves 1-byte "undefined" data at referenced addresses; the
     *  decompiler then renders wider accesses as _DAT_ overlaps of unknown
     *  size.  Clearing them lets it type each global from its uses. */
    private void clearUndefinedData() throws Exception {
        int n = 0;
        Listing listing = currentProgram.getListing();
        for (MemoryBlock b : currentProgram.getMemory().getBlocks()) {
            if (b.getName().equals(".text") || !b.isInitialized() && !b.getName().equals(".data")) { }
            if (b.getName().equals(".text") || b.getName().equals(".idata") || b.getName().equals(".rsrc") || b.getName().startsWith("Headers")) continue;
            DataIterator it = listing.getDefinedData(new AddressSet(b.getStart(), b.getEnd()), true);
            List<Address> kill = new ArrayList<>();
            while (it.hasNext()) {
                Data d = it.next();
                DataType dt = d.getDataType();
                if (dt instanceof Undefined || dt instanceof DefaultDataType) kill.add(d.getAddress());
            }
            for (Address a : kill) { listing.clearCodeUnits(a, a, false); n++; }
        }
        println("cleared " + n + " undefined data items");
    }

    /** MSVC's __ftol takes its argument in ST0; without this Ghidra drops the operand. */
    private void fixX87Helpers() throws Exception {
        for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
            if (!f.getName().equals("__ftol")) continue;
            ghidra.program.model.lang.Register st0 = currentProgram.getRegister("ST0");
            ghidra.program.model.lang.Register eax = currentProgram.getRegister("EAX");
            ghidra.program.model.lang.Register edx = currentProgram.getRegister("EDX");
            Parameter p = new ParameterImpl("x", new Float10DataType(),
                new VariableStorage(currentProgram, st0), currentProgram);
            ReturnParameterImpl ret = new ReturnParameterImpl(new LongLongDataType(),
                new VariableStorage(currentProgram, edx, eax), currentProgram);
            f.updateFunction("__cdecl", ret, Arrays.asList(p),
                Function.FunctionUpdateType.CUSTOM_STORAGE, true, SourceType.USER_DEFINED);
            println("fixed __ftol at " + hex(f.getEntryPoint()));
        }
    }

    private static String hex(Address a) {
        return String.format("%08x", a.getOffset());
    }

    /** C declaration of `name` with type `dt`. */
    private static String decl(DataType dt, String name) {
        if (dt == null) return "undefined " + name;
        if (dt instanceof TypeDef && !(dt instanceof Pointer)) {
            return dt.getName() + " " + name;
        }
        if (dt instanceof Array) {
            Array a = (Array) dt;
            return decl(a.getDataType(), name + "[" + a.getNumElements() + "]");
        }
        if (dt instanceof Pointer) {
            Pointer p = (Pointer) dt;
            DataType pt = p.getDataType();
            String inner = (pt instanceof Array) ? "(*" + name + ")" : "*" + name;
            if (pt == null) return "undefined " + inner;
            if (pt instanceof FunctionDefinition) return pt.getName() + " " + inner;
            return decl(pt, inner);
        }
        if (dt instanceof FunctionDefinition) return dt.getName() + " " + name;
        return dt.getName() + " " + name;
    }
}
