param(
  [string]$Dump = "C:\Users\curry\AppData\Roaming\openmohaa\openmohaa.exe.15776.dmp",
  [string]$SymPath = "C:\mohaa-coop-dev\openmohaa-hzm\.cmake\RelWithDebInfo"
)

$src = @"
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class Dbg {
    [DllImport("dbgeng.dll")]
    public static extern int DebugCreate(ref Guid InterfaceId, [MarshalAs(UnmanagedType.IUnknown)] out object Interface);
}
"@
Add-Type -TypeDefinition $src -ErrorAction Stop

# Use the dbgeng COM interfaces via reflection is hard; instead shell to a tiny approach:
Write-Host "Falling back to cdb-style not available; using dbghelp minidump reader"
