# Porting the mod to a new Dofus build

Offline tools for the day Dofus changes and the mod refuses to load. They read
the game's files and change the repository; they never touch a running game.

Their output describes the game's code. Keep it **outside the repository and never
publish it**. They need Python 3 and metadata version 39: `il2cpp_dump.py` stops if
the version or the layouts it looks for change, and its offsets must then be updated.

## Steps

1. Dump the new build, outside the repository:

   ```
   python tools/port/il2cpp_dump.py <game folder> <work>/new.json
   ```

2. Propose where each guarded address moved:

   ```
   python tools/port/port_map.py . <game folder> <work>/new.json <work>/map.json
   ```

   The tool only proposes addresses backed by the source's method name, the table's
   identity comment, or neighbouring addresses shifted by the same amount. Read
   these parts of `map.json`:

   - `review`: no strong evidence. Interior call sites and unnamed helpers end up
     here. Decide each one with a disassembler; `suggestion` is only a starting point.
   - `collisions`: two old addresses on one new address. One of them is usually wrong.
   - `weak`: a proposal whose 16 bytes differ a lot. Check it.

3. Write your decisions in `<work>/overrides.json`, as `{ "0xOLD": "0xNEW" }`. Add
   what no guard row holds: the data addresses in `src/core/runtime.cpp`, and the
   constants the tests derive from an address (a return address, a near miss).
4. Apply:

   ```
   python tools/port/port_apply.py . <game folder> <work>/map.json <work>/overrides.json
   ```

   This rewrites the addresses in `src/` and `tests/`, the guard table and the pinned
   hashes. Nothing changes if a referenced guard has no new address.
5. Update the client version by hand: `src/core/release-policy.hpp`,
   `src/core/module.cpp`, `src/loader/launcher.cpp`, `src/ui/options-ui.cpp`, the
   README badges, `docs/GUIDE.md` and `package/LISEZ-MOI.txt`.
6. Check against the build. This must pass, with every field warning explained:

   ```
   python tools/port/port_check.py . <game folder> <work>/new.json
   ```

7. Build with `build.ps1` and run the tests. Fix a failure at its cause: never relax
   a guard, a hash or an ABI check to get through.
8. Load it once in the real game, on an explicit request, before publishing: these
   tools validate nothing live.

Field offsets: `port_check.py` compares them with the dump, but a name found only in
a generic class has no fixed offset there, so those stay warnings to read.
