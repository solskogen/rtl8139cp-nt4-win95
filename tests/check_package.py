#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Structural INF checks, not a substitute for legacy Setup testing."""
import argparse
import pathlib
import re


def sections(path):
    result = {}
    section = None
    for line in path.read_text(encoding="ascii").splitlines():
        line = line.strip()
        if not line or line.startswith(";"):
            continue
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1].lower()
            assert section not in result, (path, section)
            result[section] = []
        else:
            assert section is not None, path
            result[section].append(line)
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--dist", action="store_true")
    parser.add_argument("--floppy", type=pathlib.Path)
    args = parser.parse_args()
    if args.floppy:
        check_floppy(args.floppy)
        return
    check_nt4_driver()
    check_win95_driver()
    if args.dist:
        check_driver_dist()


def raw_sections(path):
    """Section bodies of an NT Setup script, keeping indentation-free lines."""
    data = path.read_bytes()
    assert b"\r\n" in data and data.count(b"\n") == data.count(b"\r\n"), path
    result = {}
    section = None
    for line in data.decode("ascii").splitlines():
        stripped = line.strip()
        if stripped.startswith("[") and stripped.endswith("]"):
            section = stripped[1:-1]
            assert section not in result, (path, section)
            result[section] = []
        elif stripped and not stripped.startswith(";"):
            assert section is not None, path
            result[section].append(stripped)
    return result


def symbols(lines):
    return dict(re.match(r"(\S+)\s*=\s*(.*)$", line).groups() for line in lines)


