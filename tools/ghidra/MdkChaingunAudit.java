// Read-only navigation index for the recorded BUILD_A chaingun audio seam.
// No bytes, instruction text, symbols, or decompiler bodies are exported.
// Run on an EXISTING analyzed program with -process -readOnly -noanalysis.
// Optional arguments: up to eight exact function-entry VAs (hex) to inspect
// instead of the default focus/slot-reference functions. Never weapon names.
// @category MDK.Evidence

import java.util.LinkedHashMap;
import java.util.Map;
import java.util.TreeMap;

import ghidra.app.script.GhidraScript;
import ghidra.framework.Application;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.symbol.FlowType;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import ghidra.util.exception.CancelledException;

public class MdkChaingunAudit extends GhidraScript {
    private static final String BUILD_A_SHA256 =
        "e57bd63ba6af1ce201e3dd28a352e36ffa737267a9be0ed733fb81e6c40ba788";
    private static final int MAX_ROWS = 400;
    private static final long[] FOCUS = {0x469668L, 0x432f84L, 0x465228L};
    private final Map<Long, String> globals = new LinkedHashMap<>();
    private final Map<Long, String> apis = new LinkedHashMap<>();
    private final Map<Address, Function> selected = new TreeMap<>();
    private int rows;
    private int omitted;
    private int gaps;
    private int unresolved;

    private boolean defaultAddress(Address a) {
        return a != null && !Address.NO_ADDRESS.equals(a) &&
            a.getAddressSpace().equals(currentProgram.getAddressFactory()
                .getDefaultAddressSpace());
    }

    private String at(Address a) {
        if (a == null) return "-";
        if (Address.NO_ADDRESS.equals(a)) return "NO_ADDRESS";
        return (defaultAddress(a) ? "" : "space" + a.getAddressSpace().getSpaceID() + ":") +
            String.format("0x%08x", a.getOffset());
    }

    private Address addr(long value) {
        return currentProgram.getAddressFactory().getDefaultAddressSpace()
            .getAddress(value);
    }

    private void emit(String facts) {
        if (rows++ < MAX_ROWS) println("MDK_AUDIO|" + facts + "|END");
        else ++omitted;
    }

    private String functionAt(Address a) {
        if (a == null || Address.NO_ADDRESS.equals(a)) return "-";
        Function f = currentProgram.getFunctionManager().getFunctionContaining(a);
        return f == null ? "-" : at(f.getEntryPoint());
    }

    private String api(Address a) {
        return defaultAddress(a) ? apis.getOrDefault(a.getOffset(), "unclassified") :
            "unclassified";
    }

    private void select(Address entry) {
        Function f = currentProgram.getFunctionManager().getFunctionAt(entry);
        if (f == null) {
            ++gaps;
            emit("GAP|undefined_function=" + at(entry));
        } else selected.put(entry, f);
    }

    private boolean guard() {
        if (currentProgram == null) {
            emit("REFUSED|reason=no_program");
            return false;
        }
        // These are stored import/analysis properties, NOT a hash of current
        // memory. The owner must separately hash the original file and confirm
        // the saved project has no byte patches. A metadata match is not proof
        // of pristine retail provenance or correct function/reference analysis.
        String sha = currentProgram.getExecutableSHA256();
        if (sha == null || !BUILD_A_SHA256.equalsIgnoreCase(sha)) {
            emit("REFUSED|reason=import_sha256_missing_or_mismatch");
            return false;
        }
        if (!"Portable Executable (PE)".equals(currentProgram.getExecutableFormat()) ||
            !"x86:LE:32:default".equals(currentProgram.getLanguageID().toString()) ||
            currentProgram.getDefaultPointerSize() != 4 ||
            currentProgram.getImageBase().getOffset() != 0x400000L) {
            emit("REFUSED|reason=format_language_pointer_size_or_base_mismatch");
            return false;
        }
        for (long target : FOCUS) {
            if (!currentProgram.getMemory().contains(addr(target))) {
                emit("REFUSED|reason=unmapped_focus|address=" + at(addr(target)));
                return false;
            }
        }
        for (long base : globals.keySet()) {
            for (int i = 0; i < 4; ++i) {
                if (!currentProgram.getMemory().contains(addr(base + i))) {
                    emit("REFUSED|reason=unmapped_global|address=" + at(addr(base + i)));
                    return false;
                }
            }
        }
        emit("GUARD_OK|import_sha256=" + BUILD_A_SHA256 +
             "|language=x86:LE:32:default|image_base=0x00400000|ghidra=" +
             Application.getApplicationVersion());
        emit("LIMITS|source=existing_analysis_database|byte_patch_check=manual" +
             "|address_order_is_not_execution_order|weapon_semantics=UNKNOWN");
        return true;
    }

