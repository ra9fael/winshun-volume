#!/usr/bin/env python3
# e2e_device_switch.py — the regression test for "the plugin kept adjusting the
# previous output device": switches the system default output while a real
# volume.exe is running, and checks that the next operation follows.
#
# Uses the undocumented IPolicyConfig COM interface, the same one the Settings
# app and the tray flyout use. It is declared as an IUnknown-based interface -
# the default IDispatch layout puts every method on the wrong vtable slot and
# crashes the caller - and the default device is restored no matter how the
# test ends.
#
# Switching the default output is a system-wide change: the script refuses to
# run unless TVB_E2E_DEVICE_SWITCH=1 is set, so it stays out of the ordinary
# ctest run. Exits 77 (ctest SKIP_RETURN_CODE) when declined or when the
# machine has no second active render endpoint.
#
# Usage: TVB_E2E_DEVICE_SWITCH=1 python tools/e2e_device_switch.py <volume.exe>
import json
import os
import sys
import time
import uuid

from e2e_common import PluginProcess, find_repo_exe, master_item, parse_percent

SKIP_EXIT_CODE = 77

# IPolicyConfig: the interface the Settings app drives. Every method occupies
# exactly one vtable slot; only SetDefaultEndpoint is called, the rest keep the
# offsets right. CLSID {870af99c-...} is CPolicyConfigClient, IID {f8679f50-...}
# is IPolicyConfig.
POLICY_CONFIG_CS = r'''
using System;
using System.Runtime.InteropServices;
public static class PolicyConfig {
    [ComImport, Guid("BCDE0395-E52F-467C-8E3D-C4579291692E")] class MMDeviceEnumerator { }
    [ComImport, Guid("A95664D2-9614-4F35-A746-DE8DB63617E6"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IMMDeviceEnumerator {
        [PreserveSig] int EnumAudioEndpoints(int flow, int mask, out IntPtr col);
        [PreserveSig] int GetDefaultAudioEndpoint(int flow, int role, out IMMDevice dev);
        [PreserveSig] int GetDevice([MarshalAs(UnmanagedType.LPWStr)] string id, out IMMDevice dev);
        [PreserveSig] int RegisterEndpointNotificationCallback(IntPtr cb);
        [PreserveSig] int UnregisterEndpointNotificationCallback(IntPtr cb);
    }
    [ComImport, Guid("0BD7A1BE-7A1A-44DB-8397-CC5392387B5E"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IMMDeviceCollection {
        [PreserveSig] int GetCount(out uint count);
        [PreserveSig] int Item(uint index, out IMMDevice dev);
    }
    [ComImport, Guid("D666063F-1587-4E43-81F1-B948E807363F"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IMMDevice {
        [PreserveSig] int Activate(ref Guid iid, int ctx, IntPtr p, [MarshalAs(UnmanagedType.IUnknown)] out object o);
        [PreserveSig] int OpenPropertyStore(int access, out IntPtr props);
        [PreserveSig] int GetId([MarshalAs(UnmanagedType.LPWStr)] out string id);
        [PreserveSig] int GetState(out int state);
    }
    [ComImport, Guid("f8679f50-850a-41cf-9c72-430f290290c8"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IPolicyConfig {
        [PreserveSig] int GetMixFormat([MarshalAs(UnmanagedType.LPWStr)] string id, IntPtr f);
        [PreserveSig] int GetDeviceFormat([MarshalAs(UnmanagedType.LPWStr)] string id, int d, IntPtr f);
        [PreserveSig] int ResetDeviceFormat([MarshalAs(UnmanagedType.LPWStr)] string id);
        [PreserveSig] int SetDeviceFormat([MarshalAs(UnmanagedType.LPWStr)] string id, IntPtr a, IntPtr b);
        [PreserveSig] int GetProcessingPeriod([MarshalAs(UnmanagedType.LPWStr)] string id, int d, IntPtr a, IntPtr b);
        [PreserveSig] int SetProcessingPeriod([MarshalAs(UnmanagedType.LPWStr)] string id, IntPtr p);
        [PreserveSig] int GetShareMode([MarshalAs(UnmanagedType.LPWStr)] string id, IntPtr m);
        [PreserveSig] int SetShareMode([MarshalAs(UnmanagedType.LPWStr)] string id, IntPtr m);
        [PreserveSig] int GetPropertyValue([MarshalAs(UnmanagedType.LPWStr)] string id, IntPtr k, IntPtr v);
        [PreserveSig] int SetPropertyValue([MarshalAs(UnmanagedType.LPWStr)] string id, IntPtr k, IntPtr v);
        [PreserveSig] int SetDefaultEndpoint([MarshalAs(UnmanagedType.LPWStr)] string id, int role);
        [PreserveSig] int SetEndpointVisibility([MarshalAs(UnmanagedType.LPWStr)] string id, int v);
    }
    [ComImport, Guid("870af99c-171d-4f9e-af0d-e63df40c2bc9")] class CPolicyConfigClient { }
    public static string Default(int role) {
        var en = (IMMDeviceEnumerator)new MMDeviceEnumerator(); IMMDevice d;
        if (en.GetDefaultAudioEndpoint(0, role, out d) != 0) return null;
        string id; d.GetId(out id); Marshal.ReleaseComObject(d); return id;
    }
    public static string[] ActiveIds() {
        var en = (IMMDeviceEnumerator)new MMDeviceEnumerator();
        IntPtr colPtr; var ids = new System.Collections.Generic.List<string>();
        if (en.EnumAudioEndpoints(0, 1, out colPtr) == 0) {
            var col = (IMMDeviceCollection)Marshal.GetObjectForIUnknown(colPtr);
            uint count; col.GetCount(out count);
            for (uint i = 0; i < count; i++) {
                IMMDevice d;
                if (col.Item(i, out d) == 0) { string id; d.GetId(out id); ids.Add(id); Marshal.ReleaseComObject(d); }
            }
            Marshal.Release(colPtr);
        }
        return ids.ToArray();
    }
    public static int SetDefault(string id) {
        var pc = (IPolicyConfig)new CPolicyConfigClient(); int failed = 0;
        for (int role = 0; role < 3; role++)
            if (pc.SetDefaultEndpoint(id, role) != 0) failed++;
        Marshal.ReleaseComObject(pc);
        return failed;
    }
}
'''


