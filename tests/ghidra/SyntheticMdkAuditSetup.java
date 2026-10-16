// Synthetic validation only. Never run on an original game program.
// Filename, marker, and actual file hash are checked before any mutation.
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.security.MessageDigest;
import java.util.Arrays;
import java.util.HexFormat;
import java.util.Set;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.symbol.RefType;
import ghidra.program.model.symbol.SourceType;

public class SyntheticMdkAuditSetup extends GhidraScript {
    private static final String MOCKED_SHA =
        "e57bd63ba6af1ce201e3dd28a352e36ffa737267a9be0ed733fb81e6c40ba788";

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (currentProgram == null || args.length != 2) {
            println("SYNTHETIC_SETUP_REFUSED|missing_program_or_arguments");
            return;
        }
        String mode = args[0];
        Path file = Path.of(currentProgram.getExecutablePath());
        Set<String> names = Set.of("SYNTHETIC-MDK-AUDIT-flow.exe",
                                   "SYNTHETIC-MDK-AUDIT-resolved.exe");
        Set<String> modes = Set.of("pass", "wrong-sha", "wrong-format",
            "wrong-base", "missing-core", "truncate", "unmapped-global");
        String actual = HexFormat.of().formatHex(MessageDigest.getInstance("SHA-256")
            .digest(Files.readAllBytes(file)));
        byte[] marker = "SYNTHETIC_MDK_AUDIT_FIXTURE_ONLY_v1"
            .getBytes(StandardCharsets.US_ASCII);
        byte[] imported = new byte[marker.length];
        if (!names.contains(file.getFileName().toString()) ||
            !names.contains(currentProgram.getName()) || !modes.contains(mode) ||
            MOCKED_SHA.equals(actual) || !actual.equals(args[1]) ||
            !actual.equals(currentProgram.getExecutableSHA256()) ||
            currentProgram.getMemory().getBytes(toAddr(0x540100L), imported) != marker.length ||
            !Arrays.equals(marker, imported)) {
            println("SYNTHETIC_SETUP_REFUSED|fixture_identity_mismatch");
            return;
        }
        // Deliberately mocked import metadata exercises the production guard.
        // The real synthetic SHA was checked above; this is no MDK evidence.
        if (!"wrong-sha".equals(mode)) currentProgram.setExecutableSHA256(MOCKED_SHA);
        if ("wrong-format".equals(mode)) {
            currentProgram.setExecutableFormat("SYNTHETIC unsupported format");
        }
        long[] entries = {0x40210cL, 0x4022b8L, 0x402388L,
                          0x469668L, 0x465228L, 0x432f84L, 0x401000L};
        for (long entry : entries) {
            Address address = toAddr(entry);
            disassemble(address);
            if (getFunctionAt(address) == null) {
                createFunction(address, "SYNTHETIC_" + Long.toHexString(entry));
            }
        }
        if ("missing-core".equals(mode)) {
            currentProgram.getFunctionManager().removeFunction(toAddr(0x469668L));
        }
        if ("truncate".equals(mode)) {
            for (int i = 0; i < 500; ++i) {
                currentProgram.getReferenceManager().addMemoryReference(
                    toAddr(0x410000L + i), toAddr(0x54c5e0L),
                    RefType.READ, SourceType.USER_DEFINED, 0);
            }
        }
        if ("unmapped-global".equals(mode)) {
            currentProgram.getMemory().removeBlock(
                currentProgram.getMemory().getBlock(toAddr(0x54c5e0L)), monitor);
        }
        if ("wrong-base".equals(mode)) {
            currentProgram.setImageBase(toAddr(0x500000L), true);
        }
        println("SYNTHETIC_FIXTURE|mode=" + mode + "|mocked_stored_hash=" +
                !"wrong-sha".equals(mode) + "|actual_sha256=" + actual);
    }
}