    private void slotReferences(long base, String label, boolean chooseFunctions)
            throws CancelledException {
        int count = 0;
        // Include byte/word accesses into a dword, but do not pretend this
        // discovers aliases, array-base accesses, or wider overlapping loads.
        for (int offset = 0; offset < 4; ++offset) {
            ReferenceIterator refs = currentProgram.getReferenceManager()
                .getReferencesTo(addr(base + offset));
            while (refs.hasNext()) {
                monitor.checkCancelled();
                Reference r = refs.next();
                ++count;
                emit("SLOT_REF|slot=" + label + "|from=" + at(r.getFromAddress()) +
                     "|to=" + at(r.getToAddress()) +
                     "|function=" + functionAt(r.getFromAddress()) +
                     "|type=" + r.getReferenceType() + "|operand=" + r.getOperandIndex() +
                     "|source=" + r.getSource() + "|primary=" + r.isPrimary());
                Function f = currentProgram.getFunctionManager()
                    .getFunctionContaining(r.getFromAddress());
                if (chooseFunctions && f != null) selected.put(f.getEntryPoint(), f);
            }
        }
        emit("SLOT_COUNT|slot=" + label + "|database_refs=" + count);
        if (count == 0) {
            ++gaps;
            emit("GAP|slot=" + label + "|reason=no_stored_refs_not_proof_of_no_use");
        }
    }

    private void inspect(Function f) throws CancelledException {
        Address entry = f.getEntryPoint();
        emit("FUNCTION|entry=" + at(entry) + "|body_ranges=" +
             f.getBody().getNumAddressRanges());
        if (f.isThunk()) {
            Function target = f.getThunkedFunction(true);
            emit("THUNK|entry=" + at(entry) + "|target=" +
                 (target == null ? "unresolved" : at(target.getEntryPoint())));
            if (target == null) ++unresolved;
        }
        ReferenceIterator incoming = currentProgram.getReferenceManager()
            .getReferencesTo(entry);
        while (incoming.hasNext()) {
            monitor.checkCancelled();
            Reference r = incoming.next();
            // Keep data refs too: a function pointer is not a proven caller.
            emit("ENTRY_REF|entry=" + at(entry) + "|from=" + at(r.getFromAddress()) +
                 "|function=" + functionAt(r.getFromAddress()) +
                 "|type=" + r.getReferenceType() + "|source=" + r.getSource());
        }
        int instructions = 0;
        InstructionIterator it = currentProgram.getListing()
            .getInstructions(f.getBody(), true);
        while (it.hasNext()) {
            monitor.checkCancelled();
            Instruction ins = it.next();
            ++instructions;
            Address site = ins.getAddress();
            for (Reference r : currentProgram.getReferenceManager().getReferencesFrom(site)) {
                if (!r.isMemoryReference() || !defaultAddress(r.getToAddress())) continue;
                for (Map.Entry<Long, String> global : globals.entrySet()) {
                    long to = r.getToAddress().getOffset();
                    if (to >= global.getKey() && to < global.getKey() + 4) {
                        emit("GLOBAL_REF|function=" + at(entry) + "|site=" + at(site) +
                             "|global=" + global.getValue() + "|to=" + at(r.getToAddress()) +
                             "|type=" + r.getReferenceType() + "|source=" + r.getSource());
                    }
                }
            }
            FlowType flow = ins.getFlowType();
            if (!flow.isCall() && !flow.isJump()) continue;
            Address[] destinations = ins.getFlows();
            String kind = flow.isCall() ? "CALL" : "JUMP";
            emit("FLOW|function=" + at(entry) + "|site=" + at(site) +
                 "|kind=" + kind + "|conditional=" + flow.isConditional() +
                 "|computed=" + flow.isComputed() + "|override=" + ins.getFlowOverride() +
                 "|fallthrough=" + at(ins.getFallThrough()));
            if (flow.isComputed() || destinations == null || destinations.length == 0) {
                ++unresolved;
                emit("REVIEW|site=" + at(site) + "|reason=indirect_or_unresolved_flow");
            }
            if (destinations == null) continue;
            for (Address to : destinations) {
                if (!defaultAddress(to)) {
                    ++unresolved;
                    emit("REVIEW|site=" + at(site) + "|target=" + at(to) +
                         "|reason=nondefault_or_unknown_target");
                }
                emit("EDGE|site=" + at(site) + "|to=" + at(to) +
                     "|target_function=" + functionAt(to) + "|api=" + api(to) +
                     "|outside_body=" + !f.getBody().contains(to));
            }
        }
        if (instructions == 0) {
            ++gaps;
            emit("GAP|function=" + at(entry) + "|reason=no_defined_instructions");
        }
    }

