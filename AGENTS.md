# Working on ios-kebab (iOS 16 on the OnePlus 8T)

- Read `NOTES.md` before starting; it contains the project goal, hardware facts,
  current blocker, local artifact locations and the next concrete action.
- The user switches agents frequently. Update the active handoff in `NOTES.md`
  after each milestone and before stopping. Record commits, CI run IDs, exact
  build/boot results and unfinished work. Distinguish observations from guesses.
- Commit completed changes. Use `gh` for GitHub Actions kernel builds and logs.
  Local single-file cross-compiles are useful before spending a CI run.
- **The OnePlus 8T Android installation is preserved.** Everything boots with
  temporary `fastboot boot` images and a RAM (or later USB) root; userdata is
  NEVER flashed, repartitioned or mounted by iOS. Keep bootloaders, recovery,
  modem, persist and the backups intact, and confirm each destructive step
  with the user before running it.
- **Naming:** this repo targets the OnePlus 8T (`kebab`, SM8250). The XNU
  machine config is still called SWEET (the port codename from the Redmi
  Note 10 Pro origin; the kernel patch series is shared). Kernel boot-args
  (`sweetfb_*`, `sweettouch`, `sweetusb`, `sweetlog`, ...) keep their names
  for the same reason. New device code for the 8T goes into
  `xnu/patches/0082-*` (staged from `drivers/`).
- The QEMU sample (`scripts/mk-qemu-image.sh`, `qemu-img/run.sh`) must build
  and boot after shim, demo or tool changes: run it before committing those.
- Keep device dumps, serial numbers and downloaded Apple artifacts out of git.