def check_nt4_driver():
    path = pathlib.Path("packaging/nt4/OEMSETUP.INF")
    data = raw_sections(path)
    for name in ("Identification", "Options", "OptionsTextENG", "FileConstants",
                 "FileConstantsENG", "GeneralConstants", "Identify",
                 "ReturnOptions", "InstallOption", "Install-Option",
                 "Install-Update", "Source Media Descriptions", "Files-RTL8139CP",
                 "LanguagesSupported", "date"):
        assert name in data, name
    assert data["Identification"] == ["OptionType = NetAdapter"]
    assert data["Options"] == ["RTL8139CP"]
    assert list(symbols(data["OptionsTextENG"])) == ["RTL8139CP"]
    assert data["Files-RTL8139CP"] == ["1, r8139cp.sys, SIZE=40000"]
    assert data["Source Media Descriptions"] == [
        '1 = "RTL8139C+ Windows NT 4.0 driver disk"']
    # OEM disks pass NO; YES makes setup copy from the NT install source.
    script = path.read_text()
    assert "DoAskSource, $(!STF_CWDDIR), $(SrcDir) NO" in script
    assert "$(SrcDir) YES" not in script
    constants = symbols(data["FileConstants"])
    assert constants["ProductSoftwareImagePath"] == \
        r'"\SystemRoot\System32\drivers\r8139cp.sys"'
    assert constants["VendorID"] == str(0x10EC)
    assert constants["DeviceID"] == str(0x8139)
    # DDK Network Guide 3.4.1: user-defined net rule names start lowercase,
    # are C identifiers and at most 30 characters.
    rule_names = set()
    for key in ("NetRuleSoftwareType", "NetRuleSoftwareClass",
                "NetRuleSoftwareBindable", "NetRuleHardwareType",
                "NetRuleHardwareClass"):
        words = re.findall(r"[A-Za-z_][A-Za-z0-9_]*", constants[key])
        rule_names.update(w for w in words if w not in (
            "ndisDriver", "basic", "non", "exclusive"))
    for name in rule_names:
        assert re.fullmatch(r"[a-z][A-Za-z0-9_]{0,29}", name), name
    assert constants["NetRuleSoftwareType"] == \
        '"rtl8139cpSys ndisDriver rtl8139cpDriver"'
    assert constants["NetRuleSoftwareBindable"] == \
        '{"rtl8139cpDriver rtl8139cpAdapter non exclusive 100"}'
    assert constants["NetRuleHardwareType"] == '"rtl8139cp rtl8139cpAdapter"'
    assert constants["NetRuleHardwareClass"] == '{"rtl8139cpAdapter basic"}'
    defined = set(constants) | set(symbols(data["FileConstantsENG"])) | \
        set(symbols(data["GeneralConstants"]))
    body = data["InstallOption"]
    labels = [m[1] for line in body
              for m in [re.match(r"(\w+) = \+$", line)] if m]
    assert len(labels) == len(set(labels))
    for line in body:
        for target in re.findall(r"\bgoto (\w+)", line, re.IGNORECASE):
            assert target in labels, target
    depth = loops = 0
    for line in body:
        word = line.split()[0].lower() if line.split() else ""
        if re.match(r"if(str|int|contains)(\(i\))?$", word):
            depth += 1
        elif word == "endif":
            depth -= 1
        elif word == "fordo" or word == "forlistdo":
            loops += 1
        elif word == "endforlistdo":
            loops -= 1
        assert depth >= 0 and loops >= 0, line
    assert depth == 0 and loops == 0
    # Local symbols referenced as $(Name) must be defined somewhere.
    assigned = {m[1] for line in body for m in
                [re.match(r"set(?:-(?:add|sub|mul|div))? (\w+)\s*=", line)] if m}
    # Output variables of OpenRegKey, EnumRegValue, Split-String, QueryListSize.
    for line in body:
        m = re.match(r"(?:OpenRegKey|EnumRegValue|Split-String) .* (\w+)$", line) \
            or re.match(r"QueryListSize (\w+) ", line)
        if m:
            assigned.add(m[1])
    # Set by InstallOption arguments, Setup and "ui start" dialogs.
    assigned |= {"Option", "SrcDir", "AddCopy", "DoCopy", "DoConfig", "Now",
                 "STF_CONTEXTINFNAME", "STF_INSTALL_OUTCOME", "DLGEVENT",
                 "Combo1Out", "EditTextOut"}
    text = "\n".join(body)
    for name in set(re.findall(r"\$\((\w+)\)", text)):
        assert name in defined | assigned, name
    for name in re.findall(r"\bShell \$\(UtilityInf\),? (\w+)", text):
        assert name in {"GetPCIInformation", "DoAskSource", "AddSoftwareComponent",
                        "AddValueList", "IsNetCardAlreadyInstalled",
                        "AddHardwareComponent", "RemoveSoftwareComponent",
                        "RemoveHardwareComponent", "RegistryErrorString",
                        "FindService"}, name
    # Properties dialogs (configure mode): stock Setup templates as used by
    # Realtek's NT4 INF; every dialog symbol section exists and is complete.
    dialogs = re.findall(r"read-syms (\w+)\$\(!STF_LANGUAGE\)", text)
    assert sorted(dialogs) == ["AddressDlg", "FileConstants", "MediaDlg",
                               "VlanDlg"], dialogs
    for name in ("MediaDlg", "VlanDlg", "AddressDlg"):
        dlg = symbols(data[name + "ENG"])
        for key in ("Caption", "DlgText", "DlgType", "DlgTemplate", "Continue",
                    "Help", "HelpContext"):
            assert key in dlg, (name, key)
        if dlg["DlgType"] == '"Combination"':
            assert dlg["DlgTemplate"] == '"ADAP_PROTO_COMBO"', name
            for key in ("Combo1List", "Combo1Out", "ComboListItemsIn",
                        "ComboListItemsOut", "CBOptionsGreyed", "Cancel"):
                assert key in dlg, (name, key)
        else:
            assert dlg["DlgType"] == '"Edit"', name
            assert dlg["DlgTemplate"] == '"QUERY_COMPUTER_NAME"', name
            for key in ("Edit1Label", "EditTextIn", "EditTextLim", "Exit"):
                assert key in dlg, (name, key)
    assert symbols(data["VlanDlgENG"])["EditTextLim"] == "4"
    assert symbols(data["AddressDlgENG"])["EditTextLim"] == "12"
    # Connection types match the driver's ConnectionType 0-4 (rtl_media).
    choices = [re.fullmatch(r'Media(\d) = "[^"]+", (\d)', line)
               for line in data["MediaChoicesENG"]]
    assert all(choices), data["MediaChoicesENG"]
    assert [(c[1], c[2]) for c in choices] == [(str(i), str(i)) for i in range(5)]
    # Configure writes only the user options; the driver needs a reboot.
    configure = text[text.index("configureadapter = +"):text.index("configuredone = +")]
    for value in ("{ConnectionType,$(NoTitle),$(!REG_VT_DWORD),$(Media)}",
                  "{VlanId,$(NoTitle),$(!REG_VT_DWORD),$(VlanId)}",
                  "{NetworkAddress,$(NoTitle),$(!REG_VT_SZ),$(NetworkAddress)}",
                  'DeleteRegValue $(KeyParameters) "NetworkAddress"',
                  "ifint $(VlanId) > 4094", "ifint $(Count) != 12",
                  "set CommonStatus = STATUS_REBOOT"):
        assert value in configure, value
    for name in ("BusType", "BusNumber", "SlotNumber", "MediaType"):
        assert "{" + name + "," not in configure, name
    for section in re.findall(r'install "([^"]+)"', text):
        assert section in data, section
    # Instance parameters the driver and NDIS read (see read_settings()).
    for value in ("{BusType,$(NoTitle),$(!REG_VT_DWORD),5}",
                  "{BusNumber,$(NoTitle),$(!REG_VT_DWORD),$(Bus)}",
                  "{SlotNumber,$(NoTitle),$(!REG_VT_DWORD),$(Slot)}",
                  "{MediaType,$(NoTitle),$(!REG_VT_DWORD),1}",
                  "{ConnectionType,$(NoTitle),$(!REG_VT_DWORD),0}",
                  "{VlanId,$(NoTitle),$(!REG_VT_DWORD),0}"):
        assert value in text, value
    # SlotNumber is the plain device number from GetPCIInformation (the
    # vendor convention; NT setup arithmetic commands are not relied on).
    assert "set Slot     = *($($),2)" in text
    assert "set-mul" not in text and "set-add Slot" not in text
    assert "IsNetCardAlreadyInstalled, $(Bus), $(Slot)," in text
    print(f"{path}: NCPA script structure, net rules and parameters passed")