    @Override
    protected void run() throws Exception {
        globals.put(0x54c5e0L, "MULTIFIRE_slot");
        globals.put(0x54c5e4L, "GATTFIRE_slot");
        globals.put(0x540c74L, "fire_latch");
        globals.put(0x54161fL, "ammo0");
        apis.put(0x4022b8L, "spawn");
        apis.put(0x402388L, "ensure_or_restart_mode_UNKNOWN");
        apis.put(0x40210cL, "stop_by_record");
        apis.put(0x402160L, "positional_spawn");
        apis.put(0x402288L, "positional_restart");
        apis.put(0x4020b4L, "release_instance");
        apis.put(0x402068L, "release_pool_entry");
        apis.put(0x402658L, "query_record");
        apis.put(0x402698L, "set_volume");
        apis.put(0x40247cL, "rate_path");
        apis.put(0x46c31cL, "buffer_operations");
        if (!guard()) return; // No completion sentinel on refusal.
        try {
            String[] args = getScriptArgs();
            if (args.length > 8) {
                emit("REFUSED|reason=at_most_eight_function_entries");
                return;
            }
            // Validate all arguments before producing the navigation index.
            for (String arg : args) {
                if (!arg.matches("(?i)(0x)?[0-9a-f]{1,8}")) {
                    emit("REFUSED|reason=invalid_hex_function_entry");
                    return;
                }
            }
            if (args.length == 0) for (long target : FOCUS) select(addr(target));
            else for (String arg : args) select(addr(Long.parseLong(
                arg.replaceFirst("(?i)^0x", ""), 16)));
            slotReferences(0x54c5e0L, "MULTIFIRE_slot", args.length == 0);
            slotReferences(0x54c5e4L, "GATTFIRE_slot", args.length == 0);
            for (Function f : selected.values()) inspect(f);
            // This sentinel is not subject to the row cap. OK_DB_ONLY means
            // iteration finished, never that weapon behavior was established.
            println("MDK_AUDIO|COMPLETE|status=" + (omitted > 0 ? "TRUNCATED" :
                    gaps > 0 || unresolved > 0 ? "INCOMPLETE" : "OK_DB_ONLY") +
                    "|rows=" + Math.min(rows, MAX_ROWS) + "|omitted=" + omitted +
                    "|gaps=" + gaps + "|indirect_or_unresolved=" + unresolved +
                    "|weapon_semantics=UNKNOWN|END");
        } catch (CancelledException e) {
            println("MDK_AUDIO|CANCELLED|no_complete_report|END");
        }
    }
}
