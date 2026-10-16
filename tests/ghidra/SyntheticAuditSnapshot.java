// Synthetic validation only. Checks that the production audit does not mutate
// the Program, including its modification counter, functions, and listing.
import ghidra.app.script.GhidraScript;

public class SyntheticAuditSnapshot extends GhidraScript {
    @Override
    public void run() throws Exception {
        String state = currentProgram.getModificationNumber() + "|" +
            currentProgram.getFunctionManager().getFunctionCount() + "|" +
            currentProgram.getListing().getNumCodeUnits() + "|" +
            currentProgram.getListing().getNumInstructions();
        String stage = getScriptArgs()[0];
        if ("before".equals(stage)) {
            System.setProperty("mdk.synthetic.snapshot", state);
        } else if (!state.equals(System.getProperty("mdk.synthetic.snapshot"))) {
            throw new IllegalStateException("Synthetic program changed across audit");
        }
        println("SYNTHETIC_INVARIANT|" + stage + "|" + state);
    }
}
