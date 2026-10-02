SudoVDA virtual display driver
==============================

This folder contains prebuilt third-party files that Shell installs to provide its
virtual display. They are redistributed unmodified.

SudoVDA (SudoVDA.dll, SudoVDA.inf, sudovda.cat, sudovda.cer)
  Driver version 1.10.9.289, by SudoMaker.
  Source and license: https://github.com/SudoMaker/SudoVDA
  License (from the upstream README): MIT and CC0 or Public Domain; choose the least
  restrictive one that applies.
  The driver package is signed with SudoMaker's self-signed certificate (sudovda.cer).
  install.bat adds that certificate to the Local Machine "Trusted Root Certification
  Authorities" and "Trusted Publishers" stores so Windows accepts the driver.

nefcon (nefconc.exe)
  Version 1.12.0.0, by Nefarius Software Solutions e.U.
  Source and license: https://github.com/nefarius/nefcon
  License: MIT.
  Used by install.bat and uninstall.bat to create and remove the driver's device node.

install.bat and uninstall.bat are part of Shell (GPL-3.0, see the repository LICENSE).