def check_win95_driver():
    path = pathlib.Path("packaging/win95/r8139cp.inf")
    raw = path.read_bytes()
    assert raw.count(b"\n") == raw.count(b"\r\n"), path
    data = sections(path)
    for lines in data.values():
        for line in lines:
            match = re.match(r"(?:CopyFiles|AddReg|DelReg)=(.+)", line, re.I)
            if match:
                for ref in match[1].split(","):
                    assert ref.strip().lower() in data, ref
    assert data["version"][:3] == [
        'Signature="$CHICAGO$"', "Class=Net",
        "ClassGUID={4d36e972-e325-11ce-bfc1-08002be10318}"]
    assert data["adapters"] == [r"%Description%=RTL8139CP.ndi,PCI\VEN_10EC&DEV_8139"]
    assert data["rtl8139cp.files"] == ["r8139cp.sys"]
    assert data["sourcedisksfiles"] == ["r8139cp.sys=1"]
    reg = data["rtl8139cp.ndi.reg"]
    assert 'HKR,Ndi\\Install,ndis3,,"RTL8139CP.ndis3"' in reg
    assert 'HKR,Ndi\\Interfaces,LowerRange,,"ethernet"' in reg
    for value in ("BusType,,5", "AdapterType,,5", "MediaType,,1"):
        assert "HKR,," + value in reg, value
    # As in Windows 95's own INFs: loader values in the device's AddReg (set
    # from the ndis3 install section they ended up empty), install section
    # only copies files.
    assert reg[:2] == ['HKR,,DevLoader,,"*ndis"', 'HKR,,DeviceVxDs,,"r8139cp.sys"']
    assert 'HKR,Ndi,DeviceID,,"PCI\\VEN_10EC&DEV_8139"' in reg
    assert data["rtl8139cp.ndis3"] == ["CopyFiles=RTL8139CP.Files"]
    params = data["rtl8139cp.params.reg"]
    enum = [line for line in params if "ConnectionType\\enum" in line]
    assert [line.split(",")[2] for line in enum] == [f'"{i}"' for i in range(5)]
    assert 'HKR,Ndi\\params\\VlanId,max,,"4094"' in params
    assert 'HKR,Ndi\\params\\VlanId,min,,"0"' in params
    assert 'HKR,,VlanId,,"0"' in params and 'HKR,,ConnectionType,,"0"' in params
    strings = dict(line.split("=", 1) for line in data["strings"])
    for lines in data.values():
        for name in re.findall(r"%(\w+)%", "\n".join(lines)):
            assert name in strings, name
    print(f"{path}: references, PCI ID, ndis3 install and parameters passed")


