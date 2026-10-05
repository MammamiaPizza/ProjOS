# Build and Use — Single Path

Follow these steps in order. Do not skip any. Each step says what it does and why.

---

## PART A — BUILD (do this once)

### Step 1. Open PowerShell

**Purpose:** PowerShell is the Windows shell. You need it to enter Linux.

Press the Windows key, type `PowerShell`, press Enter.

---

### Step 2. Enter the Ubuntu Linux environment

**Purpose:** The build tools (`make`, `gcc`) live inside Linux, not Windows. This step crosses that
boundary. Without it you get `make: not recognized`.

```powershell
wsl -d Ubuntu
```

You must use `-d Ubuntu`. Your computer has two WSL systems, and the default one
(`docker-desktop`) has no build tools.

**How you know it worked:** your prompt changes to something like:

```
pizza@LaptopOfPizza:~$
```

---

### Step 3. Go to the project folder

**Purpose:** Move to where the source code lives, so the build can find it.

```bash
cd /mnt/d/Code/OS_Tet/proj
```

The path is `/mnt/d/...` because inside Linux your Windows `D:` drive is called `/mnt/d`.

**How you know it worked:** type `ls` and press Enter. You should see:

```
command_history_tool.c   Makefile   README.md   QUICK_START.md   IMPLEMENTATION_SUMMARY.md
```

---

### Step 4. Build the program

**Purpose:** Turn the C source code into a runnable program called `history_tool`.

```bash
make
```

**How you know it worked:** you see these two lines:

```
gcc -O2 -Wall -Wextra -std=c99 command_history_tool.c -o history_tool
Build complete: ./history_tool
```

**If you see `make: Nothing to be done for 'all'`** — that is also success. It means the program is
already built and the source has not changed. Nothing is wrong. Continue to Step 5.

---

## PART B — USE (do this every time)

### Step 5. Start the program

**Purpose:** Launch the tool so you can type commands into it.

```bash
./history_tool
```

The `./` means "the file in this folder". It is required.

**How you know it worked:** you see the banner, then a prompt ending in `>`:

```
=== Enhanced Command History & Replay Tool ===
Features: Dynamic Memory | Better Parsing | Process Groups | CPU Time
          Environment Context | Session Tracking | Security | Terminal Control

[Loaded 3 entries from history.log]
/mnt/d/Code/OS_Tet/proj>
```

**You are now inside the tool.** Every command below is typed at that `>` prompt, not in Linux.

---

### Step 6. Run a normal command

**Purpose:** Confirm the tool executes commands and records them.

```
echo hello world
```

You should see:

```
hello world
[Completed] exit=0 wall=4.12ms cpu_user=0.00ms cpu_sys=3.98ms pid=1523
```

That `[Completed]` line reports: the command succeeded (`exit=0`), how long it took in real time
(`wall`), how much processor time it used (`cpu_user`, `cpu_sys`), and which process ran it (`pid`).

---

### Step 7. Test the arrow-key history

**Purpose:** This is the terminal-control feature. It lets you reuse a past command without
retyping it.

Press the **Up arrow** key once.

The command you just ran appears on the prompt line. Press Up again for older commands.
Press **Down arrow** to move forward again.

---

### Step 8. View your history

**Purpose:** See every recorded command with its timing and exit code.

```
history
```

Each row is one command, on a single line. Note the ID number in the first column — you need it
for Step 9.

---

### Step 9. Replay a command

**Purpose:** Re-run a past command by its ID number. This is the main feature of the project.

```
replay 1
```

The tool re-runs that command. If the command originally ran in a different folder, the tool
changes to that folder first, then changes back afterwards.

---

### Step 10. Try the other built-in commands

**Purpose:** Each of these is a feature you will show in your demo.

| Type this | What it does |
|---|---|
| `search echo` | Finds every past command containing the word "echo" |
| `stats` | Shows most-used command, slowest command, success rate |
| `info` | Shows this session's process ID, session ID, and folder |
| `cd /tmp` | Changes folder (the tool tracks this per command) |
| `pwd` | Shows the current folder |

---

### Step 11. Test the security feature

**Purpose:** Show that dangerous commands are blocked. Good demo moment.

```
rm -rf /
```

You should see:

```
[Security] Command blocked: matches dangerous pattern 'rm -rf /'
```

Nothing is deleted. The command never runs.

---

### Step 12. Test quote handling

**Purpose:** Show the improved parser. Plain shells split on spaces; this tool understands quotes.

```
echo "hello world"
```

Output is `hello world` as one argument, not two.

---

### Step 13. Test CPU time versus real time

**Purpose:** Show the difference between elapsed time and processor time.

```
sleep 2
```

You will see `wall` around 2000ms but `cpu_user` near 0ms. The command spent two seconds waiting,
not computing. This distinction is the point of the CPU-tracking feature.

---

### Step 14. Exit the tool

**Purpose:** Leave the program and return to Linux.

```
exit
```

You are back at the Linux `$` prompt. Your history is saved in `history.log` and will still be
there next time you run the tool.

---

## PART C — REBUILD AFTER EDITING THE CODE

### Step 15. Rebuild

**Purpose:** If you change `command_history_tool.c`, you must rebuild, or you will still be running
the old program.

Repeat Steps 1 through 4.

If `make` says `Nothing to be done` even though you edited the file, force a fresh build:

```bash
make clean
make
```

`make clean` deletes the old program so `make` has to build a new one.

---

## TROUBLESHOOTING

**`make: The term 'make' is not recognized`**
You are in PowerShell. Do Step 2 first (`wsl -d Ubuntu`).

**`wsl -d Ubuntu` errors or opens somewhere strange**
From PowerShell, list your Linux systems:
```powershell
wsl --list --verbose
```
Use whichever name is a real distro.

**`bash: ./history_tool: No such file or directory`**
The build did not happen. Do Step 4 and read any error messages.

**`make: Nothing to be done for 'all'`**
Not an error. Already built. Go to Step 5.

**`Makefile:21: *** missing separator`**
The Makefile has Windows line endings. Fix:
```bash
sed -i 's/\r$//' Makefile
```
Then `make` again.

**Arrow keys do nothing**
They only work when you run the tool yourself in a real terminal window (Step 5). They do not work
if input is piped in from a file.

**Prompt appears stuck with no `>`**
Press Enter once. If still stuck, press `Ctrl+C`, then type `exit`.

---

## FILES IN THIS FOLDER

| File | What it is |
|---|---|
| `command_history_tool.c` | The source code (about 900 lines) |
| `Makefile` | Build instructions used by `make` |
| `QUICK_START.md` | This file — how to build and use |
| `README.md` | Full feature documentation |
| `IMPLEMENTATION_SUMMARY.md` | Technical detail of all 8 improvements, for your report |
| `history.log` | Created at runtime — your recorded command history |
| `history_tool` | Created by the build — the runnable program |