def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def compile_policy_config():
    """Compiles the IPolicyConfig interop with the .NET compiler PowerShell
    ships (Add-Type), since Python COM interop would need pythonnet."""
    script = r"""
$code = @'
__CODE__
'@
Add-Type -TypeDefinition $code -Language CSharp | Out-Null
"""
    script = script.replace("__CODE__", POLICY_CONFIG_CS)
    import subprocess
    result = subprocess.run(
        ["powershell", "-NoProfile", "-NonInteractive", "-Command", script],
        capture_output=True, text=True, timeout=60)
    expect(result.returncode == 0, f"Add-Type failed: {result.stderr.strip()[:400]}")


def powershell_call(expression):
    """Calls one compiled PolicyConfig method through a PowerShell child. The
    compiled type only lives inside that child, so every call compiles again -
    a second or so, which is fine for a test that is run on demand."""
    script = r"""
$code = @'
__CODE__
'@
Add-Type -TypeDefinition $code -Language CSharp | Out-Null
__EXPR__
"""
    script = script.replace("__CODE__", POLICY_CONFIG_CS).replace("__EXPR__", expression)
    import subprocess
    result = subprocess.run(
        ["powershell", "-NoProfile", "-NonInteractive", "-Command", script],
        capture_output=True, text=True, timeout=90)
    expect(result.returncode == 0,
           f"PolicyConfig call failed: {result.stderr.strip()[:400]}")
    return result.stdout.strip()


def main():
    if os.environ.get("TVB_E2E_DEVICE_SWITCH") != "1":
        print("SKIP: set TVB_E2E_DEVICE_SWITCH=1 to allow switching the default output")
        return SKIP_EXIT_CODE

    exe = sys.argv[1] if len(sys.argv) > 1 else find_repo_exe(["volume.exe"])
    expect(exe is not None, "volume.exe not found; pass its path as the first argument")

    compile_policy_config()
    original = powershell_call("0..2 | ForEach-Object { [PolicyConfig]::Default($_) } | ConvertTo-Json -Compress")
    originals = json.loads(original)
    expect(isinstance(originals, list) and originals[0],
           "no default render endpoint on this machine")
    others = [i for i in originals if i]
    # Any active render endpoint other than the current default will do; the
    # plugin only needs to notice that the default moved.
    endpoints = powershell_call("[PolicyConfig]::ActiveIds() | ConvertTo-Json -Compress")
    loaded = json.loads(endpoints)
    candidates = [e for e in (loaded if isinstance(loaded, list) else [loaded]) if e not in originals]
    expect(candidates, "no second active render endpoint to switch to")
    other = candidates[0]

    plugin = PluginProcess(exe)
    try:
        items = plugin.query()
        if len(items) == 1 and items[0].get("id") == "audio-unavailable":
            print("SKIP: no audio endpoint")
            return SKIP_EXIT_CODE
        before = parse_percent(master_item(items)["subtitle"])

        failed = powershell_call(f"[PolicyConfig]::SetDefault('{other}') | Out-String")
        expect(failed.strip() == "0", f"SetDefaultEndpoint failed: {failed.strip()}")
        time.sleep(0.8)

        after_items = plugin.query()
        after = parse_percent(master_item(after_items)["subtitle"])
        err = plugin.stderr_text()
        expect(err.count("following output device") >= 2,
               f"the plugin did not rebind to the new device; log:\n{err[-600:]}")

        restored = powershell_call(
            f"[PolicyConfig]::SetDefault('{originals[0]}') | Out-String")
        expect(restored.strip() == "0", f"restore failed: {restored.strip()}")
        time.sleep(0.8)

        back = parse_percent(master_item(plugin.query())["subtitle"])
        expect(back == before, f"master was {before}, now {back} after restoring")
        expect("following output device" in plugin.stderr_text(), "no rebind logged")

        code, err = plugin.close()
        expect(code == 0, f"volume.exe exited with {code}")
        expect("threw" not in err, f"the plugin threw: {err[:400]}")
        print(f"OK: default output switched {originals[0][-13:]} -> {other[-13:]} -> "
              f"back; master {before}% -> {after}% -> {back}%")
        return 0
    finally:
        for role, dev in enumerate(originals):
            if dev:
                powershell_call(f"[PolicyConfig]::SetDefault('{dev}') | Out-String")
        plugin.close()


if __name__ == "__main__":
    sys.exit(main())