def check_driver_dist():
    for target, inf in (("nt4", "OEMSETUP.INF"), ("win95", "r8139cp.inf")):
        directory = pathlib.Path(f"dist/{target}")
        for relative, original in (
            ("r8139cp.sys", pathlib.Path("build/r8139cp.sys")),
            (inf, pathlib.Path(f"packaging/{target}/{inf}")),
            ("README.md", pathlib.Path("README.md")),
            ("LICENSE", pathlib.Path("LICENSE")),
            ("docs/DRIVER.md", pathlib.Path("docs/DRIVER.md")),
            ("docs/PROVENANCE.md", pathlib.Path("docs/PROVENANCE.md")),
        ):
            assert (directory / relative).read_bytes() == original.read_bytes(), \
                (directory, relative)
        for link in re.findall(r"\]\(([^)]+)\)", (directory / "README.md").read_text()):
            if not link.startswith(("http:", "https:")):
                assert (directory / link).is_file(), link


def check_floppy(image):
    import subprocess
    import tempfile
    assert image.stat().st_size == 1474560, image
    # Not bootable: FAT12 jump/BPB kept (NT's FAT driver checks the jump
    # byte), then src/boot/floppy.S, no partition table, no 0x55AA.
    boot = image.read_bytes()[:512]
    assert boot[:3] == b"\xeb\x3c\x90" and boot[0x36:0x3e] == b"FAT12   ", image
    assert boot[0x3e:] == pathlib.Path("build/floppy-boot.bin").read_bytes()[:450]
    assert boot[0x1be:] == bytes(66), "partition table or boot signature present"
    # Installer and driver files must be plain 8.3 names for DOS/Windows
    # setup; documentation may use long names.
    expected = {
        "NT4/OEMSETUP.INF": "dist/nt4/OEMSETUP.INF",
        "NT4/R8139CP.SYS": "dist/nt4/r8139cp.sys",
        "WIN95/R8139CP.INF": "dist/win95/r8139cp.inf",
        "WIN95/R8139CP.SYS": "dist/win95/r8139cp.sys",
    }
    for name in expected:
        for part in name.split("/"):
            base, _, ext = part.partition(".")
            assert 1 <= len(base) <= 8 and len(ext) <= 3, name
    listing = subprocess.run(["mdir", "-i", str(image), "-/", "-a", "::/"],
                             check=True, capture_output=True, text=True).stdout
    for name in expected:
        directory, base = name.split("/")
        stem, ext = base.split(".")
        # mdir shows a long name column only when one is stored.
        line = next(line for line in listing.splitlines()
                    if [w.upper() for w in line.split()[:2]] == [stem, ext])
        assert len(line.split()) == 5, line
    with tempfile.TemporaryDirectory() as tmp:
        for name, original in {**expected,
                               "README.md": "README.md", "LICENSE": "LICENSE",
                               "DOCS/DRIVER.md": "docs/DRIVER.md",
                               "DOCS/PROVENANCE.md": "docs/PROVENANCE.md"}.items():
            out = pathlib.Path(tmp) / "f"
            subprocess.run(["mcopy", "-n", "-o", "-i", str(image), f"::/{name}",
                            str(out)], check=True)
            assert out.read_bytes() == pathlib.Path(original).read_bytes(), name
    print(f"{image}: 1.44 MB FAT12, not bootable, 8.3 installer names, "
          "contents match dist")


if __name__ == "__main__":
    main()
